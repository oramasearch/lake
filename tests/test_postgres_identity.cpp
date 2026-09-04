#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include <liquid/identity.hpp>
#include <liquid/runtime.hpp>

#include <openssl/evp.h>

#include "lake/control/postgres_store.hpp"
#include "webauthn_test_support.hpp"

namespace {

std::string base64url(const std::vector<std::byte>& bytes) {
    const auto* input = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t size = bytes.size();
    std::string encoded(((size + 2) / 3) * 4, '\0');
    const int encoded_size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), input, static_cast<int>(size));
    encoded.resize(static_cast<std::size_t>(encoded_size));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::string signed_identity_document(std::string_view label) {
    const std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        ("lake-identity-public-api-" + std::to_string(getpid()) + "-" + std::string(label));
    std::error_code error;
    std::filesystem::remove_all(home, error);
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    const std::filesystem::path document_path =
        home / "identity" / ((signer ? signer->identity().fingerprint : "") + ".identity.cbor");
    std::ifstream input(document_path, std::ios::binary);
    std::vector<std::byte> document;
    for (char character; input.get(character);) {
        document.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    const auto decoded = liquid::decode_identity_document(document);
    EXPECT_TRUE(decoded) << decoded.error().message;
    if (decoded) {
        const auto verified = liquid::verify_document(*decoded);
        EXPECT_TRUE(verified && *verified);
    }
    std::filesystem::remove_all(home, error);
    return base64url(document);
}

TEST(PostgresIdentity, SessionsTokensAndDeviceRevocation) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }

    std::unique_ptr<lake::control::Store> store = lake::control::make_postgres_store(uri);
    const std::string email = "postgres-identity-" + std::to_string(getpid()) + "@example.test";
    const auto sign_up = store->sign_up_password({"lake",
                                                  email,
                                                  "correct horse battery staple",
                                                  "Postgres identity",
                                                  "integration test",
                                                  signed_identity_document("password")});
    ASSERT_TRUE(sign_up) << sign_up.error().message;

    const auto context = store->context_for(sign_up->secret);
    ASSERT_TRUE(context) << context.error().message;
    EXPECT_EQ(context->user_id, sign_up->session.user_id);

    const auto token = store->create_token(
        {context->user_id, "lake", "read sessions", "", {"list_sessions"}, 3600, false});
    ASSERT_TRUE(token) << token.error().message;
    EXPECT_TRUE(store->context_for(token->secret));

    const auto enrolled = store->enroll_totp(context->user_id, "lake", "0011223344556677");
    ASSERT_TRUE(enrolled) << enrolled.error().message;
    const auto mfa_required = store->sign_in_password(
        {"lake", email, "correct horse battery staple", "", "integration test"});
    ASSERT_FALSE(mfa_required);
    EXPECT_EQ(mfa_required.error().code, "mfa_required");

    const std::string provider_name = "stub-" + std::to_string(static_cast<long long>(getpid()));
    const auto registered = store->register_oauth_provider({"lake",
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
    const auto started = store->begin_oauth({"lake",
                                             provider_name,
                                             state,
                                             "pkce-verifier",
                                             "oidc-nonce",
                                             "integration browser",
                                             signed_identity_document("oauth")});
    ASSERT_TRUE(started) << started.error().message;
    const auto flow = store->consume_oauth(provider_name, state);
    ASSERT_TRUE(flow) << flow.error().message;
    EXPECT_EQ(flow->code_verifier, "pkce-verifier");
    EXPECT_EQ(flow->nonce, "oidc-nonce");
    EXPECT_FALSE(store->consume_oauth(provider_name, state));
    const auto oauth =
        store->sign_in_oauth({"lake",
                              provider_name,
                              "provider-subject",
                              "postgres-oauth-" + std::to_string(getpid()) + "@example.test",
                              "Postgres OAuth",
                              "integration browser",
                              flow->identity_document});
    ASSERT_TRUE(oauth) << oauth.error().message;
    const auto oauth_user = store->get_user(oauth->session.user_id);
    ASSERT_TRUE(oauth_user) << oauth_user.error().message;
    EXPECT_FALSE(oauth_user->taap_id.empty());

    lake::test::SoftwareAuthenticator authenticator;
    const auto registration = store->begin_passkey_registration(
        context->user_id, "lake", "lake.example", "https://lake.example");
    ASSERT_TRUE(registration) << registration.error().message;
    const auto attestation = authenticator.registration(
        registration->challenge, registration->rp_id, registration->origin);
    const auto passkey = store->register_passkey({context->user_id,
                                                  "lake",
                                                  registration->challenge,
                                                  attestation.client_data_json,
                                                  attestation.authenticator_data,
                                                  "integration passkey"});
    ASSERT_TRUE(passkey) << passkey.error().message;
    EXPECT_FALSE(store->register_passkey({context->user_id,
                                          "lake",
                                          registration->challenge,
                                          attestation.client_data_json,
                                          attestation.authenticator_data,
                                          "integration passkey"}));

    const auto assertion =
        store->begin_passkey_sign_in("lake", "lake.example", "https://lake.example");
    ASSERT_TRUE(assertion) << assertion.error().message;
    const auto signed_assertion =
        authenticator.assertion(assertion->challenge, assertion->rp_id, assertion->origin, 1);
    const auto passkey_session = store->sign_in_passkey({"lake",
                                                         assertion->challenge,
                                                         signed_assertion.credential_id,
                                                         signed_assertion.client_data_json,
                                                         signed_assertion.authenticator_data,
                                                         signed_assertion.signature,
                                                         "integration authenticator"});
    ASSERT_TRUE(passkey_session) << passkey_session.error().message;
    EXPECT_TRUE(store->context_for(passkey_session->secret));
    EXPECT_FALSE(store->sign_in_passkey({"lake",
                                         assertion->challenge,
                                         signed_assertion.credential_id,
                                         signed_assertion.client_data_json,
                                         signed_assertion.authenticator_data,
                                         signed_assertion.signature,
                                         "integration authenticator"}));

    const auto revoke = store->revoke_device(context->user_id, context->device_id);
    ASSERT_TRUE(revoke) << revoke.error().message;
    EXPECT_FALSE(store->context_for(sign_up->secret));
}

} // namespace
