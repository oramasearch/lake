#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
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
    return *liquid::runtime::block_on(
        registry.call_async("register_passkey",
                            {{"challenge", challenge},
                             {"client_data_json", response.client_data_json},
                             {"attestation_object", response.authenticator_data},
                             {"name", "software passkey"}},
                            context));
}

lake::api::ApiResult sign_in_response(lake::api::Registry& registry,
                                      lake::api::ApiContext& context,
                                      const json& options,
                                      const SoftwareAuthenticator& authenticator,
                                      std::uint32_t sign_count) {
    const std::string challenge = options["public_key"]["challenge"];
    const CeremonyResponse response =
        authenticator.assertion(challenge, "lake.example", "https://lake.example", sign_count);
    return *liquid::runtime::block_on(
        registry.call_async("sign_in_passkey",
                            {{"challenge", challenge},
                             {"credential_id", response.credential_id},
                             {"client_data_json", response.client_data_json},
                             {"authenticator_data", response.authenticator_data},
                             {"signature", response.signature},
                             {"device_name", "software authenticator"}},
                            context));
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

    const auto devices = registry.call("list_devices", json::object(), context);
    ASSERT_TRUE(devices.ok) << devices.error.message;
    const auto passkey_device = std::ranges::find_if(devices.value["items"], [](const json& item) {
        return !item["passkey_credential_id"].is_null();
    });
    ASSERT_NE(passkey_device, devices.value["items"].end());
    const std::string credential_id = (*passkey_device)["passkey_credential_id"];
    const json binding = (*passkey_device)["passkey_binding_attestation"];
    const auto signing_identity = lake::audit::identity(store, {});
    ASSERT_TRUE(signing_identity) << signing_identity.error().message;
    EXPECT_EQ(binding["authority_identifier"], signing_identity->taap_id);
    ASSERT_TRUE(lake::audit::verify_passkey_binding(
        store, binding, principal->taap_id, credential_id, "lake"));
    json tampered = binding;
    tampered["asserted_attributes"]["credential_id"] = "another-credential";
    EXPECT_FALSE(lake::audit::verify_passkey_binding(
        store, tampered, principal->taap_id, "another-credential", "lake"));
    const auto rotated = lake::audit::rotate_identity(store, {});
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_NE(rotated->taap_id, signing_identity->taap_id);
    EXPECT_TRUE(lake::audit::verify_passkey_binding(
        store, binding, principal->taap_id, credential_id, "lake"));
    const std::string passkey_device_id = (*passkey_device)["id"];

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

    context.authorized = true;
    const auto revoked = registry.call("revoke_device", {{"id", passkey_device_id}}, context);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    context.authorized = false;
    assertion = registry.call("begin_passkey_sign_in", json::object(), context);
    ASSERT_TRUE(assertion.ok) << assertion.error.message;
    const auto after_revoke =
        sign_in_response(registry, context, assertion.value, authenticator, 3);
    EXPECT_FALSE(after_revoke.ok);
    EXPECT_EQ(after_revoke.error.code, "unauthorized");
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
