#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/pki.hpp"

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
    context.instance_kek = "0123456789abcdef0123456789abcdef";

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
}

} // namespace
