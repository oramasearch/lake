#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

TEST(Pki, IssuesAndVerifiesSpiffeX509AndJwtSvids) {
    auto authority = lake::control::pki::create_authority("customer-a");
    ASSERT_TRUE(authority) << authority.error().message;

    auto x509 = lake::control::pki::issue_x509_svid(*authority, "spiffe://lake.test/worker");
    ASSERT_TRUE(x509) << x509.error().message;
    EXPECT_TRUE(lake::control::pki::verify_x509_svid(*authority, *x509))
        << lake::control::pki::verify_x509_svid(*authority, *x509).error().message;

    auto jwt = lake::control::pki::issue_jwt_svid(*authority, "spiffe://lake.test/worker", "lake");
    ASSERT_TRUE(jwt) << jwt.error().message;
    EXPECT_TRUE(lake::control::pki::verify_jwt_svid(*authority, *jwt))
        << lake::control::pki::verify_jwt_svid(*authority, *jwt).error().message;
}

TEST(Pki, ShamirRecoveryRequiresConfiguredQuorum) {
    const std::string root = "customer root material";
    std::vector<std::byte> secret;
    for (const char character : root) {
        secret.push_back(static_cast<std::byte>(character));
    }
    auto shares = lake::control::pki::split_secret(secret, 3, 5);
    ASSERT_TRUE(shares) << shares.error().message;

    std::vector<lake::control::pki::ShamirShare> short_quorum{shares->at(0), shares->at(1)};
    EXPECT_FALSE(lake::control::pki::recover_secret(short_quorum, 3));

    std::vector<lake::control::pki::ShamirShare> quorum{
        shares->at(0), shares->at(2), shares->at(4)};
    auto recovered = lake::control::pki::recover_secret(quorum, 3);
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(*recovered, secret);
}

TEST(Pki, RegistryIssuesSvidAndRecordsAuditEvent) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::control::InMemoryStore store;
    lake::control::pki::Service pki;
    lake::api::ApiContext context;
    context.store = &store;
    context.pki = &pki;
    context.authorized = true;
    context.acting_identity = "admin";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    const auto organization =
        store.create_organization({"taap:1:ed25519:organization", "Organization", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store.create_customer({"organization", "", organization->id, "pki-customer"});
    ASSERT_TRUE(customer) << customer.error().message;

    const auto authority =
        registry.call("create_pki_authority", {{"customer_id", customer->id}}, context);
    ASSERT_TRUE(authority.ok) << authority.error.message;
    context.request_id.clear();
    const auto svid =
        registry.call("issue_x509_svid",
                      {{"customer_id", customer->id}, {"spiffe_id", "spiffe://lake.test/api"}},
                      context);
    ASSERT_TRUE(svid.ok) << svid.error.message;
    const auto event = store.latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.kind, "certificate");
    EXPECT_EQ(event->draft.action, "issue_x509_svid");

    context.request_id.clear();
    const auto ceremony =
        registry.call("begin_root_recovery",
                      {{"customer_id", customer->id}, {"threshold", 2}, {"share_count", 3}},
                      context);
    ASSERT_TRUE(ceremony.ok) << ceremony.error.message;
    const auto recovery = registry.call(
        "complete_root_recovery",
        {{"customer_id", customer->id},
         {"ceremony_id", ceremony.value.at("ceremony_id")},
         {"threshold", 2},
         {"shares", {ceremony.value.at("shares").at(0), ceremony.value.at("shares").at(1)}}},
        context);
    ASSERT_TRUE(recovery.ok) << recovery.error.message;
    const auto recovery_event = store.latest_audit_event();
    ASSERT_TRUE(recovery_event) << recovery_event.error().message;
    EXPECT_EQ(recovery_event->draft.kind, "recovery");
    EXPECT_EQ(recovery_event->draft.action, "complete_root_recovery");
}

TEST(Pki, PersistsAuthorityKeysThroughTheCustomerKeystore) {
    lake::control::InMemoryStore store;
    const auto organization =
        store.create_organization({"taap:1:ed25519:pki-persistence", "PKI persistence", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store.create_customer({"organization", "", organization->id, "pki-persistence"});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    lake::control::pki::Service creator;
    ASSERT_TRUE(creator.create_authority(store, customer->id, instance_kek));

    lake::control::pki::Service after_restart;
    auto issued = after_restart.issue_x509_svid(
        store, customer->id, "spiffe://lake.test/persisted", instance_kek);
    ASSERT_TRUE(issued) << issued.error().message;
    ASSERT_TRUE(store.rotate_customer_kek(customer->id, instance_kek));
    EXPECT_TRUE(after_restart.issue_jwt_svid(
        store, customer->id, "spiffe://lake.test/persisted", "lake", instance_kek));
}

TEST(PostgresPki, PersistsAndRewrapsCustomerAuthority) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri);
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization =
        store->create_organization({"taap:1:ed25519:pki-postgres-" + suffix, "PKI PostgreSQL", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store->create_customer({"organization", "", organization->id, "pki-pg-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    lake::control::pki::Service creator;
    ASSERT_TRUE(creator.create_authority(*store, customer->id, instance_kek));
    lake::control::pki::Service after_restart;
    auto svid = after_restart.issue_x509_svid(
        *store, customer->id, "spiffe://lake.test/postgres", instance_kek);
    ASSERT_TRUE(svid) << svid.error().message;
    const auto authority = store->get_pki_authority(customer->id, instance_kek);
    ASSERT_TRUE(authority) << authority.error().message;
    EXPECT_TRUE(lake::control::pki::verify_x509_svid({authority->root_certificate_pem,
                                                      authority->intermediate_chain_pem,
                                                      authority->root_private_key_pem,
                                                      authority->intermediate_private_key_pem},
                                                     *svid));
    ASSERT_TRUE(store->rotate_customer_kek(customer->id, instance_kek));
    EXPECT_TRUE(after_restart.issue_jwt_svid(
        *store, customer->id, "spiffe://lake.test/postgres", "lake", instance_kek));
}

} // namespace
