#include <gtest/gtest.h>

#include <string>
#include <vector>

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

} // namespace
