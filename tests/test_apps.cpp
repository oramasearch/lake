// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/application_authorization_codes.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/application_settings_records.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "stream_fixture.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

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
    std::shared_ptr<lake::control::InMemoryStore> owned =
        std::make_shared<lake::control::InMemoryStore>();
    lake::control::InMemoryStore& store = *owned;
    ApiContext context;
    std::string customer_id;
    // The space's workspaces, each Ready at its own endpoint (http://<name>.test, the first at
    // http://workspace.test), with the record store a workspace Zetta keeps.
    std::unique_ptr<lake::test::StreamWorkspace> workspaces;
    std::string workspace_id;

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
        workspaces = std::make_unique<lake::test::StreamWorkspace>(owned, customer_id);
        workspace_id = workspaces->id;
        context.workspaces = workspaces->service.get();
    }

    // Every capability through the asynchronous path, as REST and MCP call them.
    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? *completed : ApiResult::failure("store_error", "did not complete");
    }

    ApiResult register_application(std::string name, std::string workspace = {},
                                    std::string customer = {}) {
        const std::string domain =
            name == "Client one" ? "login.client.example.test" : "login.client-two.example.test";
        return call("register_application",
                    {{"customer_id", customer.empty() ? customer_id : customer},
                     {"workspace_id", workspace.empty() ? workspace_id : workspace},
                     {"name", std::move(name)},
                     {"allowed_origins", json::array({"https://client.example.test"})},
                     {"redirect_uris", json::array({"https://client.example.test/callback"})},
                     {"custom_sign_in_domain", domain},
                     {"theme", {{"accent_color", "#123456"}}}});
    }

    // The records the workspace Zetta at `endpoint` holds for one lake collection.
    std::map<std::string, json>& records(const std::string& endpoint, std::string_view collection) {
        return workspaces->caller->records[endpoint].rows(collection);
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
                                            {"workspace_id", surface.workspace_id},
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
    const ApiResult events = surface.call(
        "list_application_webhook_events",
        {{"application_id", application_id}, {"workspace_id", surface.workspace_id}});
    ASSERT_TRUE(events.ok) << events.error.message;
    const auto listed = std::ranges::find_if(events.value.at("items"), [&](const json& item) {
        return item.at("id") == event.at("id");
    });
    ASSERT_NE(listed, events.value.at("items").end());
    const json& deliveries = listed->at("deliveries");
    ASSERT_EQ(deliveries.size(), 2U);
    EXPECT_EQ(deliveries.at(0).at("status"), 503);
    EXPECT_EQ(deliveries.at(0).at("attempt"), 1);
    EXPECT_EQ(deliveries.at(1).at("status"), 204);
    EXPECT_FALSE(deliveries.at(1).at("pending"));
    // The event and its attempts are in the application's workspace Zetta, sealed.
    const std::string endpoint = "http://workspace.test";
    EXPECT_EQ(surface.records(endpoint, "auth_app_webhook_deliveries").size(), 2U);
    for (const auto& [key, record] : surface.records(endpoint, "auth_app_webhook_events")) {
        EXPECT_EQ(record.dump().find("session.started"), std::string::npos) << "sealed body";
    }

    const ApiResult sign_in = surface.call("sign_in_password",
                                           {{"pool_id", pool_id},
                                            {"email", "user@example.test"},
                                            {"password", "correct horse battery staple"}});
    ASSERT_TRUE(sign_in.ok) << sign_in.error.message;
    EXPECT_TRUE(verifies_with_jwk(sign_in.value.at("access_token"), jwks.value.at("keys").at(0)));

    const ApiResult users =
        surface.call("list_application_users",
                     {{"application_id", application_id}, {"workspace_id", surface.workspace_id}});
    ASSERT_TRUE(users.ok) << users.error.message;
    ASSERT_EQ(users.value.at("items").size(), 1);
    const std::string user_id = users.value.at("items").at(0).at("user_id");
    const ApiResult suspended = surface.call("suspend_application_user",
                                             {{"application_id", application_id},
                                              {"workspace_id", surface.workspace_id},
                                              {"user_id", user_id}});
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
    // Credential failures share the generic 401 (lake#966).
    EXPECT_EQ(cross_pool.error.code, "unauthenticated");
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
    EXPECT_NE(sign_up.find("class=\"hosted-shell\""), std::string::npos);
    EXPECT_NE(sign_up.find("id=\"theme-preference\""), std::string::npos);
    EXPECT_NE(sign_up.find("id=\"theme-toggle-script\""), std::string::npos);
    EXPECT_EQ(occurrences(sign_up, "<script"), 2);
    EXPECT_EQ(occurrences(sign_in, "<script"), 2);
    EXPECT_EQ(occurrences(mfa, "<script"), 2);
    EXPECT_EQ(occurrences(passkey, "<script"), 3);
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
    EXPECT_TRUE(surface
                    .call("list_application_users",
                          {{"application_id", first.value.at("id")},
                           {"workspace_id", surface.workspace_id}})
                    .ok);
    const ApiResult denied = surface.call(
        "list_application_users",
        {{"application_id", second.value.at("id")}, {"workspace_id", surface.workspace_id}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
}

// lake#356 and lake#357: an application's events and delivery attempts live in the workspace Zetta
// of the application's workspace, chosen at registration, in a space with several workspaces and
// beside another space.
TEST(ApplicationAuth, EventsAndDeliveriesLiveInTheApplicationsWorkspace) {
    ApplicationSurface surface;
    const std::string home = surface.workspace_id;
    const std::string lab = surface.workspaces->add(surface.owned, surface.customer_id, "lab");
    const auto other_owner = surface.store.create_user(
        {"taap:1:ed25519:application-other", "other@example.test", "Other"});
    ASSERT_TRUE(other_owner);
    const auto other_customer =
        surface.store.create_customer({"individual", other_owner->id, "", "application-other"});
    ASSERT_TRUE(other_customer);
    const std::string theirs = surface.workspaces->add(surface.owned, other_customer->id, "theirs");

    // Registration names one workspace of the space: missing, malformed, unknown and foreign ids
    // are refused at the field that is at fault.
    const auto missing = surface.call(
        "register_application",
        {{"customer_id", surface.customer_id},
         {"name", "No workspace"},
         {"allowed_origins", json::array({"https://client.example.test"})},
         {"redirect_uris", json::array({"https://client.example.test/callback"})}});
    EXPECT_EQ(missing.error.code, "invalid_input");
    EXPECT_EQ(missing.error.path, "/workspace_id");
    for (const std::string bad : {std::string("not-a-workspace"), lake::control::new_uuid_v4()}) {
        const auto refused = surface.register_application("Client one", bad);
        EXPECT_EQ(refused.error.code, "invalid_input") << bad;
        EXPECT_EQ(refused.error.path, "/workspace_id") << bad;
    }
    const auto foreign = surface.register_application("Client one", theirs);
    EXPECT_EQ(foreign.error.code, "invalid_input");
    EXPECT_EQ(foreign.error.path, "/customer_id");

    const ApiResult first = surface.register_application("Client one", home);
    const ApiResult second = surface.register_application("Client two", lab);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(first.value.at("workspace_id"), home);
    EXPECT_EQ(second.value.at("workspace_id"), lab);
    const auto their_app = surface.call(
        "register_application",
        {{"customer_id", other_customer->id},
         {"workspace_id", theirs},
         {"name", "Their client"},
         {"allowed_origins", json::array({"https://their.example.test"})},
         {"redirect_uris", json::array({"https://their.example.test/callback"})}});
    ASSERT_TRUE(their_app.ok) << their_app.error.message;

    const auto organization = [&](const ApiResult& application, std::string name) {
        return surface.call("create_application_organization",
                            {{"application_id", application.value.at("id")},
                             {"workspace_id", application.value.at("workspace_id")},
                             {"taap_id", "taap:1:ed25519:" + name},
                             {"name", name}});
    };
    ASSERT_TRUE(organization(first, "first-org").ok);
    ASSERT_TRUE(organization(second, "second-org").ok);
    ASSERT_TRUE(organization(their_app, "their-org").ok);

    // Each event is stored only at its own application's workspace endpoint.
    EXPECT_EQ(surface.records("http://workspace.test", "auth_app_webhook_events").size(), 1U);
    EXPECT_EQ(surface.records("http://lab.test", "auth_app_webhook_events").size(), 1U);
    EXPECT_EQ(surface.records("http://theirs.test", "auth_app_webhook_events").size(), 1U);

    const auto list = [&](const ApiResult& application, json workspace) {
        json input{{"application_id", application.value.at("id")}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return surface.call("list_application_webhook_events", input);
    };
    const auto listed = list(first, home);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("items").size(), 1U);
    EXPECT_EQ(listed.value.at("items").at(0).at("event"), "organization.created");
    EXPECT_EQ(listed.value.at("items").at(0).at("payload").at("name"), "first-org");

    // Another workspace of the space, another space's workspace, or no workspace: refused, never
    // read anywhere else.
    for (const json& wrong : {json(lab), json(theirs), json(lake::control::new_uuid_v4()),
                              json("not-a-workspace"), json(nullptr)}) {
        const auto refused = list(first, wrong);
        EXPECT_EQ(refused.error.code, "invalid_input") << wrong;
        EXPECT_EQ(refused.error.path, "/workspace_id") << wrong;
    }

    // Least privilege: a reader of one application reads its events, but cannot retry them and
    // cannot read another application's.
    const auto reader = surface.store.create_user(
        {"taap:1:ed25519:application-events-reader", "events-reader@example.test", "Reader"});
    ASSERT_TRUE(reader);
    const auto object = surface.store.find_policy_object("application", first.value.at("id"));
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant({reader->taap_id, object->id, "read",
                                            "events-reader-attestation", "admin",
                                            "events-reader-request"}));
    surface.context.operator_credential = false;
    surface.context.acting_identity = reader->taap_id;
    EXPECT_TRUE(list(first, home).ok);
    EXPECT_EQ(list(second, lab).error.code, "unauthorized");
    const auto retried = surface.call("retry_application_webhook",
                                      {{"application_id", first.value.at("id")},
                                       {"workspace_id", home},
                                       {"event_id", listed.value.at("items").at(0).at("id")},
                                       {"webhook_id", lake::control::new_uuid_v4()}});
    EXPECT_EQ(retried.error.code, "unauthorized");
    surface.context.operator_credential = true;
    surface.context.acting_identity = "admin";

    // A workspace that is not ready refuses the event and the read instead of falling back.
    const auto lab_row = surface.store.get_workspace(lab);
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Provisioning", "",
                                                 "http://lab.test"));
    EXPECT_EQ(list(second, lab).error.code, "workspace_not_ready");
    EXPECT_EQ(organization(second, "second-late").error.code, "workspace_not_ready");
    EXPECT_EQ(surface.records("http://lab.test", "auth_app_webhook_events").size(), 1U);
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Ready", "",
                                                 "http://lab.test"));

    // Deleting a workspace takes its application's events with it; the other workspace keeps its
    // own, and the deleted one is never read again.
    ASSERT_TRUE(surface.store.update_workspace(lab, {.desired = "deleted"}));
    const auto deleted = list(second, lab);
    EXPECT_EQ(deleted.error.code, "invalid_input");
    EXPECT_EQ(deleted.error.path, "/workspace_id");
    EXPECT_FALSE(organization(second, "second-after").ok);
    EXPECT_EQ(list(first, home).value.at("items").size(), 1U);
}

// Two callers never send the same attempt: an attempt is claimed in the workspace with if_absent
// before it is sent, so a retry that finds the next attempt already claimed sends nothing.
TEST(ApplicationAuth, AnAttemptIsClaimedOnceAndSentOnce) {
    ApplicationSurface surface;
    const ApiResult application = surface.register_application("Client one");
    ASSERT_TRUE(application.ok) << application.error.message;
    const std::string application_id = application.value.at("id");
    int sends = 0;
    surface.context.webhook_sender = [&](const std::string&, const json&, const std::string&) {
        ++sends;
        return lake::api::WebhookDeliveryResult{.status = 500, .error = "down"};
    };
    const ApiResult webhook = surface.call("create_application_webhook",
                                           {{"application_id", application_id},
                                            {"workspace_id", surface.workspace_id},
                                            {"url", "https://hooks.example.test/lifecycle"},
                                            {"events", json::array({"organization.created"})}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;
    ASSERT_TRUE(surface
                    .call("create_application_organization",
                          {{"application_id", application_id},
                           {"workspace_id", surface.workspace_id},
                           {"taap_id", "taap:1:ed25519:claimed-org"},
                           {"name", "claimed"}})
                    .ok);
    EXPECT_EQ(sends, 3) << "three consecutive attempts, all failing";
    const auto listed = surface.call(
        "list_application_webhook_events",
        {{"application_id", application_id}, {"workspace_id", surface.workspace_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    const std::string event_id = listed.value.at("items").at(0).at("id");

    // Another lake process claims attempt 4 first; this retry refuses and sends nothing.
    auto records =
        lake::api::workspace_records(surface.context, surface.customer_id, surface.workspace_id);
    ASSERT_TRUE(records) << records.error().message;
    lake::control::ApplicationWebhookDelivery other;
    other.application_id = application_id;
    other.event_id = event_id;
    other.webhook_id = webhook.value.at("id");
    other.attempt = 4;
    other.pending = true;
    auto claimed = lake::control::settled(liquid::runtime::block_on(
        lake::control::claim_application_webhook_delivery(*records, other)));
    ASSERT_TRUE(claimed) << claimed.error().message;
    auto again = lake::control::settled(liquid::runtime::block_on(
        lake::control::claim_application_webhook_delivery(*records, other)));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, "conflict");

    const auto retry = [&] {
        return surface.call("retry_application_webhook",
                            {{"application_id", application_id},
                             {"workspace_id", surface.workspace_id},
                             {"event_id", event_id},
                             {"webhook_id", webhook.value.at("id")}});
    };
    const ApiResult raced = retry();
    EXPECT_EQ(raced.error.code, "conflict");
    EXPECT_EQ(sends, 3) << "a claimed attempt is never sent by a second caller";

    // Once the other caller records its outcome, the next retry continues after it.
    ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(
        lake::control::settle_application_webhook_delivery(*records, std::move(*claimed)))));
    surface.context.webhook_sender = [&](const std::string&, const json&, const std::string&) {
        ++sends;
        return lake::api::WebhookDeliveryResult{.status = 204};
    };
    const ApiResult delivered = retry();
    ASSERT_TRUE(delivered.ok) << delivered.error.message;
    EXPECT_EQ(delivered.value.at("attempt"), 5);
    EXPECT_EQ(sends, 4);
    const auto after = surface.call(
        "list_application_webhook_events",
        {{"application_id", application_id}, {"workspace_id", surface.workspace_id}});
    ASSERT_EQ(after.value.at("items").at(0).at("deliveries").size(), 5U);

    // A claim that was never settled, older than any send takes, was abandoned by a caller that
    // stopped: a retry continues after it.
    lake::control::ApplicationWebhookEvents events(*records);
    const auto old_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() -
                        10LL * 60 * 1'000'000;
    auto stale_event = lake::control::settled(liquid::runtime::block_on(events.append(
        {.application_id = application_id, .event = "organization.created", .payload = json::object()},
        lake::control::RecordWrite{lake::control::new_uuid_v4(), old_us})));
    ASSERT_TRUE(stale_event) << stale_event.error().message;
    lake::control::ApplicationWebhookDelivery abandoned = other;
    abandoned.event_id = stale_event->id;
    abandoned.attempt = 1;
    abandoned.at_us = old_us + 1'000'000;
    ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(
        lake::control::claim_application_webhook_delivery(*records, abandoned))));
    const ApiResult resumed = surface.call("retry_application_webhook",
                                           {{"application_id", application_id},
                                            {"workspace_id", surface.workspace_id},
                                            {"event_id", stale_event->id},
                                            {"webhook_id", webhook.value.at("id")}});
    ASSERT_TRUE(resumed.ok) << resumed.error.message;
    EXPECT_EQ(resumed.value.at("attempt"), 2);
    EXPECT_EQ(sends, 5);

    // An event id from another application, or one lake never made, is not found.
    const auto unknown = surface.call("retry_application_webhook",
                                      {{"application_id", application_id},
                                       {"workspace_id", surface.workspace_id},
                                       {"event_id", lake::control::new_uuid_v4()},
                                       {"webhook_id", webhook.value.at("id")}});
    EXPECT_EQ(unknown.error.code, "not_found");
}

// lake#353, lake#354: an application's users and organizations live in the workspace Zetta of the
// application's workspace, among two spaces and a space with two workspaces. What must be unique
// within an application (a pool user's email; an organization's TAAP identity and name) is a claim
// written with if_absent there, so a second sign-up or create is a conflict.
TEST(ApplicationAuth, UsersAndOrganizationsLiveInTheApplicationsWorkspace) {
    ApplicationSurface surface;
    const std::string home = surface.workspace_id;
    const std::string lab = surface.workspaces->add(surface.owned, surface.customer_id, "lab");
    const auto other_owner = surface.store.create_user(
        {"taap:1:ed25519:directory-other", "directory-other@example.test", "Other"});
    ASSERT_TRUE(other_owner);
    const auto other_customer =
        surface.store.create_customer({"individual", other_owner->id, "", "directory-other"});
    ASSERT_TRUE(other_customer);
    const std::string theirs = surface.workspaces->add(surface.owned, other_customer->id, "theirs");

    const ApiResult first = surface.register_application("Client one", home);
    const ApiResult second = surface.register_application("Client two", lab);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    const std::string first_id = first.value.at("id");
    const std::string second_id = second.value.at("id");
    const auto sign_up = [&](const ApiResult& application, const std::string& email) {
        return surface.call("sign_up_password",
                            {{"pool_id", application.value.at("pool_id")},
                             {"email", email},
                             {"password", "correct horse battery staple"},
                             {"display_name", "Directory user"},
                             {"identity_document", "{}"}});
    };
    const auto sign_in = [&](const ApiResult& application, const std::string& email) {
        return surface.call("sign_in_password",
                            {{"pool_id", application.value.at("pool_id")},
                             {"email", email},
                             {"password", "correct horse battery staple"}});
    };
    const auto users = [&](const std::string& application_id, json workspace) {
        json input{{"application_id", application_id}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return surface.call("list_application_users", input);
    };

    // One email signs up once per application, case-insensitively; another application of the
    // space has its own users.
    const ApiResult signed_up = sign_up(first, "person@example.test");
    ASSERT_TRUE(signed_up.ok) << signed_up.error.message;
    const auto again = sign_up(first, "Person@Example.test");
    EXPECT_EQ(again.error.code, "conflict");
    ASSERT_TRUE(sign_up(second, "person@example.test").ok);
    // The user and the email claim are stored sealed only at the application's own workspace.
    EXPECT_EQ(surface.records("http://workspace.test", "auth_app_users").size(), 2U);
    EXPECT_EQ(surface.records("http://lab.test", "auth_app_users").size(), 2U);
    EXPECT_TRUE(surface.records("http://theirs.test", "auth_app_users").empty());
    for (const auto& [key, record] : surface.records("http://workspace.test", "auth_app_users")) {
        EXPECT_EQ(record.dump().find("person@example.test"), std::string::npos) << "sealed";
    }
    const auto listed = users(first_id, home);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("items").size(), 1U);
    const std::string user_id = listed.value.at("items").at(0).at("user_id");
    EXPECT_EQ(listed.value.at("items").at(0).at("email"), "person@example.test");
    EXPECT_EQ(users(first_id, home).value.at("items").size(), 1U);
    EXPECT_EQ(surface
                  .call("list_application_users",
                        {{"application_id", first_id}, {"workspace_id", home}, {"query", "nobody"}})
                  .value.at("items")
                  .size(),
              0U);

    // Another workspace of the space, another space's workspace, an unknown or malformed id, or
    // none: refused at /workspace_id, never read anywhere else.
    for (const json& wrong : {json(lab),
                              json(theirs),
                              json(lake::control::new_uuid_v4()),
                              json("not-a-workspace"),
                              json(nullptr)}) {
        const auto refused = users(first_id, wrong);
        EXPECT_EQ(refused.error.code, "invalid_input") << wrong;
        EXPECT_EQ(refused.error.path, "/workspace_id") << wrong;
    }
    const auto exported =
        surface.call("export_application_user",
                     {{"application_id", first_id}, {"workspace_id", home}, {"user_id", user_id}});
    ASSERT_TRUE(exported.ok) << exported.error.message;
    EXPECT_EQ(
        surface
            .call("export_application_user",
                  {{"application_id", second_id}, {"workspace_id", lab}, {"user_id", user_id}})
            .error.code,
        "not_found")
        << "a user of one application is not a user of another";

    // An abandoned claim (older than the grace period, naming an account lake never created) is
    // taken over; a fresh one is not.
    auto records = lake::api::workspace_records(surface.context, surface.customer_id, home);
    ASSERT_TRUE(records);
    const auto collection = lake::control::ApplicationUserCodec::collection;
    const auto stale_claim = [&](const std::string& email, std::int64_t age_us) {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        return lake::control::settled(liquid::runtime::block_on(
            records->put(collection,
                         lake::control::application_claim_key(
                             *records, collection, first_id, lake::control::email_claim, email),
                         lake::control::application_claims_lookup(first_id),
                         {{"kind", "claim"},
                          {"application_id", first_id},
                          {"holder", lake::control::new_uuid_v4()}},
                         {.if_absent = true},
                         1,
                         now - age_us)));
    };
    ASSERT_TRUE(stale_claim("fresh@example.test", 1'000'000));
    EXPECT_EQ(sign_up(first, "fresh@example.test").error.code, "conflict");
    ASSERT_TRUE(stale_claim("abandoned@example.test", 10LL * 60 * 1'000'000));
    EXPECT_TRUE(sign_up(first, "abandoned@example.test").ok);

    // A sign-up whose user record was not written completes at the next sign-in, because its
    // email claim names the account; an account with neither is not a user and its new session
    // is revoked.
    const json everyone = users(first_id, home).value.at("items");
    const auto account = std::ranges::find_if(
        everyone, [](const json& item) { return item.at("email") == "abandoned@example.test"; });
    ASSERT_NE(account, everyone.end());
    const std::string abandoned_id = account->at("user_id");
    ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(records->remove(
        collection, lake::control::application_user_key(*records, first_id, abandoned_id)))));
    const auto healed = sign_in(first, "abandoned@example.test");
    ASSERT_TRUE(healed.ok) << healed.error.message;
    ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(records->remove(
        collection, lake::control::application_user_key(*records, first_id, abandoned_id)))));
    ASSERT_TRUE(
        lake::control::settled(liquid::runtime::block_on(lake::control::release_application_claim(
            *records,
            collection,
            lake::control::application_claim_key(*records,
                                                 collection,
                                                 first_id,
                                                 lake::control::email_claim,
                                                 "abandoned@example.test"),
            abandoned_id))));
    const auto live_sessions = [&] {
        const auto sessions = surface.store.list_sessions(abandoned_id, first.value.at("pool_id"));
        EXPECT_TRUE(sessions);
        return sessions
                   ? std::ranges::count_if(
                         *sessions, [](const auto& session) { return session.revoked_at.empty(); })
                   : 0;
    };
    const auto live_before = live_sessions();
    const auto stranger = sign_in(first, "abandoned@example.test");
    EXPECT_EQ(stranger.error.code, "unauthorized");
    EXPECT_EQ(live_sessions(), live_before) << "a refused sign-in leaves no live session";

    // Suspension is written in the workspace and revokes the pool's sessions; a later sign-in is
    // refused there and its session revoked too.
    const auto suspended =
        surface.call("suspend_application_user",
                     {{"application_id", first_id}, {"workspace_id", home}, {"user_id", user_id}});
    ASSERT_TRUE(suspended.ok) << suspended.error.message;
    EXPECT_FALSE(surface.store.context_for(signed_up.value.at("secret")));
    EXPECT_EQ(sign_in(first, "person@example.test").error.code, "unauthorized");
    EXPECT_EQ(
        surface
            .call("issue_application_jwt",
                  {{"application_id", first_id}, {"workspace_id", home}, {"user_id", user_id}})
            .error.code,
        "unauthorized");
    // Deleting frees the email for a new sign-up.
    ASSERT_TRUE(
        surface
            .call("delete_application_user",
                  {{"application_id", first_id}, {"workspace_id", home}, {"user_id", user_id}})
            .ok);
    EXPECT_TRUE(sign_up(first, "person@example.test").ok);

    // Organizations: the TAAP identity and the name are each unique within the application.
    const auto organization = [&](const std::string& application_id,
                                  const std::string& workspace,
                                  const std::string& taap,
                                  const std::string& name) {
        return surface.call("create_application_organization",
                            {{"application_id", application_id},
                             {"workspace_id", workspace},
                             {"taap_id", taap},
                             {"name", name}});
    };
    const auto acme = organization(first_id, home, "taap:1:ed25519:acme", "Acme");
    ASSERT_TRUE(acme.ok) << acme.error.message;
    EXPECT_EQ(organization(first_id, home, "taap:1:ed25519:acme", "Other").error.code, "conflict");
    EXPECT_EQ(organization(first_id, home, "taap:1:ed25519:other", "Acme").error.code, "conflict");
    ASSERT_TRUE(organization(second_id, lab, "taap:1:ed25519:acme", "Acme").ok)
        << "another application has its own organizations";
    const auto beta = organization(first_id, home, "taap:1:ed25519:beta", "Beta");
    ASSERT_TRUE(beta.ok);
    const auto rename = [&](const ApiResult& row, const std::string& name) {
        return surface.call("update_application_organization",
                            {{"application_id", first_id},
                             {"workspace_id", home},
                             {"organization_id", row.value.at("id")},
                             {"name", name}});
    };
    EXPECT_EQ(rename(beta, "Acme").error.code, "conflict");
    const auto renamed = rename(acme, "Acme Corp");
    ASSERT_TRUE(renamed.ok) << renamed.error.message;
    EXPECT_EQ(renamed.value.at("name"), "Acme Corp");
    EXPECT_TRUE(organization(first_id, home, "taap:1:ed25519:gamma", "Acme").ok)
        << "a rename frees the old name";
    ASSERT_TRUE(surface
                    .call("delete_application_organization",
                          {{"application_id", first_id},
                           {"workspace_id", home},
                           {"organization_id", beta.value.at("id")}})
                    .ok);
    EXPECT_TRUE(organization(first_id, home, "taap:1:ed25519:beta", "Beta").ok)
        << "a deletion frees both";
    const auto organizations = surface.call("list_application_organizations",
                                            {{"application_id", first_id}, {"workspace_id", home}});
    ASSERT_TRUE(organizations.ok);
    EXPECT_EQ(organizations.value.at("items").size(), 3U);
    EXPECT_EQ(surface
                  .call("delete_application_organization",
                        {{"application_id", second_id},
                         {"workspace_id", lab},
                         {"organization_id", beta.value.at("id")}})
                  .error.code,
              "not_found");

    // Least privilege: a reader of one application lists its users and organizations, cannot
    // change them, and cannot read another application's.
    const auto reader = surface.store.create_user(
        {"taap:1:ed25519:directory-reader", "directory-reader@example.test", "Reader"});
    ASSERT_TRUE(reader);
    const auto object = surface.store.find_policy_object("application", first_id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant({reader->taap_id,
                                            object->id,
                                            "read",
                                            "directory-reader-attestation",
                                            "admin",
                                            "directory-reader-request"}));
    surface.context.operator_credential = false;
    surface.context.acting_identity = reader->taap_id;
    EXPECT_TRUE(users(first_id, home).ok);
    EXPECT_EQ(users(second_id, lab).error.code, "unauthorized");
    EXPECT_EQ(organization(first_id, home, "taap:1:ed25519:reader", "Reader").error.code,
              "unauthorized");
    surface.context.operator_credential = true;
    surface.context.acting_identity = "admin";

    // A workspace that is not ready refuses reads, writes and sign-ins rather than falling back.
    const auto lab_row = surface.store.get_workspace(lab);
    ASSERT_TRUE(surface.store.set_service_status(
        lab_row->zetta_service_id, "Provisioning", "", "http://lab.test"));
    EXPECT_EQ(users(second_id, lab).error.code, "workspace_not_ready");
    EXPECT_EQ(sign_in(second, "person@example.test").error.code, "workspace_not_ready");
    ASSERT_TRUE(surface.store.set_service_status(
        lab_row->zetta_service_id, "Ready", "", "http://lab.test"));
    EXPECT_TRUE(sign_in(second, "person@example.test").ok);

    // Deleting the workspace takes the application's users with it; the other is untouched.
    ASSERT_TRUE(surface.store.update_workspace(lab, {.desired = "deleted"}));
    EXPECT_EQ(users(second_id, lab).error.path, "/workspace_id");
    EXPECT_FALSE(sign_in(second, "person@example.test").ok);
    EXPECT_TRUE(users(first_id, home).ok);
}

// lake#352 and lake#355: an application's signing keys and its webhooks with their secrets live in
// the workspace Zetta of the application's workspace, sealed, among two spaces with two workspaces
// each. The JWKS is read anonymously by the application id alone.
TEST(ApplicationAuth, KeysAndWebhooksLiveInTheApplicationsWorkspace) {
    ApplicationSurface surface;
    const std::string home = surface.workspace_id;
    const std::string lab = surface.workspaces->add(surface.owned, surface.customer_id, "lab");
    const auto other_owner = surface.store.create_user(
        {"taap:1:ed25519:credential-other", "credential-other@example.test", "Other"});
    ASSERT_TRUE(other_owner);
    const auto other_customer =
        surface.store.create_customer({"individual", other_owner->id, "", "credential-other"});
    ASSERT_TRUE(other_customer);
    const std::string theirs = surface.workspaces->add(surface.owned, other_customer->id, "theirs");
    const std::string theirs_too =
        surface.workspaces->add(surface.owned, other_customer->id, "theirs-too");
    int sends = 0;
    surface.context.webhook_sender = [&](const std::string&, const json&, const std::string&) {
        ++sends;
        return lake::api::WebhookDeliveryResult{.status = 204};
    };

    const ApiResult first = surface.register_application("Client one", home);
    const ApiResult second = surface.register_application("Client two", lab);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    const auto their_app = surface.call(
        "register_application",
        {{"customer_id", other_customer->id},
         {"workspace_id", theirs_too},
         {"name", "Their client"},
         {"allowed_origins", json::array({"https://their.example.test"})},
         {"redirect_uris", json::array({"https://their.example.test/callback"})}});
    ASSERT_TRUE(their_app.ok) << their_app.error.message;
    const std::string first_id = first.value.at("id");
    const std::string second_id = second.value.at("id");

    // Each key ring is stored only at its own application's workspace endpoint, sealed: neither
    // the key id nor the public key nor the seed's envelope is readable there, and the control
    // store keeps no key row.
    const std::string home_url = "http://workspace.test";
    const std::string lab_url = "http://lab.test";
    EXPECT_EQ(surface.records(home_url, "auth_app_signing_keys").size(), 1U);
    EXPECT_EQ(surface.records(lab_url, "auth_app_signing_keys").size(), 1U);
    EXPECT_EQ(surface.records("http://theirs-too.test", "auth_app_signing_keys").size(), 1U);
    EXPECT_TRUE(surface.records("http://theirs.test", "auth_app_signing_keys").empty());
    EXPECT_EQ(surface.records(home_url, "auth_app_signing_keys").begin()->first, first_id);
    for (const auto& [key, record] : surface.records(home_url, "auth_app_signing_keys")) {
        EXPECT_EQ(record.dump().find("app-" + first_id), std::string::npos) << "sealed body";
        EXPECT_EQ(record.dump().find("encrypted_seed"), std::string::npos) << "sealed body";
    }

    // The JWKS is anonymous and needs only the application id; the routing pointer names the
    // workspace. A sign-in's token verifies with it.
    const auto jwks = [&](const std::string& application_id) {
        lake::api::ApiContext anonymous;
        anonymous.store = &surface.store;
        anonymous.workspaces = surface.context.workspaces;
        anonymous.instance_kek = surface.context.instance_kek;
        anonymous.request_id = lake::control::new_uuid_v4();
        auto completed = liquid::runtime::block_on(surface.registry.call_async(
            "application_jwks", {{"application_id", application_id}}, anonymous));
        EXPECT_TRUE(completed);
        return completed ? *completed : ApiResult::failure("store_error", "did not complete");
    };
    const auto first_keys = jwks(first_id);
    ASSERT_TRUE(first_keys.ok) << first_keys.error.message;
    ASSERT_EQ(first_keys.value.at("keys").size(), 1U);
    EXPECT_EQ(first_keys.value.at("keys").at(0).at("kid"), "app-" + first_id + "-1");
    const auto signed_up = surface.call("sign_up_password",
                                        {{"pool_id", first.value.at("pool_id")},
                                         {"email", "credential-user@example.test"},
                                         {"password", "correct horse battery staple"},
                                         {"display_name", "Credential user"},
                                         {"identity_document", "{}"}});
    ASSERT_TRUE(signed_up.ok) << signed_up.error.message;
    EXPECT_TRUE(verifies_with_jwk(signed_up.value.at("access_token"),
                                  first_keys.value.at("keys").at(0)));
    EXPECT_FALSE(verifies_with_jwk(signed_up.value.at("access_token"),
                                   jwks(second_id).value.at("keys").at(0)))
        << "another application's key";
    EXPECT_EQ(jwks(lake::control::new_uuid_v4()).error.code, "not_found");

    // Webhooks: created, listed and kept in the application's workspace, the secret returned once
    // and sealed there. Missing, malformed, unknown and foreign workspace ids are refused at
    // /workspace_id for every caller of the collection.
    const auto create_webhook = [&](const std::string& application_id, json workspace) {
        json input{{"application_id", application_id},
                   {"url", "https://hooks.example.test/" + application_id},
                   {"events", json::array({"organization.created"})}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return surface.call("create_application_webhook", input);
    };
    const auto list_webhooks = [&](const std::string& application_id, json workspace) {
        json input{{"application_id", application_id}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return surface.call("list_application_webhooks", input);
    };
    const auto rotate = [&](const std::string& application_id, json workspace) {
        json input{{"application_id", application_id}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return surface.call("rotate_application_signing_key", input);
    };
    const ApiResult webhook = create_webhook(first_id, home);
    ASSERT_TRUE(webhook.ok) << webhook.error.message;
    const std::string secret = webhook.value.at("secret");
    ASSERT_TRUE(create_webhook(second_id, lab).ok);
    EXPECT_EQ(surface.records(home_url, "auth_app_webhooks").size(), 1U);
    EXPECT_EQ(surface.records(lab_url, "auth_app_webhooks").size(), 1U);
    for (const auto& [key, record] : surface.records(home_url, "auth_app_webhooks")) {
        EXPECT_EQ(key, webhook.value.at("id").get<std::string>());
        EXPECT_EQ(record.dump().find(secret), std::string::npos) << "sealed secret";
        EXPECT_EQ(record.dump().find("hooks.example.test"), std::string::npos) << "sealed body";
    }
    const auto listed = list_webhooks(first_id, home);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("items").size(), 1U);
    EXPECT_EQ(listed.value.at("items").at(0).at("id"), webhook.value.at("id"));
    EXPECT_FALSE(listed.value.at("items").at(0).contains("secret"));
    for (const json& wrong : {json(lab), json(theirs), json(lake::control::new_uuid_v4()),
                              json("not-a-workspace"), json(nullptr)}) {
        for (const auto& refused :
             {create_webhook(first_id, wrong), list_webhooks(first_id, wrong), rotate(first_id, wrong)}) {
            EXPECT_EQ(refused.error.code, "invalid_input") << wrong;
            EXPECT_EQ(refused.error.path, "/workspace_id") << wrong;
        }
    }
    EXPECT_EQ(surface.records(home_url, "auth_app_webhooks").size(), 1U);

    // An event is signed with the secret opened from the workspace.
    std::string sent_signature, sent_payload, sent_event;
    surface.context.webhook_sender = [&](const std::string&, const json& headers,
                                         const std::string& payload) {
        ++sends;
        sent_signature = headers.at("X-Lake-Signature");
        sent_event = headers.at("X-Lake-Event-Id");
        sent_payload = payload;
        return lake::api::WebhookDeliveryResult{.status = 204};
    };
    ASSERT_TRUE(surface
                    .call("create_application_organization",
                          {{"application_id", first_id},
                           {"workspace_id", home},
                           {"taap_id", "taap:1:ed25519:credential-org"},
                           {"name", "credential org"}})
                    .ok);
    EXPECT_EQ(sends, 1);
    EXPECT_EQ(sent_signature,
              lake::control::apps::webhook_signature(secret, sent_event, sent_payload));
    // A webhook id of another application is not found through this one.
    const auto events = surface.call("list_application_webhook_events",
                                     {{"application_id", first_id}, {"workspace_id", home}});
    ASSERT_TRUE(events.ok) << events.error.message;
    const auto second_hooks = list_webhooks(second_id, lab);
    ASSERT_TRUE(second_hooks.ok);
    EXPECT_EQ(surface
                  .call("retry_application_webhook",
                        {{"application_id", first_id},
                         {"workspace_id", home},
                         {"event_id", events.value.at("items").at(0).at("id")},
                         {"webhook_id", second_hooks.value.at("items").at(0).at("id")}})
                  .error.code,
              "not_found");

    // Rotation keeps the old public key, retires it, and signs new tokens with the next key.
    const auto rotated = rotate(first_id, home);
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_EQ(rotated.value.at("version"), 2);
    const auto rotated_keys = jwks(first_id);
    ASSERT_EQ(rotated_keys.value.at("keys").size(), 2U);
    const auto signed_in = surface.call("sign_in_password",
                                        {{"pool_id", first.value.at("pool_id")},
                                         {"email", "credential-user@example.test"},
                                         {"password", "correct horse battery staple"}});
    ASSERT_TRUE(signed_in.ok) << signed_in.error.message;
    EXPECT_TRUE(verifies_with_jwk(signed_in.value.at("access_token"),
                                  rotated_keys.value.at("keys").at(1)));
    EXPECT_FALSE(verifies_with_jwk(signed_in.value.at("access_token"),
                                   rotated_keys.value.at("keys").at(0)));
    EXPECT_EQ(jwks(second_id).value.at("keys").size(), 1U) << "another application unchanged";

    // Least privilege: a reader of one application lists its webhooks but cannot create one or
    // rotate its keys, and cannot read another application's.
    const auto reader = surface.store.create_user(
        {"taap:1:ed25519:credential-reader", "credential-reader@example.test", "Reader"});
    ASSERT_TRUE(reader);
    const auto object = surface.store.find_policy_object("application", first_id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant({reader->taap_id, object->id, "read",
                                            "credential-reader-attestation", "admin",
                                            "credential-reader-request"}));
    surface.context.operator_credential = false;
    surface.context.acting_identity = reader->taap_id;
    EXPECT_TRUE(list_webhooks(first_id, home).ok);
    EXPECT_EQ(list_webhooks(second_id, lab).error.code, "unauthorized");
    EXPECT_EQ(create_webhook(first_id, home).error.code, "unauthorized");
    EXPECT_EQ(rotate(first_id, home).error.code, "unauthorized");
    surface.context.operator_credential = true;
    surface.context.acting_identity = "admin";
    EXPECT_EQ(jwks(first_id).value.at("keys").size(), 2U);

    // A workspace that is not ready refuses keys and webhooks instead of falling back.
    const auto lab_row = surface.store.get_workspace(lab);
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Provisioning", "",
                                                 lab_url));
    EXPECT_EQ(jwks(second_id).error.code, "workspace_not_ready");
    EXPECT_EQ(rotate(second_id, lab).error.code, "workspace_not_ready");
    EXPECT_EQ(list_webhooks(second_id, lab).error.code, "workspace_not_ready");
    EXPECT_EQ(surface
                  .call("issue_application_jwt",
                        {{"application_id", second_id}, {"user_id", lake::control::new_uuid_v4()}})
                  .ok,
              false);
    EXPECT_EQ(surface.records(lab_url, "auth_app_signing_keys").size(), 1U);
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Ready", "", lab_url));

    // Deleting a workspace takes its application's keys and webhooks with it; the other workspace
    // keeps its own.
    ASSERT_TRUE(surface.store.update_workspace(lab, {.desired = "deleted"}));
    EXPECT_FALSE(jwks(second_id).ok);
    EXPECT_EQ(list_webhooks(second_id, lab).error.path, "/workspace_id");
    EXPECT_EQ(jwks(first_id).value.at("keys").size(), 2U);
    EXPECT_EQ(list_webhooks(first_id, home).value.at("items").size(), 1U);
}

// lake#351: an application's settings (name, allowed origins, redirect URIs, theme) live in the
// workspace Zetta of its workspace, among two spaces with several workspaces each. Lake's
// PostgreSQL keeps only the content-free routing row that finds that workspace.
TEST(ApplicationAuth, SettingsLiveInTheApplicationsWorkspace) {
    ApplicationSurface surface;
    const std::string home = surface.workspace_id;
    const std::string lab = surface.workspaces->add(surface.owned, surface.customer_id, "lab");
    const auto other_owner = surface.store.create_user(
        {"taap:1:ed25519:settings-other", "settings-other@example.test", "Other"});
    ASSERT_TRUE(other_owner);
    const auto other_customer =
        surface.store.create_customer({"individual", other_owner->id, "", "settings-other"});
    ASSERT_TRUE(other_customer);
    const std::string theirs = surface.workspaces->add(surface.owned, other_customer->id, "theirs");
    const std::string theirs_too =
        surface.workspaces->add(surface.owned, other_customer->id, "theirs-too");

    const ApiResult first = surface.register_application("Client one", home);
    const ApiResult second = surface.register_application("Client two", lab);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(first.value.at("name"), "Client one") << "registration answers the settings";
    const auto their_app = surface.call(
        "register_application",
        {{"customer_id", other_customer->id},
         {"workspace_id", theirs_too},
         {"name", "Their client"},
         {"allowed_origins", json::array({"https://their.example.test"})},
         {"redirect_uris", json::array({"https://their.example.test/callback"})}});
    ASSERT_TRUE(their_app.ok) << their_app.error.message;
    const std::string first_id = first.value.at("id");
    const std::string second_id = second.value.at("id");
    const std::string their_id = their_app.value.at("id");

    // Each application's settings are one sealed record at its own workspace endpoint only,
    // keyed by the application; the routing row holds none of them.
    const std::string home_url = "http://workspace.test";
    const std::string lab_url = "http://lab.test";
    ASSERT_EQ(surface.records(home_url, "auth_applications").size(), 1U);
    EXPECT_EQ(surface.records(home_url, "auth_applications").begin()->first, first_id);
    EXPECT_EQ(surface.records(lab_url, "auth_applications").size(), 1U);
    EXPECT_EQ(surface.records("http://theirs-too.test", "auth_applications").size(), 1U);
    EXPECT_TRUE(surface.records("http://theirs.test", "auth_applications").empty());
    for (const auto& [key, record] : surface.records(home_url, "auth_applications")) {
        for (const std::string plain :
             {"Client one", "client.example.test", "#123456", "accent_color"}) {
            EXPECT_EQ(record.dump().find(plain), std::string::npos) << "sealed body: " << plain;
        }
    }
    const auto routing = surface.store.get_application(first_id);
    ASSERT_TRUE(routing);
    EXPECT_TRUE(routing->name.empty());
    EXPECT_TRUE(routing->allowed_origins.empty());
    EXPECT_TRUE(routing->redirect_uris.empty());
    EXPECT_TRUE(routing->theme.empty());
    EXPECT_EQ(routing->workspace_id, home);
    EXPECT_EQ(routing->custom_sign_in_domain, "login.client.example.test");

    // Reads fill the settings from the workspace; listing reads each workspace once.
    const auto read = [&](const std::string& id) {
        return surface.call("get_application", {{"id", id}});
    };
    const auto got = read(first_id);
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value.at("name"), "Client one");
    EXPECT_EQ(got.value.at("allowed_origins"), json::array({"https://client.example.test"}));
    EXPECT_EQ(got.value.at("redirect_uris"),
              json::array({"https://client.example.test/callback"}));
    EXPECT_EQ(got.value.at("theme").at("accent_color"), "#123456");
    EXPECT_EQ(got.value.at("workspace_id"), home);
    EXPECT_EQ(read(their_id).value.at("name"), "Their client");
    const auto listed = surface.call("list_applications", {{"customer_id", surface.customer_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("items").size(), 2U);
    for (const json& item : listed.value.at("items")) {
        EXPECT_FALSE(item.contains("settings_unavailable")) << item;
        EXPECT_EQ(item.at("name"), item.at("id") == first_id ? "Client one" : "Client two");
    }

    // The theme is changed in the workspace, conditionally on the revision read. Missing,
    // malformed, unknown and foreign workspace ids are refused at /workspace_id.
    const auto theme = [&](const std::string& id, json workspace, std::string color) {
        json input{{"application_id", id}, {"theme", {{"accent_color", color}}}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return surface.call("update_application_theme", input);
    };
    for (const json& wrong : {json(lab), json(theirs), json(lake::control::new_uuid_v4()),
                              json("not-a-workspace"), json(nullptr)}) {
        const auto refused = theme(first_id, wrong, "#000000");
        EXPECT_EQ(refused.error.code, "invalid_input") << wrong;
        EXPECT_EQ(refused.error.path, "/workspace_id") << wrong;
    }
    const auto themed = theme(first_id, home, "#654321");
    ASSERT_TRUE(themed.ok) << themed.error.message;
    EXPECT_EQ(themed.value.at("theme").at("accent_color"), "#654321");
    EXPECT_EQ(themed.value.at("name"), "Client one") << "the rest of the settings kept";
    EXPECT_EQ(read(first_id).value.at("theme").at("accent_color"), "#654321");
    EXPECT_EQ(read(second_id).value.at("theme").at("accent_color"), "#123456");

    auto records =
        lake::api::workspace_records(surface.context, surface.customer_id, surface.workspace_id);
    ASSERT_TRUE(records) << records.error().message;
    int changes = 0;
    const auto raced = lake::control::settled(
        liquid::runtime::block_on(lake::control::update_application_settings(
            *records, first_id, surface.customer_id,
            [&](lake::control::ApplicationSettings& settings) {
                if (++changes == 1) {
                    // Another lake process changes the theme between this read and this write.
                    const auto other = lake::control::settled(liquid::runtime::block_on(
                        lake::control::update_application_settings(
                            *records, first_id, surface.customer_id,
                            [](lake::control::ApplicationSettings& value) {
                                value.theme["background_color"] = "#fafafa";
                            })));
                    EXPECT_TRUE(other);
                }
                settings.theme["accent_color"] = "#111111";
            })));
    ASSERT_TRUE(raced) << raced.error().message;
    EXPECT_EQ(changes, 2) << "the conflicting write was reread";
    EXPECT_EQ(raced->theme.at("accent_color"), "#111111");
    EXPECT_EQ(raced->theme.at("background_color"), "#fafafa") << "neither change lost";
    EXPECT_EQ(raced->lake_revision, 4);

    // A cross-origin sign-in is allowed only from the origins the workspace record lists.
    surface.context.caller_origin = "https://client.example.test";
    const auto allowed = surface.call("sign_up_password",
                                      {{"pool_id", first.value.at("pool_id")},
                                       {"email", "settings-user@example.test"},
                                       {"password", "correct horse battery staple"},
                                       {"display_name", "Settings user"},
                                       {"identity_document", "{}"}});
    EXPECT_TRUE(allowed.ok) << allowed.error.message;
    surface.context.caller_origin = "https://elsewhere.example.test";
    const auto foreign_origin = surface.call("sign_in_password",
                                             {{"pool_id", first.value.at("pool_id")},
                                              {"email", "settings-user@example.test"},
                                              {"password", "correct horse battery staple"}});
    EXPECT_EQ(foreign_origin.error.code, "unauthorized");
    surface.context.caller_origin.clear();

    // Least privilege: a reader of one application reads its settings but cannot change them,
    // and cannot read another application's.
    const auto reader = surface.store.create_user(
        {"taap:1:ed25519:settings-reader", "settings-reader@example.test", "Reader"});
    ASSERT_TRUE(reader);
    const auto object = surface.store.find_policy_object("application", first_id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant({reader->taap_id, object->id, "read",
                                            "settings-reader-attestation", "admin",
                                            "settings-reader-request"}));
    surface.context.operator_credential = false;
    surface.context.acting_identity = reader->taap_id;
    EXPECT_TRUE(read(first_id).ok);
    EXPECT_EQ(read(second_id).error.code, "unauthorized");
    EXPECT_EQ(theme(first_id, home, "#222222").error.code, "unauthorized");
    surface.context.operator_credential = true;
    surface.context.acting_identity = "admin";

    // A record moved under another application's key does not open as that application's.
    auto& rows = surface.records(home_url, "auth_applications");
    const json kept = rows[first_id];
    const auto third = surface.register_application("Client one", home);
    ASSERT_FALSE(third.ok) << "the custom sign-in host is unique";
    const auto moved_to = surface.call(
        "register_application",
        {{"customer_id", surface.customer_id},
         {"workspace_id", home},
         {"name", "Client three"},
         {"allowed_origins", json::array({"https://three.example.test"})},
         {"redirect_uris", json::array()}});
    ASSERT_TRUE(moved_to.ok) << moved_to.error.message;
    EXPECT_TRUE(surface.records(home_url, "auth_applications").size() == 2U)
        << "a refused registration leaves nothing in the workspace";
    const std::string third_id = moved_to.value.at("id");
    rows[third_id]["value"] = kept["value"];
    EXPECT_EQ(read(third_id).error.code, "not_found");
    EXPECT_EQ(read(first_id).value.at("name"), "Client one");

    // A workspace that is not ready refuses the settings instead of falling back; the listing
    // still names the application, with the reason.
    const auto lab_row = surface.store.get_workspace(lab);
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Provisioning", "",
                                                 lab_url));
    EXPECT_EQ(read(second_id).error.code, "workspace_not_ready");
    EXPECT_EQ(theme(second_id, lab, "#333333").error.code, "workspace_not_ready");
    const auto partial = surface.call("list_applications", {{"customer_id", surface.customer_id}});
    ASSERT_TRUE(partial.ok) << partial.error.message;
    for (const json& item : partial.value.at("items")) {
        // The third application's record was replaced above and no longer opens.
        EXPECT_EQ(item.contains("settings_unavailable"),
                  item.at("id") == second_id || item.at("id") == third_id)
            << item;
    }
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Ready", "", lab_url));
    EXPECT_EQ(read(second_id).value.at("name"), "Client two");

    // Deleting a workspace takes its applications' settings with it; the others keep theirs.
    ASSERT_TRUE(surface.store.update_workspace(lab, {.desired = "deleted"}));
    EXPECT_FALSE(read(second_id).ok);
    EXPECT_EQ(theme(second_id, lab, "#444444").error.path, "/workspace_id");
    EXPECT_EQ(read(first_id).value.at("name"), "Client one");
    EXPECT_EQ(read(their_id).value.at("name"), "Their client");
}

// A rotation is one write conditional on the revision it read: a concurrent rotation is reread and
// the next version made, so there is always exactly one current key and none is lost.
TEST(ApplicationAuth, AConcurrentRotationIsRereadNotLost) {
    ApplicationSurface surface;
    const ApiResult application = surface.register_application("Client one");
    ASSERT_TRUE(application.ok) << application.error.message;
    const std::string id = application.value.at("id");
    auto records = lake::api::workspace_records(surface.context, surface.customer_id,
                                                surface.workspace_id);
    ASSERT_TRUE(records) << records.error().message;
    const auto make = [&](int version)
        -> std::expected<lake::control::ApplicationSigningKey, lake::control::ZettaControlError> {
        auto key = lake::control::apps::generate_signing_key(id, version);
        key->encrypted_seed = *surface.store.encrypt_application_credential(
            "application.jwt." + id, key->seed);
        return *key;
    };
    int calls = 0;
    auto rotated = lake::control::settled(liquid::runtime::block_on(
        lake::control::rotate_application_signing_keys(*records, id, [&](int version) {
            if (++calls == 1) {
                // Another lake process rotates between this read and this write.
                const auto raced = lake::control::settled(liquid::runtime::block_on(
                    lake::control::rotate_application_signing_keys(*records, id, make)));
                EXPECT_TRUE(raced);
                EXPECT_EQ(raced->version, 2);
            }
            return make(version);
        })));
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(calls, 2) << "the conflicting write was reread";
    EXPECT_EQ(rotated->version, 3);
    const auto ring = lake::control::settled(
        liquid::runtime::block_on(lake::control::get_application_signing_keys(*records, id)));
    ASSERT_TRUE(ring && *ring);
    ASSERT_EQ((*ring)->keys.size(), 3U);
    EXPECT_EQ(std::ranges::count_if((*ring)->keys, &lake::control::ApplicationSigningKey::current), 1);
    EXPECT_TRUE((*ring)->keys.back().current);
    EXPECT_EQ((*ring)->lake_revision, 3);

    // A stale write is refused by the engine: conditional on the revision read, never last-writer.
    auto stale = **ring;
    const auto first_write = lake::control::settled(liquid::runtime::block_on(records->put(
        lake::control::ApplicationSigningKeysCodec::collection, id,
        lake::control::ApplicationSigningKeysCodec::lookup(stale),
        lake::control::ApplicationSigningKeysCodec::encode(stale),
        lake::control::RecordCondition{.if_revision = stale.revision}, stale.lake_revision + 1,
        stale.at_us)));
    ASSERT_TRUE(first_write) << first_write.error().message;
    const auto second_write = lake::control::settled(liquid::runtime::block_on(records->put(
        lake::control::ApplicationSigningKeysCodec::collection, id,
        lake::control::ApplicationSigningKeysCodec::lookup(stale),
        lake::control::ApplicationSigningKeysCodec::encode(stale),
        lake::control::RecordCondition{.if_revision = stale.revision}, stale.lake_revision + 1,
        stale.at_us)));
    ASSERT_FALSE(second_write);
    EXPECT_EQ(second_write.error().code, "conflict");

    // A ring copied into another application's key does not open as that application's.
    const auto other = surface.register_application("Client two");
    ASSERT_TRUE(other.ok);
    auto& rows = surface.records("http://workspace.test", "auth_app_signing_keys");
    rows[other.value.at("id")]["value"] = rows[id]["value"];
    const auto moved = lake::control::settled(liquid::runtime::block_on(
        lake::control::get_application_signing_keys(*records, other.value.at("id"))));
    ASSERT_TRUE(moved);
    EXPECT_FALSE(*moved);
}

// lake#363: an application's one-use authorization codes live in the workspace Zetta of its
// workspace, among two spaces with two workspaces each. Issue writes `if_absent`; the relying
// party's exchange (application id and code only) finds the workspace from the routing row and
// redeems by a delete at the revision read, so a code is redeemed at most once; the collection's
// retention expires codes.
TEST(ApplicationAuth, AuthorizationCodesLiveInTheApplicationsWorkspace) {
    ApplicationSurface surface;
    const std::string home = surface.workspace_id;
    const std::string lab = surface.workspaces->add(surface.owned, surface.customer_id, "lab");
    const auto other_owner = surface.store.create_user(
        {"taap:1:ed25519:code-other", "code-other@example.test", "Other"});
    ASSERT_TRUE(other_owner);
    const auto other_customer =
        surface.store.create_customer({"individual", other_owner->id, "", "code-other"});
    ASSERT_TRUE(other_customer);
    const std::string theirs = surface.workspaces->add(surface.owned, other_customer->id, "theirs");
    const std::string theirs_too =
        surface.workspaces->add(surface.owned, other_customer->id, "theirs-too");
    const ApiResult first = surface.register_application("Client one", home);
    const ApiResult second = surface.register_application("Client two", lab);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_TRUE(second.ok) << second.error.message;
    const auto their_app = surface.call(
        "register_application",
        {{"customer_id", other_customer->id},
         {"workspace_id", theirs_too},
         {"name", "Their client"},
         {"allowed_origins", json::array({"https://their.example.test"})},
         {"redirect_uris", json::array({"https://their.example.test/callback"})}});
    ASSERT_TRUE(their_app.ok) << their_app.error.message;
    const std::string first_id = first.value.at("id");
    const std::string second_id = second.value.at("id");
    const std::string their_id = their_app.value.at("id");

    // The signed-in lake user authorizes; the relying party's backend exchanges anonymously.
    const auto person = surface.store.create_user(
        {"taap:1:ed25519:code-person", "code-person@example.test", "Code person"});
    ASSERT_TRUE(person);
    surface.context.acting_user_id = person->id;
    const std::string verifier = "authorization-code-verifier-0123456789abcdef";
    const auto authorize = [&](const std::string& application_id, const std::string& redirect) {
        return surface.call(
            "begin_application_authorization",
            {{"application_id", application_id},
             {"redirect_uri", redirect},
             {"code_challenge", lake::control::identity_crypto::sha256_base64url(verifier)},
             {"code_challenge_method", "S256"},
             {"state", "state-1"},
             {"nonce", "nonce-1"}});
    };
    const auto exchange = [&](const json& application_id, const json& code,
                              const std::string& redirect = "https://client.example.test/callback") {
        lake::api::ApiContext anonymous;
        anonymous.store = &surface.store;
        anonymous.workspaces = surface.context.workspaces;
        anonymous.instance_kek = surface.context.instance_kek;
        anonymous.public_host = surface.context.public_host;
        anonymous.request_id = lake::control::new_uuid_v4();
        json input{{"code", code}, {"redirect_uri", redirect}, {"code_verifier", verifier}};
        if (!application_id.is_null()) {
            input["application_id"] = application_id;
        }
        auto completed = liquid::runtime::block_on(surface.registry.call_async(
            "exchange_application_authorization_code", input, anonymous));
        EXPECT_TRUE(completed);
        return completed ? *completed : ApiResult::failure("store_error", "did not complete");
    };
    const std::string home_url = "http://workspace.test";
    const std::string lab_url = "http://lab.test";

    // Issued only into the application's own workspace, sealed and under a blind key: neither the
    // code, nor its hash, nor the redirect or nonce is readable there, and the control store keeps
    // no row. The collection is declared with the code lifetime as its retention.
    const ApiResult issued = authorize(first_id, "https://client.example.test/callback");
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const std::string code = issued.value.at("code");
    const std::string code_hash = lake::control::identity_crypto::sha256_hex(code);
    ASSERT_EQ(surface.records(home_url, "auth_app_authorization_codes").size(), 1U);
    for (const auto& [key, record] : surface.records(home_url, "auth_app_authorization_codes")) {
        EXPECT_EQ(key.find(code_hash), std::string::npos) << "blind key";
        for (const std::string& secret : {code, code_hash, std::string("client.example.test"),
                                          std::string("nonce-1"), person->id}) {
            EXPECT_EQ(record.dump().find(secret), std::string::npos) << secret;
        }
    }
    EXPECT_EQ(surface.workspaces->caller->records[home_url]
                  .collections.at("lake_auth_app_authorization_codes")
                  .retention_ms,
              lake::control::ApplicationAuthorizationCodeCodec::lifetime_ms);
    for (const std::string url : {lab_url, std::string("http://theirs.test"),
                                  std::string("http://theirs-too.test")}) {
        EXPECT_TRUE(surface.records(url, "auth_app_authorization_codes").empty()) << url;
    }
    EXPECT_EQ(authorize(first_id, "https://evil.example.test/callback").error.path,
              "/redirect_uri");

    // Another application's id finds another workspace, where the code is not: refused, and the
    // code is still redeemable through its own application exactly once.
    EXPECT_EQ(exchange(second_id, code).error.code, "unauthorized");
    EXPECT_EQ(exchange(their_id, code).error.code, "unauthorized");
    const ApiResult tokens = exchange(first_id, code);
    ASSERT_TRUE(tokens.ok) << tokens.error.message;
    EXPECT_FALSE(tokens.value.at("id_token").get<std::string>().empty());
    EXPECT_TRUE(surface.records(home_url, "auth_app_authorization_codes").empty())
        << "redeemed by deleting it";
    const ApiResult replay = exchange(first_id, code);
    EXPECT_EQ(replay.error.code, "unauthorized") << "a code is redeemed once";

    // Two exchanges racing for one code: the other's delete lands between this one's read and its
    // conditional delete. Only one gets tokens.
    const auto race = [&](std::function<void(lake::test::RecordTables&)> other) {
        const ApiResult raced = authorize(first_id, "https://client.example.test/callback");
        EXPECT_TRUE(raced.ok) << raced.error.message;
        auto& tables = surface.workspaces->caller->records[home_url];
        const std::string key = tables.rows("auth_app_authorization_codes").begin()->first;
        tables.before_delete[key] = std::move(other);
        return exchange(first_id, raced.value.at("code"));
    };
    const auto lost = race([](lake::test::RecordTables& tables) {
        tables.rows("auth_app_authorization_codes").clear(); // the other exchange redeemed it
    });
    EXPECT_EQ(lost.error.code, "unauthorized");
    const auto stale = race([](lake::test::RecordTables& tables) {
        for (auto& [key, record] : tables.rows("auth_app_authorization_codes")) {
            record["revision"] = record.at("revision").get<std::int64_t>() + 1000;
        }
    });
    EXPECT_EQ(stale.error.code, "unauthorized") << "a delete at a stale revision is refused";
    surface.records(home_url, "auth_app_authorization_codes").clear();

    // An expired code is refused, and removed.
    auto records = lake::api::workspace_records(surface.context, surface.customer_id, home);
    ASSERT_TRUE(records) << records.error().message;
    lake::control::ApplicationAuthorizationCode old;
    old.application_id = first_id;
    old.user_id = person->id;
    old.redirect_uri = "https://client.example.test/callback";
    old.code_hash = lake::control::identity_crypto::sha256_hex("an-old-code");
    old.code_challenge = lake::control::identity_crypto::sha256_base64url(verifier);
    old.nonce = "nonce-old";
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    old.at_us = now_us - 6LL * 60 * 1'000'000;
    const auto written = lake::control::settled(liquid::runtime::block_on(
        lake::control::issue_application_authorization_code(*records, old)));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written->expires_us, old.at_us + 5LL * 60 * 1'000'000);
    const auto twice = lake::control::settled(liquid::runtime::block_on(
        lake::control::issue_application_authorization_code(*records, old)));
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().code, "conflict") << "issued if_absent";
    EXPECT_EQ(exchange(first_id, "an-old-code").error.code, "unauthorized");
    EXPECT_TRUE(surface.records(home_url, "auth_app_authorization_codes").empty());

    // A code copied under another application's key does not open as that application's.
    const ApiResult moved = authorize(first_id, "https://client.example.test/callback");
    ASSERT_TRUE(moved.ok);
    const ApiResult second_code = authorize(second_id, "https://client.example.test/callback");
    ASSERT_TRUE(second_code.ok);
    {
        auto& rows = surface.records(home_url, "auth_app_authorization_codes");
        const auto& value = rows.begin()->second.at("value");
        auto& lab_rows = surface.records(lab_url, "auth_app_authorization_codes");
        lab_rows.begin()->second["value"] = value;
    }
    EXPECT_EQ(exchange(second_id, second_code.value.at("code")).error.code, "unauthorized");
    EXPECT_TRUE(exchange(first_id, moved.value.at("code")).ok);

    // Missing, malformed and unknown application ids are refused at /application_id.
    for (const json& wrong : {json(nullptr), json("not-an-application"),
                              json(lake::control::new_uuid_v4())}) {
        const auto refused = exchange(wrong, code);
        EXPECT_FALSE(refused.ok) << wrong;
        EXPECT_EQ(refused.error.path, "/application_id") << wrong;
    }

    // Least privilege: a reader of the application cannot issue a code for it.
    const auto reader = surface.store.create_user(
        {"taap:1:ed25519:code-reader", "code-reader@example.test", "Reader"});
    ASSERT_TRUE(reader);
    const auto object = surface.store.find_policy_object("application", first_id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant({reader->taap_id, object->id, "read",
                                            "code-reader-attestation", "admin",
                                            "code-reader-request"}));
    surface.context.operator_credential = false;
    surface.context.acting_identity = reader->taap_id;
    surface.context.acting_user_id = reader->id;
    EXPECT_EQ(authorize(first_id, "https://client.example.test/callback").error.code,
              "unauthorized");
    surface.context.operator_credential = true;
    surface.context.acting_identity = "admin";
    surface.context.acting_user_id = person->id;

    // A workspace that is not ready refuses issue and exchange instead of falling back.
    const ApiResult pending = authorize(second_id, "https://client.example.test/callback");
    ASSERT_TRUE(pending.ok) << pending.error.message;
    const auto lab_row = surface.store.get_workspace(lab);
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Provisioning", "",
                                                 lab_url));
    EXPECT_EQ(authorize(second_id, "https://client.example.test/callback").error.code,
              "workspace_not_ready");
    EXPECT_EQ(exchange(second_id, pending.value.at("code")).error.code, "workspace_not_ready");
    ASSERT_TRUE(surface.store.set_service_status(lab_row->zetta_service_id, "Ready", "", lab_url));

    // Deleting a workspace takes its codes with it; the other workspace keeps its own.
    const ApiResult kept = authorize(first_id, "https://client.example.test/callback");
    ASSERT_TRUE(kept.ok);
    ASSERT_TRUE(surface.store.update_workspace(lab, {.desired = "deleted"}));
    EXPECT_FALSE(exchange(second_id, pending.value.at("code")).ok);
    EXPECT_TRUE(exchange(first_id, kept.value.at("code")).ok);
}

} // namespace
