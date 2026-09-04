#include <gtest/gtest.h>

#include <cstdlib>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/callers.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

class StubTokenReviewer final : public lake::control::callers::TokenReviewer {
public:
    lake::control::callers::Result<lake::control::callers::TokenReview>
    review(std::string_view token, const std::vector<std::string>& audiences) override {
        if (token != "projected-token" || audiences != std::vector<std::string>{"lake"}) {
            return std::unexpected(
                lake::control::callers::Error{"unauthorized", "unexpected token"});
        }
        return lake::control::callers::TokenReview{true, "payments", "api", {"lake"}};
    }
};

class StubJwtVerifier final : public lake::control::callers::JwtVerifier {
public:
    lake::control::callers::Result<lake::control::callers::VerifiedJwt>
    verify(std::string_view token) override {
        if (token != "external-jwt") {
            return std::unexpected(lake::control::callers::Error{"unauthorized", "unexpected JWT"});
        }
        return lake::control::callers::VerifiedJwt{
            "https://issuer.example.test", "service-account", {{"namespace", "payments"}}};
    }
};

class StubCloudVerifier final : public lake::control::callers::CloudIdentityVerifier {
public:
    lake::control::callers::Result<lake::control::callers::VerifiedCloudIdentity>
    verify(std::string_view provider, std::string_view document) override {
        if (document != "signed-document") {
            return std::unexpected(
                lake::control::callers::Error{"unauthorized", "unexpected evidence"});
        }
        return lake::control::callers::VerifiedCloudIdentity{
            std::string(provider), "instance-123", {}};
    }
};

TEST(Callers, KubernetesTokenReviewExchangesWithoutBootstrapSecret) {
    StubTokenReviewer reviewer;
    StubJwtVerifier jwt;
    StubCloudVerifier cloud;
    lake::control::callers::InMemoryFederationIssuerPolicy policy;
    lake::control::callers::Service service(&reviewer, &jwt, &cloud, &policy);

    auto credential = service.exchange_kubernetes(
        "organization", "projected-token", "taap:1:ed25519:workload", {"lake"});
    ASSERT_TRUE(credential) << credential.error().message;
    EXPECT_EQ(credential->attestation_type, "kubernetes");
    EXPECT_EQ(credential->subject, "system:serviceaccount:payments:api");
    EXPECT_FALSE(credential->value.empty());
}

TEST(Callers, FederationRequiresEveryClaimToBeAllowed) {
    StubTokenReviewer reviewer;
    StubJwtVerifier jwt;
    StubCloudVerifier cloud;
    lake::control::callers::InMemoryFederationIssuerPolicy policy;
    lake::control::callers::Service service(&reviewer, &jwt, &cloud, &policy);

    EXPECT_FALSE(service.exchange_jwt("organization", "external-jwt", "taap:1:ed25519:workload"));
    policy.allow("organization", "https://issuer.example.test", "namespace");
    EXPECT_TRUE(service.exchange_jwt("organization", "external-jwt", "taap:1:ed25519:workload"));
}

TEST(Callers, AwsAndAzureIdentityEvidenceAreBoundToWorkloadIdentity) {
    StubTokenReviewer reviewer;
    StubJwtVerifier jwt;
    StubCloudVerifier cloud;
    lake::control::callers::InMemoryFederationIssuerPolicy policy;
    lake::control::callers::Service service(&reviewer, &jwt, &cloud, &policy);

    auto aws = service.exchange_cloud("aws", "signed-document", "taap:1:ed25519:workload");
    ASSERT_TRUE(aws) << aws.error().message;
    EXPECT_EQ(aws->subject, "instance-123");
    EXPECT_TRUE(service.exchange_cloud("azure", "signed-document", "taap:1:ed25519:workload"));
}

TEST(Callers, RegistryRecordsFederatedAndCloudExchanges) {
    StubTokenReviewer reviewer;
    StubJwtVerifier jwt;
    StubCloudVerifier cloud;
    lake::control::callers::InMemoryFederationIssuerPolicy policy;
    policy.allow("organization", "https://issuer.example.test", "namespace");
    lake::control::callers::Service callers(&reviewer, &jwt, &cloud, &policy);
    lake::control::InMemoryStore store;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.callers = &callers;
    const auto organization =
        store.create_organization({"taap:1:ed25519:caller-registry", "Callers", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store.create_customer({"organization", "", organization->id, "caller-registry"});
    ASSERT_TRUE(customer) << customer.error().message;

    const auto federation = registry.call("exchange_jwt_attestation",
                                          {{"customer_id", customer->id},
                                           {"organization_id", "organization"},
                                           {"token", "external-jwt"},
                                           {"workload_taap_id", "taap:1:ed25519:workload"}},
                                          context);
    ASSERT_TRUE(federation.ok) << federation.error.message;
    context.request_id.clear();
    const auto aws = registry.call("exchange_cloud_attestation",
                                   {{"customer_id", customer->id},
                                    {"provider", "aws"},
                                    {"document", "signed-document"},
                                    {"workload_taap_id", "taap:1:ed25519:workload"}},
                                   context);
    ASSERT_TRUE(aws.ok) << aws.error.message;
    const auto event = store.latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.kind, "authentication");
    EXPECT_EQ(event->draft.action, "exchange_cloud_attestation");
    const auto records = store.list_pki_callers(customer->id, 10);
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records->size(), 2);
    EXPECT_EQ(records->front().id, aws.value.at("caller_id").get<std::string>());
    EXPECT_EQ(records->front().attestation_type, "aws");
    EXPECT_EQ(records->front().subject, "instance-123");
    EXPECT_FALSE(records->front().attributes.contains("credential"));
}

TEST(PostgresCallers, PersistsKubernetesCredentialMetadata) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    StubTokenReviewer reviewer;
    StubJwtVerifier jwt;
    StubCloudVerifier cloud;
    lake::control::callers::InMemoryFederationIssuerPolicy policy;
    lake::control::callers::Service callers(&reviewer, &jwt, &cloud, &policy);
    auto store = lake::control::make_postgres_store(uri);
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization =
        store->create_organization({"taap:1:ed25519:caller-pg-" + suffix, "Caller PostgreSQL", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store->create_customer({"organization", "", organization->id, "caller-pg-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = store.get();
    context.callers = &callers;
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    const auto exchange = registry.call("exchange_kubernetes_attestation",
                                        {{"customer_id", customer->id},
                                         {"organization_id", organization->id},
                                         {"token", "projected-token"},
                                         {"workload_taap_id", "taap:1:ed25519:workload"},
                                         {"audiences", {"lake"}}},
                                        context);
    ASSERT_TRUE(exchange.ok) << exchange.error.message;
    const auto records = store->list_pki_callers(customer->id, 10);
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records->size(), 1);
    EXPECT_EQ(records->front().id, exchange.value.at("caller_id").get<std::string>());
    EXPECT_EQ(records->front().issuer, "kubernetes");
    EXPECT_EQ(records->front().attributes.at("namespace"), "payments");
    EXPECT_EQ(
        records->front().attributes.dump().find(exchange.value.at("credential").get<std::string>()),
        std::string::npos);
}

} // namespace
