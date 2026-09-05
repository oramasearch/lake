#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/memory_store.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

std::vector<unsigned char> decode_base64url(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) {
        value.push_back('=');
    }
    std::vector<unsigned char> decoded((value.size() / 4) * 3);
    const int size = EVP_DecodeBlock(decoded.data(),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    if (value.ends_with('=')) {
        ++padding;
    }
    if (value.ends_with("==")) {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(size) - padding);
    return decoded;
}

std::vector<std::string> token_parts(const std::string& token) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= token.size()) {
        const std::size_t end = token.find('.', start);
        parts.push_back(
            token.substr(start, end == std::string::npos ? token.size() - start : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

bool verifies_with_jwk(const std::string& token, const json& jwk) {
    const auto parts = token_parts(token);
    if (parts.size() != 3) {
        return false;
    }
    const auto public_key = decode_base64url(jwk.at("x"));
    const auto signature = decode_base64url(parts[2]);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    const std::string signed_value = parts[0] + "." + parts[1];
    return key && context &&
           EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
           EVP_DigestVerify(context.get(),
                            signature.data(),
                            signature.size(),
                            reinterpret_cast<const unsigned char*>(signed_value.data()),
                            signed_value.size()) == 1;
}

json token_claims(const std::string& token) {
    const auto parts = token_parts(token);
    if (parts.size() != 3) {
        return json::object();
    }
    const auto decoded = decode_base64url(parts[1]);
    return json::parse(std::string(decoded.begin(), decoded.end()));
}

struct ApplicationSurface {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext context;
    std::string customer_id;

    ApplicationSurface() {
        lake::api::register_core_capabilities(registry, "test");
        context.store = &store;
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        context.public_host = "lake.example.test";
        context.request_origin = "https://lake.example.test";
        const auto owner =
            store.create_user({"taap:1:ed25519:application-owner", "owner@example.test", "Owner"});
        EXPECT_TRUE(owner) << owner.error().message;
        if (owner) {
            const auto customer =
                store.create_customer({"individual", owner->id, "", "application-owner"});
            EXPECT_TRUE(customer) << customer.error().message;
            if (customer) {
                customer_id = customer->id;
            }
        }
    }

    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id = lake::control::new_uuid_v4();
        return registry.call(name, input, context);
    }

    ApiResult register_application(std::string name) {
        const std::string domain =
            name == "Client one" ? "login.client.example.test" : "login.client-two.example.test";
        return call("register_application",
                    {{"customer_id", customer_id},
                     {"name", std::move(name)},
                     {"allowed_origins", json::array({"https://client.example.test"})},
                     {"redirect_uris", json::array({"https://client.example.test/callback"})},
                     {"custom_sign_in_domain", domain},
                     {"theme", {{"accent_color", "#123456"}}}});
    }
};

TEST(ApplicationAuth, RegistrationHostedIdentityJwtWebhooksAndIsolation) {
    ApplicationSurface surface;
    const ApiResult first = surface.register_application("Client one");
    const ApiResult second = surface.register_application("Client two");
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    const std::string application_id = first.value.at("id");
    const std::string pool_id = first.value.at("pool_id");

    int sends = 0;
    json sent_headers;
    std::string sent_payload;
    surface.context.webhook_sender =
        [&](const std::string& url, const json& headers, const std::string& payload) {
            EXPECT_EQ(url, "https://hooks.example.test/lifecycle");
            sent_headers = headers;
            sent_payload = payload;
            ++sends;
            return lake::api::WebhookDeliveryResult{.status = sends == 1 ? 503 : 204,
                                                    .error = sends == 1 ? "temporary failure" : ""};
        };
    const ApiResult webhook = surface.call("create_application_webhook",
                                           {{"application_id", application_id},
                                            {"url", "https://hooks.example.test/lifecycle"},
                                            {"events", json::array({"session.started"})}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;
    ASSERT_TRUE(webhook.value.contains("secret"));

    const ApiResult sign_up = surface.call("sign_up_password",
                                           {{"pool_id", pool_id},
                                            {"email", "user@example.test"},
                                            {"password", "correct horse battery staple"},
                                            {"display_name", "Application user"},
                                            {"identity_document", "{}"}});
    ASSERT_TRUE(sign_up.ok) << sign_up.error.message;
    ASSERT_TRUE(sign_up.value.contains("access_token"));
    EXPECT_EQ(sends, 2);

    const ApiResult jwks = surface.call("application_jwks", {{"application_id", application_id}});
    ASSERT_TRUE(jwks.ok) << jwks.error.message;
    ASSERT_EQ(jwks.value.at("keys").size(), 1);
    const std::string token = sign_up.value.at("access_token");
    EXPECT_TRUE(verifies_with_jwk(token, jwks.value.at("keys").at(0)));
    const json claims = token_claims(token);
    EXPECT_EQ(claims.at("aud"), application_id);
    EXPECT_EQ(claims.at("pool_id"), pool_id);

    const auto event = json::parse(sent_payload);
    EXPECT_EQ(event.at("event"), "session.started");
    EXPECT_EQ(sent_headers.at("X-Lake-Event-Id"), event.at("id"));
    EXPECT_EQ(sent_headers.at("X-Lake-Signature"),
              lake::control::apps::webhook_signature(webhook.value.at("secret").get<std::string>(),
                                                     event.at("id").get<std::string>(),
                                                     sent_payload));
    const auto deliveries = surface.store.list_application_webhook_deliveries(event.at("id"));
    ASSERT_TRUE(deliveries);
    ASSERT_EQ(deliveries->size(), 2);
    EXPECT_EQ(deliveries->at(0).status, 503);
    EXPECT_EQ(deliveries->at(1).status, 204);

    const ApiResult sign_in = surface.call("sign_in_password",
                                           {{"pool_id", pool_id},
                                            {"email", "user@example.test"},
                                            {"password", "correct horse battery staple"}});
    ASSERT_TRUE(sign_in.ok) << sign_in.error.message;
    EXPECT_TRUE(verifies_with_jwk(sign_in.value.at("access_token"), jwks.value.at("keys").at(0)));

    const ApiResult users =
        surface.call("list_application_users", {{"application_id", application_id}});
    ASSERT_TRUE(users.ok) << users.error.message;
    ASSERT_EQ(users.value.at("items").size(), 1);
    const std::string user_id = users.value.at("items").at(0).at("user_id");
    const ApiResult suspended = surface.call(
        "suspend_application_user", {{"application_id", application_id}, {"user_id", user_id}});
    ASSERT_TRUE(suspended.ok) << suspended.error.message;
    EXPECT_EQ(suspended.value.at("status"), "suspended");
    EXPECT_FALSE(surface.store.context_for(sign_in.value.at("secret")));
    EXPECT_FALSE(surface
                     .call("sign_in_password",
                           {{"pool_id", pool_id},
                            {"email", "user@example.test"},
                            {"password", "correct horse battery staple"}})
                     .ok);

    const ApiResult cross_pool = surface.call("sign_in_password",
                                              {{"pool_id", second.value.at("pool_id")},
                                               {"email", "user@example.test"},
                                               {"password", "correct horse battery staple"}});
    EXPECT_FALSE(cross_pool.ok);
    EXPECT_EQ(cross_pool.error.code, "unauthorized");
}

TEST(ApplicationAuth, HostedRenderersApplyThemeAndLimitScriptsToWebAuthn) {
    lake::control::Application application{
        .id = "7cd99aac-4f89-4d4e-a587-0d297940bd09",
        .customer_id = "customer",
        .pool_id = "app:7cd99aac-4f89-4d4e-a587-0d297940bd09",
        .name = "Example product",
        .allowed_origins = {"https://product.example.test"},
        .redirect_uris = {"https://product.example.test/callback"},
        .custom_sign_in_domain = "login.product.example.test",
        .theme = {{"accent_color", "#123456"}, {"background_color", "#abcdef"}},
    };
    const std::string sign_up = lake::api::web_ui_application_sign_up(application, "");
    const std::string sign_in = lake::api::web_ui_application_sign_in(application, "");
    const std::string mfa = lake::api::web_ui_application_mfa(application, "");
    const std::string passkey = lake::api::web_ui_application_passkey_sign_in(application, "");
    EXPECT_NE(sign_up.find("Example product"), std::string::npos);
    EXPECT_NE(sign_up.find("--accent:#123456"), std::string::npos);
    EXPECT_NE(sign_up.find("--paper:#abcdef"), std::string::npos);
    EXPECT_EQ(sign_up.find("<script"), std::string::npos);
    EXPECT_EQ(sign_in.find("<script"), std::string::npos);
    EXPECT_EQ(mfa.find("<script"), std::string::npos);
    EXPECT_EQ(passkey.find("<script"), passkey.rfind("<script"));
    EXPECT_NE(passkey.find("navigator.credentials.get"), std::string::npos);
}

TEST(ApplicationAuth, ApplicationListsFilterThroughPolicy) {
    ApplicationSurface surface;
    const ApiResult first = surface.register_application("Client one");
    const ApiResult second = surface.register_application("Client two");
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    const auto reader = surface.store.create_user(
        {"taap:1:ed25519:application-reader", "reader@example.test", "Reader"});
    ASSERT_TRUE(reader) << reader.error().message;
    const auto object = surface.store.find_policy_object("application", first.value.at("id"));
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(surface.store.create_grant({reader->taap_id,
                                            object->id,
                                            "read",
                                            "application-reader-attestation",
                                            "admin",
                                            "application-reader-request"}));

    surface.context.operator_credential = false;
    surface.context.acting_identity = reader->taap_id;
    const ApiResult applications =
        surface.call("list_applications", {{"customer_id", surface.customer_id}});
    ASSERT_TRUE(applications.ok) << applications.error.message;
    ASSERT_EQ(applications.value.at("items").size(), 1);
    EXPECT_EQ(applications.value.at("items").at(0).at("id"), first.value.at("id"));
    EXPECT_TRUE(
        surface.call("list_application_users", {{"application_id", first.value.at("id")}}).ok);
    const ApiResult denied =
        surface.call("list_application_users", {{"application_id", second.value.at("id")}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
}

} // namespace
