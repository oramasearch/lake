#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "webauthn_test_support.hpp"

namespace {

using lake::test::CeremonyResponse;
using lake::test::SoftwareAuthenticator;
using nlohmann::json;

lake::api::ApiResult register_response(lake::api::Registry& registry,
                                       lake::api::ApiContext& context,
                                       const json& options,
                                       const SoftwareAuthenticator& authenticator,
                                       std::string_view origin = "https://lake.example") {
    const std::string challenge = options["public_key"]["challenge"];
    const CeremonyResponse response = authenticator.registration(challenge, "lake.example", origin);
    return registry.call("register_passkey",
                         {{"challenge", challenge},
                          {"client_data_json", response.client_data_json},
                          {"attestation_object", response.authenticator_data},
                          {"name", "software passkey"}},
                         context);
}

lake::api::ApiResult sign_in_response(lake::api::Registry& registry,
                                      lake::api::ApiContext& context,
                                      const json& options,
                                      const SoftwareAuthenticator& authenticator,
                                      std::uint32_t sign_count) {
    const std::string challenge = options["public_key"]["challenge"];
    const CeremonyResponse response =
        authenticator.assertion(challenge, "lake.example", "https://lake.example", sign_count);
    return registry.call("sign_in_passkey",
                         {{"challenge", challenge},
                          {"credential_id", response.credential_id},
                          {"client_data_json", response.client_data_json},
                          {"authenticator_data", response.authenticator_data},
                          {"signature", response.signature},
                          {"device_name", "software authenticator"}},
                         context);
}

} // namespace

TEST(WebAuthn, PersistedRegistrationAssertionAndSignCount) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "0.0.1");
    lake::control::InMemoryStore store;
    const auto user = store.sign_up_password({"lake",
                                              "passkey@example.test",
                                              "correct horse battery staple",
                                              "Passkey User",
                                              "registration browser",
                                              "{}"});
    ASSERT_TRUE(user) << user.error().message;

    lake::api::ApiContext context;
    context.store = &store;
    context.public_host = "lake.example";
    context.pool_id = "lake";
    context.authorized = true;
    const auto principal = store.context_for(user->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;
    context.session_credential = true;
    context.scopes = {"*"};
    SoftwareAuthenticator authenticator;

    auto begun = registry.call("begin_passkey_registration", json::object(), context);
    ASSERT_TRUE(begun.ok) << begun.error.message;
    EXPECT_EQ(begun.value["public_key"]["attestation"], "none");
    const auto wrong_origin =
        register_response(registry, context, begun.value, authenticator, "https://wrong.example");
    EXPECT_FALSE(wrong_origin.ok);
    EXPECT_EQ(wrong_origin.error.code, "unauthorized");
    EXPECT_FALSE(register_response(registry, context, begun.value, authenticator).ok);

    begun = registry.call("begin_passkey_registration", json::object(), context);
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const auto registered = register_response(registry, context, begun.value, authenticator);
    ASSERT_TRUE(registered.ok) << registered.error.message;

    context.authorized = false;
    auto assertion = registry.call("begin_passkey_sign_in", json::object(), context);
    ASSERT_TRUE(assertion.ok) << assertion.error.message;
    const auto signed_in = sign_in_response(registry, context, assertion.value, authenticator, 1);
    ASSERT_TRUE(signed_in.ok) << signed_in.error.message;
    EXPECT_TRUE(store.context_for(signed_in.value["secret"]));
    EXPECT_FALSE(sign_in_response(registry, context, assertion.value, authenticator, 2).ok);

    assertion = registry.call("begin_passkey_sign_in", json::object(), context);
    ASSERT_TRUE(assertion.ok) << assertion.error.message;
    const auto stale = sign_in_response(registry, context, assertion.value, authenticator, 1);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(stale.error.code, "unauthorized");

    assertion = registry.call("begin_passkey_sign_in", json::object(), context);
    ASSERT_TRUE(assertion.ok) << assertion.error.message;
    const auto advanced = sign_in_response(registry, context, assertion.value, authenticator, 2);
    ASSERT_TRUE(advanced.ok) << advanced.error.message;
}

TEST(WebAuthn, RejectsMalformedClientDataWithoutThrowing) {
    const auto verified = lake::control::identity_crypto::verify_passkey_registration(
        lake::test::base64url(
            R"({"type":7,"challenge":"challenge","origin":"https://lake.example"})"),
        "not-an-attestation",
        "challenge",
        "lake.example",
        "https://lake.example");
    EXPECT_FALSE(verified);
}
