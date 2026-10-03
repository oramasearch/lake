#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/memory_store.hpp"
#include "api/workspace_records_api.hpp"
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
    EXPECT_TRUE(
        surface.call("list_application_users", {{"application_id", first.value.at("id")}}).ok);
    const ApiResult denied =
        surface.call("list_application_users", {{"application_id", second.value.at("id")}});
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
                                            {"url", "https://hooks.example.test/lifecycle"},
                                            {"events", json::array({"organization.created"})}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;
    ASSERT_TRUE(surface
                    .call("create_application_organization",
                          {{"application_id", application_id},
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

} // namespace
