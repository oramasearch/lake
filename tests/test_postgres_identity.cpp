#include <array>
#include <cstdlib>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <nlohmann/json.hpp>

#include "lake/control/postgres_store.hpp"

namespace {

std::string base64(const unsigned char *bytes, std::size_t size) {
  std::string encoded(((size + 2) / 3) * 4, '\0');
  const int encoded_size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), bytes,
                      static_cast<int>(size));
  encoded.resize(static_cast<std::size_t>(encoded_size));
  return encoded;
}

std::string signed_identity_document(std::string_view label) {
  EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  EXPECT_NE(key_context, nullptr);
  EXPECT_EQ(EVP_PKEY_keygen_init(key_context), 1);
  EVP_PKEY *key = nullptr;
  EXPECT_EQ(EVP_PKEY_keygen(key_context, &key), 1);
  EVP_PKEY_CTX_free(key_context);

  std::array<unsigned char, 32> public_key{};
  std::size_t public_key_size = public_key.size();
  EXPECT_EQ(
      EVP_PKEY_get_raw_public_key(key, public_key.data(), &public_key_size), 1);
  const nlohmann::json payload = {
      {"id", "taap:1:ed25519:postgres-" + std::to_string(getpid()) + "-" +
                 std::string(label)},
      {"public_key", base64(public_key.data(), public_key_size)},
  };
  const std::string canonical_payload = payload.dump();
  EVP_MD_CTX *signature_context = EVP_MD_CTX_new();
  EXPECT_EQ(
      EVP_DigestSignInit(signature_context, nullptr, nullptr, nullptr, key), 1);
  std::array<unsigned char, 64> signature{};
  std::size_t signature_size = signature.size();
  EXPECT_EQ(EVP_DigestSign(signature_context, signature.data(), &signature_size,
                           reinterpret_cast<const unsigned char *>(
                               canonical_payload.data()),
                           canonical_payload.size()),
            1);
  EVP_MD_CTX_free(signature_context);
  EVP_PKEY_free(key);
  return nlohmann::json(
             {{"payload", payload},
              {"signature", base64(signature.data(), signature_size)}})
      .dump();
}

TEST(PostgresIdentity, SessionsTokensAndDeviceRevocation) {
  const char *uri = std::getenv("LAKE_TEST_DB_URI");
  if (uri == nullptr || *uri == '\0') {
    GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
  }

  std::unique_ptr<lake::control::Store> store =
      lake::control::make_postgres_store(uri);
  const std::string email =
      "postgres-identity-" + std::to_string(getpid()) + "@example.test";
  const auto sign_up = store->sign_up_password(
      {"lake", email, "correct horse battery staple", "Postgres identity",
       "integration test", signed_identity_document("password")});
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

  const std::string provider_name =
      "stub-" + std::to_string(static_cast<long long>(getpid()));
  const auto registered =
      store->register_oauth_provider({"lake",
                                      provider_name,
                                      "https://issuer.example.test",
                                      "lake-test",
                                      "oauth-client-secret",
                                      {"openid", "email"}});
  ASSERT_TRUE(registered) << registered.error().message;
  const auto provider = store->get_oauth_provider("lake", provider_name);
  ASSERT_TRUE(provider) << provider.error().message;
  EXPECT_EQ(provider->client_secret, "oauth-client-secret");
  const std::string state = "state-" + std::to_string(getpid());
  const auto started = store->begin_oauth(
      {"lake", provider_name, state, "pkce-verifier", "oidc-nonce",
       "integration browser", signed_identity_document("oauth")});
  ASSERT_TRUE(started) << started.error().message;
  const auto flow = store->consume_oauth(provider_name, state);
  ASSERT_TRUE(flow) << flow.error().message;
  EXPECT_EQ(flow->code_verifier, "pkce-verifier");
  EXPECT_EQ(flow->nonce, "oidc-nonce");
  EXPECT_FALSE(store->consume_oauth(provider_name, state));
  const auto oauth = store->sign_in_oauth(
      {"lake", provider_name, "provider-subject",
       "postgres-oauth-" + std::to_string(getpid()) + "@example.test",
       "Postgres OAuth", "integration browser", flow->identity_document});
  ASSERT_TRUE(oauth) << oauth.error().message;
  const auto oauth_user = store->get_user(oauth->session.user_id);
  ASSERT_TRUE(oauth_user) << oauth_user.error().message;
  EXPECT_FALSE(oauth_user->taap_id.empty());

  const auto revoke =
      store->revoke_device(context->user_id, context->device_id);
  ASSERT_TRUE(revoke) << revoke.error().message;
  EXPECT_FALSE(store->context_for(sign_up->secret));
}

} // namespace
