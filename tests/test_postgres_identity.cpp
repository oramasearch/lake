#include <cstdlib>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "lake/control/postgres_store.hpp"

namespace {

TEST(PostgresIdentity, SessionsTokensAndDeviceRevocation) {
  const char *uri = std::getenv("LAKE_TEST_DB_URI");
  if (uri == nullptr || *uri == '\0') {
    GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
  }

  std::unique_ptr<lake::control::Store> store =
      lake::control::make_postgres_store(uri);
  const std::string email =
      "postgres-identity-" + std::to_string(getpid()) + "@example.test";
  const auto sign_up =
      store->sign_up_password({"lake", email, "correct horse battery staple",
                               "Postgres identity", "integration test"});
  ASSERT_TRUE(sign_up) << sign_up.error().message;

  const auto context = store->context_for(sign_up->secret);
  ASSERT_TRUE(context) << context.error().message;
  EXPECT_EQ(context->user_id, sign_up->session.user_id);

  const auto token = store->create_token({context->user_id,
                                          "lake",
                                          "read sessions",
                                          "",
                                          {"list_sessions"},
                                          3600,
                                          false});
  ASSERT_TRUE(token) << token.error().message;
  EXPECT_TRUE(store->context_for(token->secret));

  const auto enrolled =
      store->enroll_totp(context->user_id, "lake", "0011223344556677");
  ASSERT_TRUE(enrolled) << enrolled.error().message;
  const auto mfa_required = store->sign_in_password(
      {"lake", email, "correct horse battery staple", "", "integration test"});
  ASSERT_FALSE(mfa_required);
  EXPECT_EQ(mfa_required.error().code, "mfa_required");

  const auto revoke =
      store->revoke_device(context->user_id, context->device_id);
  ASSERT_TRUE(revoke) << revoke.error().message;
  EXPECT_FALSE(store->context_for(sign_up->secret));
}

} // namespace
