#include <gtest/gtest.h>

#include "lake/control/callers.hpp"

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

} // namespace
