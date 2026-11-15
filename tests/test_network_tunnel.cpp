// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <liquid/runtime.hpp>
#include <pqxx/pqxx>
#include <type_traits>

#include "lake/api/capabilities.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/network_records.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"
#include "tunnel_fixture.hpp"

namespace {
using namespace lake::control;
using nlohmann::json;

template <class Result> auto checked(Result result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    if constexpr (!std::is_void_v<typename Result::value_type>) {
        return std::move(*result);
    }
}

class TunnelCredentials : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            uri = lake::test::postgres_uri();
            if (!uri) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is absent";
            }
            store = make_postgres_store(uri, kek);
        } else {
            store = std::make_shared<InMemoryStore>();
        }
        owner = checked(store->create_user({"taap:tunnel-admin", "admin@example.test", "Admin"}));
        recipient =
            checked(store->create_user({"taap:tunnel-recipient", "dev@example.test", "Dev"}));
        other = checked(store->create_user({"taap:tunnel-other", "other@example.test", "Other"}));
        customer = checked(store->create_customer({"individual", owner.id, "", "tunnels"}));
        const auto object = checked(store->find_policy_object("customer", customer.id));
        for (const auto& [identity, permission] : {std::pair{owner.taap_id, "admin"},
                                                   std::pair{recipient.taap_id, "read"},
                                                   std::pair{other.taap_id, "write"}}) {
            checked(
                store->create_grant({identity, object.id, permission, "test", "admin", "setup"}));
        }
        workspace = checked(store->create_workspace({customer.id, "Tunnel", "dev", owner.taap_id}));
        scope = {customer.id, workspace.id};
        lake::api::register_core_capabilities(registry, "test");
        context.store = store.get();
        context.authorized = true;
        context.instance_kek = kek;
        context.pki = &pki;
    }

    json input() const {
        return {{"customer_id", customer.id}, {"workspace_id", workspace.id}};
    }

    lake::api::ApiResult call(const std::string& name, const json& request, const User& actor) {
        context.acting_identity = actor.taap_id;
        context.acting_user_id = actor.id;
        context.request_id = "tunnel-request-" + std::to_string(++requests);
        context.audit_customer_id.clear();
        context.audit_object.clear();
        context.audit_kind.clear();
        context.audit_details = json::object();
        return checked(liquid::runtime::block_on(registry.call_async(name, request, context)));
    }

    NetworkTunnelGrant grant(std::chrono::hours lifetime = std::chrono::hours(1)) {
        auto request = input();
        request.update({{"user_id", recipient.id},
                        {"target_kind", "service"},
                        {"target_id", workspace.zetta_service_id},
                        {"port", 4800},
                        {"expires_at", iso8601_at(std::chrono::system_clock::now() + lifetime)}});
        const auto result = call("grant_network_tunnel", request, owner);
        if (!result.ok) {
            throw std::runtime_error(result.error.message);
        }
        return checked(store->get_network_tunnel_grant(scope, result.value.at("id")));
    }

    void observe(const NetworkTunnelGrant& grant,
                 std::optional<NetworkStatus> observation = std::nullopt) {
        NetworkStatus status = observation.value_or(NetworkStatus{});
        if (!observation) {
            status.phase = "Ready";
            status.observed_generation = grant.generation;
            status.endpoint = "relay.example.test:7443";
            status.relay_ca_pem = grant.spec.client_ca_pem;
        }
        if (auto* memory = dynamic_cast<InMemoryStore*>(store.get())) {
            checked(memory->set_network_tunnel_grant_status(grant.id, status));
            return;
        }
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO network_tunnel_grant_status "
                       "(grant_id, phase, message, observed_generation, endpoint, relay_ca_pem) "
                       "VALUES ($1::uuid,$2,'',$3,$4,$5) ON CONFLICT (grant_id) DO UPDATE SET "
                       "phase=EXCLUDED.phase, observed_generation=EXCLUDED.observed_generation, "
                       "endpoint=EXCLUDED.endpoint, relay_ca_pem=EXCLUDED.relay_ca_pem",
                       grant.id,
                       status.phase,
                       status.observed_generation,
                       status.endpoint,
                       status.relay_ca_pem);
        tx.commit();
    }

    json issuance(const NetworkTunnelGrant& grant, const std::string& csr) const {
        auto request = input();
        request.update({{"tunnel_grant_id", grant.id}, {"csr_pem", csr}});
        return request;
    }

    const char* uri = nullptr;
    const std::string kek = std::string(64, '3');
    std::shared_ptr<Store> store;
    lake::api::Registry registry;
    lake::api::ApiContext context;
    pki::Service pki;
    User owner, recipient, other;
    Customer customer;
    Workspace workspace;
    NetworkScope scope;
    int requests = 0;
};

TEST_P(TunnelCredentials, SignsOnlyTheGrantIdentityWithRecipientKeyAndBoundedValidity) {
    const auto csr = lake::test::tunnel_csr();
    for (auto lifetime : {std::chrono::hours(1), std::chrono::hours(24)}) {
        const auto created = grant(lifetime);
        EXPECT_FALSE(created.spec.client_ca_pem.empty());
        observe(created);
        const auto response =
            call("issue_network_tunnel_certificate", issuance(created, csr.pem), recipient);
        ASSERT_TRUE(response.ok) << response.error.message;
        EXPECT_EQ(response.value.at("endpoint"), "relay.example.test:7443");
        EXPECT_EQ(response.value.at("relay_ca_pem"), created.spec.client_ca_pem);
        EXPECT_EQ(response.value.dump().find("PRIVATE KEY"), std::string::npos);
        auto certificate = lake::test::tunnel_certificate(response.value.at("certificate_pem"));
        auto issuer = lake::test::tunnel_certificate(response.value.at("chain_pem"));
        lake::test::TunnelKey issuer_key(X509_get_pubkey(issuer.get()), EVP_PKEY_free);
        lake::test::TunnelKey public_key(X509_get_pubkey(certificate.get()), EVP_PKEY_free);
        EXPECT_EQ(EVP_PKEY_eq(public_key.get(), csr.key.get()), 1);
        EXPECT_EQ(X509_verify(certificate.get(), issuer_key.get()), 1);
        EXPECT_EQ(X509_check_ca(certificate.get()), 0);
        EXPECT_EQ(X509_check_purpose(certificate.get(), X509_PURPOSE_SSL_CLIENT, 0), 1);
        EXPECT_EQ(X509_check_purpose(certificate.get(), X509_PURPOSE_SSL_SERVER, 0), 0);
        auto* names = static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, nullptr, nullptr));
        ASSERT_NE(names, nullptr);
        ASSERT_EQ(sk_GENERAL_NAME_num(names), 1);
        const auto* name = sk_GENERAL_NAME_value(names, 0);
        ASSERT_EQ(name->type, GEN_URI);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(
                                  ASN1_STRING_get0_data(name->d.uniformResourceIdentifier)),
                              ASN1_STRING_length(name->d.uniformResourceIdentifier)),
                  "urn:orama:tunnel-grant:" + created.id);
        GENERAL_NAMES_free(names);
        std::tm not_after{};
        std::tm not_before{};
        ASSERT_EQ(ASN1_TIME_to_tm(X509_get0_notAfter(certificate.get()), &not_after), 1);
        ASSERT_EQ(ASN1_TIME_to_tm(X509_get0_notBefore(certificate.get()), &not_before), 1);
        const auto expires = std::chrono::system_clock::from_time_t(timegm(&not_after));
        const auto starts = std::chrono::system_clock::from_time_t(timegm(&not_before));
        EXPECT_EQ(
            expires,
            std::min(
                std::chrono::floor<std::chrono::seconds>(*network_tunnel_grant_expiry(created)),
                std::chrono::floor<std::chrono::seconds>(starts + std::chrono::hours(12))));
        EXPECT_LE(expires, *network_tunnel_grant_expiry(created));
        EXPECT_LE(expires, std::chrono::system_clock::now() + std::chrono::hours(12));
        EXPECT_GT(expires, std::chrono::system_clock::now());
        NetworkTunnelGrant reported = created;
        reported.spec.expires_at = response.value.at("expires_at");
        EXPECT_EQ(network_tunnel_grant_expiry(reported), expires);
    }
    const auto first = grant();
    const auto second = grant();
    EXPECT_EQ(first.spec.client_ca_pem, second.spec.client_ca_pem);
}

TEST_P(TunnelCredentials, RecipientOnlyEvenWhenAnotherCallerIsAnAdminOrOperator) {
    const auto created = grant();
    observe(created);
    const auto csr = lake::test::tunnel_csr();
    const auto request = issuance(created, csr.pem);
    for (const auto& actor : {owner, other}) {
        const auto response = call("issue_network_tunnel_certificate", request, actor);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error.code, "forbidden");
    }
    context.operator_credential = true;
    EXPECT_EQ(call("issue_network_tunnel_certificate", request, recipient).error.code, "forbidden");
    context.operator_credential = false;
    auto forged = recipient;
    forged.taap_id = owner.taap_id;
    EXPECT_EQ(call("issue_network_tunnel_certificate", request, forged).error.code, "forbidden");
    context.authorized = false;
    EXPECT_FALSE(call("issue_network_tunnel_certificate", request, recipient).ok);
    context.authorized = true;
    auto wrong = request;
    wrong["workspace_id"] = new_uuid_v4();
    EXPECT_FALSE(call("issue_network_tunnel_certificate", wrong, recipient).ok);
    wrong = request;
    wrong["customer_id"] = new_uuid_v4();
    EXPECT_FALSE(call("issue_network_tunnel_certificate", wrong, recipient).ok);
    EXPECT_TRUE(call("issue_network_tunnel_certificate", request, recipient).ok);
}

TEST_P(TunnelCredentials, OnlyAdminsGrantAndRevokeAndRevocationRefusesRenewal) {
    const auto created = grant();
    observe(created);
    auto request = input();
    request["tunnel_grant_id"] = created.id;
    for (const auto& actor : {recipient, other}) {
        EXPECT_FALSE(call("revoke_network_tunnel", request, actor).ok);
        auto create = input();
        create.update({{"user_id", recipient.id},
                       {"target_kind", "service"},
                       {"target_id", workspace.zetta_service_id},
                       {"port", 4800},
                       {"expires_at", created.spec.expires_at}});
        EXPECT_FALSE(call("grant_network_tunnel", create, actor).ok);
    }
    const auto csr = lake::test::tunnel_csr();
    EXPECT_TRUE(call("issue_network_tunnel_certificate", issuance(created, csr.pem), recipient).ok);
    const auto revoked = call("revoke_network_tunnel", request, owner);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_EQ(revoked.value.at("generation"), created.generation + 1);
    EXPECT_FALSE(revoked.value.at("revoked_at").is_null());
    EXPECT_FALSE(revoked.value.at("active"));
    EXPECT_EQ(
        call("issue_network_tunnel_certificate", issuance(created, csr.pem), recipient).error.code,
        "conflict");
    EXPECT_EQ(call("revoke_network_tunnel", request, owner).error.code, "conflict");
}

TEST_P(TunnelCredentials, RefusesMissingStaleOrUntrustedRelayStatus) {
    const auto created = grant();
    const auto csr = lake::test::tunnel_csr();
    const auto request = issuance(created, csr.pem);
    EXPECT_EQ(call("issue_network_tunnel_certificate", request, recipient).error.code, "conflict");
    for (int variant = 0; variant < 4; ++variant) {
        NetworkStatus status{.phase = "Ready",
                             .observed_generation = created.generation,
                             .endpoint = "relay.test:7443",
                             .relay_ca_pem = created.spec.client_ca_pem};
        if (variant == 0) {
            status.phase = "Failed";
        } else if (variant == 1) {
            status.observed_generation = 0;
        } else if (variant == 2) {
            status.endpoint.clear();
        } else {
            status.relay_ca_pem.clear();
        }
        observe(created, status);
        EXPECT_EQ(call("issue_network_tunnel_certificate", request, recipient).error.code,
                  "conflict");
    }
    observe(created);
    context.pki = nullptr;
    EXPECT_FALSE(call("issue_network_tunnel_certificate", request, recipient).ok);
}

TEST_P(TunnelCredentials, RefusesMalformedUnownedOrWeakRequests) {
    const auto created = grant();
    observe(created);
    const auto csr = lake::test::tunnel_csr();
    for (const auto& pem : {std::string{},
                            std::string("not a request"),
                            std::string(16385, 'x'),
                            csr.pem + csr.pem,
                            csr.pem + "PRIVATE KEY",
                            lake::test::tunnel_csr(true).pem,
                            lake::test::tunnel_csr(false, true).pem}) {
        const auto response =
            call("issue_network_tunnel_certificate", issuance(created, pem), recipient);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error.code, "invalid_input");
    }
}

TEST_P(TunnelCredentials, ExpiredOrUnpinnedGrantsCannotBeSigned) {
    auto created = grant();
    const auto csr = lake::test::tunnel_csr();
    const auto original = created;
    created.spec.expires_at =
        iso8601_at(std::chrono::system_clock::now() - std::chrono::seconds(1));
    EXPECT_FALSE(network_tunnel_grant_active(created));
    EXPECT_EQ(pki.sign_tunnel_certificate(*store, created, csr.pem, kek).error().code, "conflict");
    created.spec.expires_at = "invalid";
    EXPECT_FALSE(pki.sign_tunnel_certificate(*store, created, csr.pem, kek));
    created = original;
    created.spec.client_ca_pem.clear();
    EXPECT_EQ(pki.sign_tunnel_certificate(*store, created, csr.pem, kek).error().code, "conflict");
    created.spec.client_ca_pem = "another CA";
    EXPECT_FALSE(pki.sign_tunnel_certificate(*store, created, csr.pem, kek));
    if (GetParam()) {
        observe(original);
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE network_tunnel_grants SET expires_at=now()-interval '1 second' "
                       "WHERE id=$1::uuid",
                       original.id);
        tx.commit();
        EXPECT_EQ(call("issue_network_tunnel_certificate", issuance(original, csr.pem), recipient)
                      .error.code,
                  "conflict");
    }
}

TEST_P(TunnelCredentials, OtherPkiProfilesCannotMintTunnelGrantUris) {
    const auto created = grant();
    const auto identity = "urn:orama:tunnel-grant:" + created.id;
    const auto public_key = std::string(43, 'A');
    ASSERT_TRUE(
        pki.issue_agent_certificate(*store, customer.id, "taap:test-agent", public_key, kek));
    const auto direct = pki.issue_agent_certificate(*store, customer.id, identity, public_key, kek);
    ASSERT_FALSE(direct);
    EXPECT_EQ(direct.error().code, "invalid_input");
    const auto injected = pki.issue_agent_certificate(
        *store, customer.id, "taap:test-agent,URI:" + identity, public_key, kek);
    ASSERT_FALSE(injected);
    EXPECT_EQ(injected.error().code, "invalid_input");
    EXPECT_FALSE(
        pki.issue_x509_svid(*store, customer.id, "spiffe://test/agent,URI:" + identity, kek));
    // A generic client subject is only a CN, not a relay grant identity.
    const auto generic = pki.issue_x509_certificate(*store, customer.id, identity, kek);
    ASSERT_TRUE(generic);
    const auto certificate = lake::test::tunnel_certificate(generic->certificate_pem);
    EXPECT_EQ(X509_get_ext_by_NID(certificate.get(), NID_subject_alt_name, -1), -1);
}

TEST_P(TunnelCredentials, AuditRecordsWhoWhatWhenWithoutPrivateMaterial) {
    const auto created = grant();
    observe(created);
    const auto csr = lake::test::tunnel_csr();
    const auto issued =
        call("issue_network_tunnel_certificate", issuance(created, csr.pem), recipient);
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const auto success_request = context.request_id;
    EXPECT_FALSE(call("issue_network_tunnel_certificate", issuance(created, csr.pem), owner).ok);
    const auto failure_request = context.request_id;
    auto revoke = input();
    revoke["tunnel_grant_id"] = created.id;
    ASSERT_TRUE(call("revoke_network_tunnel", revoke, owner).ok);
    const auto events = checked(store->list_audit_events({}));
    ASSERT_TRUE(lake::audit::verify(*store, events));
    bool found_success = false;
    bool found_failure = false;
    bool found_revoke = false;
    for (const auto& event : events) {
        const auto& draft = event.draft;
        EXPECT_FALSE(event.occurred_at.empty());
        const auto details = draft.details.dump();
        EXPECT_EQ(details.find("PRIVATE KEY"), std::string::npos);
        EXPECT_EQ(details.find("CERTIFICATE REQUEST"), std::string::npos);
        if (draft.request_id == success_request) {
            found_success = true;
            EXPECT_EQ(draft.principal, recipient.taap_id);
            EXPECT_EQ(draft.outcome, "success");
            EXPECT_EQ(draft.object, "network_tunnel_grant/" + created.id);
            EXPECT_EQ(draft.details.at("user_id"), recipient.id);
            EXPECT_EQ(draft.details.at("target_id"), workspace.zetta_service_id);
            EXPECT_EQ(draft.details.at("port"), 4800);
            EXPECT_EQ(draft.details.at("expires_at"), issued.value.at("expires_at"));
            EXPECT_EQ(draft.details.at("certificate_sha256").get<std::string>().size(), 64U);
        }
        if (draft.request_id == failure_request) {
            found_failure = true;
            EXPECT_EQ(draft.principal, owner.taap_id);
            EXPECT_NE(draft.outcome, "success");
        }
        found_revoke |= draft.action == "revoke_network_tunnel_grant";
    }
    EXPECT_TRUE(found_success);
    EXPECT_TRUE(found_failure);
    EXPECT_TRUE(found_revoke);
}

INSTANTIATE_TEST_SUITE_P(Stores, TunnelCredentials, testing::Bool());
} // namespace
