#include "lake/api/apps.hpp"
#include "lake/api/authentication_work.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <map>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "lake/control/application_authorization_codes.hpp"
#include "lake/control/application_credential_records.hpp"
#include "lake/control/application_settings_records.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/identity_crypto.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 8> lifecycle_events = {
    "user.created",
    "user.updated",
    "user.deleted",
    "organization.created",
    "organization.updated",
    "organization.deleted",
    "session.started",
    "mfa.changed",
};

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable",
                              "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

json input_schema(json properties, json required = json::array()) {
    json schema = {
        {"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

json object_schema() {
    return {{"type", "object"}};
}

json items_schema() {
    return {{"type", "object"}, {"properties", {{"items", {{"type", "array"}}}}}};
}

int limit_of(const json& input) {
    return std::clamp(input.value("limit", 100), 1, 1000);
}

template <class T> ApiResult one_result(control::Result<T> value) {
    if (!value) {
        return from_store_error(value.error());
    }
    return ApiResult::success(control::to_json(*value));
}

template <class T> ApiResult list_result(control::Result<std::vector<T>> values) {
    if (!values) {
        return from_store_error(values.error());
    }
    json items = json::array();
    for (const T& value : *values) {
        items.push_back(control::to_json(value));
    }
    return ApiResult::success({{"items", std::move(items)}});
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_kind = "application";
    context.audit_object = "customer/" + customer_id + "/applications";
    context.audit_details = {{"customer_id", customer_id}};
    auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> application_object(const json& input, ApiContext& context) {
    const std::string application_id = input.at("application_id");
    auto application = context.store->get_application(application_id);
    if (!application) {
        return std::unexpected(
            ApiError{application.error().code, application.error().message, "/application_id"});
    }
    context.audit_customer_id = application->customer_id;
    context.audit_kind = "application";
    context.audit_object = "application/" + application_id;
    context.audit_details = {{"application_id", application_id}};
    auto object = context.store->find_policy_object("application", application_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/application_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> application_id_object(const json& input, ApiContext& context) {
    json normalized = input;
    normalized["application_id"] = input.at("id");
    return application_object(normalized, context);
}

std::optional<ApiResult> authorize_application_list(const json& input, ApiContext& context) {
    const auto object = application_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return from_store_error(decision.error());
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read application");
    }
    return std::nullopt;
}

bool valid_https_url(const std::string& value, bool allow_local_http) {
    if (value.starts_with("https://")) {
        return value.size() > 8;
    }
    return allow_local_http &&
           (value.starts_with("http://localhost") || value.starts_with("http://127.0.0.1"));
}

std::optional<ApiResult> validate_registration(const json& input) {
    const auto origins = input.at("allowed_origins").get<std::vector<std::string>>();
    const auto redirects = input.at("redirect_uris").get<std::vector<std::string>>();
    if (origins.empty()) {
        return ApiResult::failure(
            "invalid_input", "at least one allowed origin is required", "/allowed_origins");
    }
    if (std::ranges::any_of(origins, [](const std::string& value) {
            return !valid_https_url(value, true) || value.ends_with('/');
        })) {
        return ApiResult::failure("invalid_input",
                                  "allowed origins must be HTTPS origins without a trailing slash",
                                  "/allowed_origins");
    }
    if (std::ranges::any_of(
            redirects, [](const std::string& value) { return !valid_https_url(value, true); })) {
        return ApiResult::failure(
            "invalid_input", "redirect URIs must use HTTPS", "/redirect_uris");
    }
    const std::string domain = input.value("custom_sign_in_domain", "");
    if (!domain.empty() && (domain.contains('/') || domain.contains(':') || domain.contains(' '))) {
        return ApiResult::failure(
            "invalid_input", "custom sign-in domain must be a host name", "/custom_sign_in_domain");
    }
    return std::nullopt;
}

bool accepts_event(const control::ApplicationWebhook& webhook, std::string_view event) {
    return webhook.enabled &&
           std::ranges::find(webhook.events, std::string(event)) != webhook.events.end();
}

// The records of the application's workspace, where its events and delivery attempts live
// (lake#356, lake#357). Never anywhere else: an application with no workspace yet, or a workspace
// that is not ready, is refused.
std::expected<control::WorkspaceRecords, ApiResult>
application_records(const control::Application& application, const ApiContext& context) {
    if (application.workspace_id.empty()) {
        return std::unexpected(ApiResult::failure(
            "workspace_not_ready", "application " + application.id +
                                       " has no workspace yet; its events, keys and webhooks are "
                                       "unavailable until an operator assigns one (migration 0049)"));
    }
    auto records = workspace_records(context, application.customer_id, application.workspace_id);
    if (!records) {
        return std::unexpected(
            ApiResult::failure(records.error().code, records.error().message, records.error().path));
    }
    return std::move(*records);
}

// A pending attempt this old was claimed by a caller that stopped before recording the outcome:
// an outbound exchange times out after 30 seconds.
constexpr std::int64_t abandoned_claim_us = 5LL * 60 * 1'000'000;

ApiResult from_records_error(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

// The settings of a new application, from register_application's input.
control::ApplicationSettings registration_settings(const std::string& id, const json& input) {
    control::ApplicationSettings settings;
    settings.application_id = id;
    settings.customer_id = input.at("customer_id");
    settings.name = input.at("name");
    settings.allowed_origins = input.at("allowed_origins").get<std::vector<std::string>>();
    settings.redirect_uris = input.at("redirect_uris").get<std::vector<std::string>>();
    settings.theme = input.value("theme", json::object());
    return settings;
}

// The secrets subsystem's envelope purposes of an application's credentials (lake#352, lake#355).
std::string signing_key_purpose(const std::string& application_id) {
    return "application.jwt." + application_id;
}
std::string webhook_secret_purpose(const std::string& webhook_id) {
    return "application.webhook." + webhook_id;
}

// The current signing key of an application, read from the application's workspace records and
// opened for signing (lake#352). Every token issuer calls it on its asynchronous path.
liquid::runtime::task<std::expected<control::ApplicationSigningKey, ApiResult>>
application_signing_key(control::WorkspaceRecords& records, std::string application_id,
                        ApiContext& context) {
    using Key = std::expected<control::ApplicationSigningKey, ApiResult>;
    auto ring = control::settled(
        co_await control::get_application_signing_keys(records, application_id));
    if (!ring) {
        co_return Key(std::unexpected(from_records_error(ring.error())));
    }
    if (!*ring) {
        co_return Key(std::unexpected(ApiResult::failure(
            "not_found", "application has no current signing key in its workspace")));
    }
    const auto current =
        std::ranges::find_if((*ring)->keys, &control::ApplicationSigningKey::current);
    control::ApplicationSigningKey key = *current; // the codec admits only rings with one
    auto seed = context.store->decrypt_application_credential(signing_key_purpose(application_id),
                                                               key.encrypted_seed);
    if (!seed) {
        co_return Key(std::unexpected(from_store_error(seed.error())));
    }
    key.seed = std::move(*seed);
    co_return Key(std::move(key));
}

// The webhook with its signing secret opened, for signing one delivery.
std::expected<control::ApplicationWebhook, ApiResult>
opened_webhook(control::ApplicationWebhook webhook, ApiContext& context) {
    auto secret = context.store->decrypt_application_credential(
        webhook_secret_purpose(webhook.id), webhook.encrypted_secret);
    if (!secret) {
        return std::unexpected(from_store_error(secret.error()));
    }
    webhook.secret = std::move(*secret);
    return webhook;
}

// Sends `event` to `webhook` up to three consecutive attempts from `first_attempt`, stopping at the
// first 2xx. Each attempt is claimed in the workspace before it is sent and its outcome recorded
// after: a caller whose claim conflicts sends nothing, so an attempt is never sent twice.
liquid::runtime::task<ApiResult> deliver_to_webhook(control::WorkspaceRecords& records,
                                                    const control::ApplicationWebhookEvent& event,
                                                    const control::ApplicationWebhook& webhook,
                                                    int first_attempt,
                                                    ApiContext& context) {
    const json body = {{"id", event.id},
                       {"application_id", event.application_id},
                       {"event", event.event},
                       {"created_at", event.created_at},
                       {"data", event.payload}};
    const std::string payload = body.dump();
    const std::string signature =
        control::apps::webhook_signature(webhook.secret, event.id, payload);
    control::ApplicationWebhookDelivery last;
    for (int attempt = first_attempt; attempt < first_attempt + 3; ++attempt) {
        control::ApplicationWebhookDelivery delivery;
        delivery.application_id = event.application_id;
        delivery.event_id = event.id;
        delivery.webhook_id = webhook.id;
        delivery.attempt = attempt;
        delivery.signature = signature;
        delivery.pending = true;
        auto claimed = control::settled(
            co_await control::claim_application_webhook_delivery(records, std::move(delivery)));
        if (!claimed) {
            if (claimed.error().code == "conflict") {
                co_return ApiResult::failure(
                    "conflict", "attempt " + std::to_string(attempt) +
                                    " of this event to this webhook is already being delivered");
            }
            co_return from_records_error(claimed.error());
        }
        WebhookDeliveryResult sent;
        if (context.webhook_sender) {
            // On the blocking pool, never on the server's own thread (lake#772).
            auto ran = co_await run_blocking([sender = context.webhook_sender,
                                              url = webhook.url,
                                              headers = json{{"Content-Type", "application/json"},
                                                             {"X-Lake-Event", event.event},
                                                             {"X-Lake-Event-Id", event.id},
                                                             {"X-Lake-Signature", signature}},
                                              payload] { return sender(url, headers, payload); });
            sent = ran ? std::move(*ran) : WebhookDeliveryResult{.error = ran.error().message};
        } else {
            sent.error = "webhook sender is not configured";
        }
        claimed->delivery.status = sent.status;
        claimed->delivery.error = sent.error;
        auto recorded = control::settled(
            co_await control::settle_application_webhook_delivery(records, std::move(*claimed)));
        if (!recorded) {
            co_return from_records_error(recorded.error());
        }
        last = std::move(*recorded);
        if (sent.status >= 200 && sent.status < 300) {
            break;
        }
    }
    co_return ApiResult::success(control::to_json(last));
}

// Records one event in the application's workspace and delivers it to each subscribed webhook.
liquid::runtime::task<ApiResult> record_and_deliver(control::WorkspaceRecords& records,
                                                    control::ApplicationWebhookEvent event,
                                                    ApiContext& context) {
    // The application's webhooks live in the same workspace (lake#355).
    auto webhooks = control::settled(
        co_await control::list_application_webhooks(records, event.application_id));
    if (!webhooks) {
        co_return from_records_error(webhooks.error());
    }
    control::ApplicationWebhookEvents events(records);
    auto stored = control::settled(co_await events.append(std::move(event)));
    if (!stored) {
        co_return from_records_error(stored.error());
    }
    for (const control::ApplicationWebhook& listed : *webhooks) {
        if (!accepts_event(listed, stored->event)) {
            continue;
        }
        auto webhook = opened_webhook(listed, context);
        if (!webhook) {
            co_return webhook.error();
        }
        auto delivered = co_await deliver_to_webhook(records, *stored, *webhook, 1, context);
        if (!delivered) {
            co_return ApiResult::failure("store_error", delivered.error().message);
        }
        if (!delivered->ok) {
            co_return std::move(*delivered);
        }
    }
    co_return ApiResult::success(control::to_json(*stored));
}

// The application named by `application_id`, provided `workspace_id` is the workspace that holds
// its events: anything else is invalid input at /workspace_id, never a read elsewhere.
std::expected<control::Application, ApiResult> application_in_workspace(const json& input,
                                                                        ApiContext& context) {
    auto application = context.store->get_application(input.at("application_id"));
    if (!application) {
        return std::unexpected(from_store_error(application.error()));
    }
    if (application->workspace_id.empty()) {
        return std::unexpected(ApiResult::failure(
            "workspace_not_ready", "application has no workspace yet; an operator assigns one"));
    }
    if (input.at("workspace_id") != application->workspace_id) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "workspace_id is not the workspace of this application",
            "/workspace_id"));
    }
    return std::move(*application);
}

ApiResult completed(std::expected<ApiResult, liquid::error> result) {
    return result ? std::move(*result) : ApiResult::failure("store_error", result.error().message);
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A claim this young may belong to a sign-up or create still in flight, whose holder is not
// written yet; only an older claim whose holder does not exist is abandoned and may be taken over.
constexpr std::int64_t claim_grace_us = 2LL * 60 * 1'000'000;

// Takes the claim of `value` (kind `kind`) for `holder` in the application's workspace, with
// `if_absent`. A claim another holder has is refused as `conflict` naming `what`, unless it is
// abandoned: older than the grace period, with a holder that does not exist (a user lake has no
// account for, or an organization that has no record). nullopt when `holder` holds the claim.
liquid::runtime::task<std::optional<ApiResult>> take_claim(control::WorkspaceRecords& records,
                                                           control::RecordCollection collection,
                                                           std::string application_id,
                                                           std::string_view kind,
                                                           std::string value,
                                                           std::string holder,
                                                           std::string what,
                                                           ApiContext& context) {
    const std::string key =
        control::application_claim_key(records, collection, application_id, kind, value);
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto written = control::settled(co_await control::write_application_claim(
            records, collection, application_id, key, holder));
        if (written) {
            co_return std::nullopt;
        }
        if (written.error().code != "conflict") {
            co_return from_records_error(written.error());
        }
        auto claim =
            control::settled(co_await control::read_application_claim(records, collection, key));
        if (!claim) {
            co_return from_records_error(claim.error());
        }
        if (!*claim) {
            continue; // released meanwhile: claim it again
        }
        if ((*claim)->holder == holder) {
            co_return std::nullopt;
        }
        bool abandoned = now_us() - (*claim)->at_us >= claim_grace_us;
        if (abandoned && collection.name == control::ApplicationUserCodec::collection.name) {
            const auto account = context.store->get_user((*claim)->holder);
            if (!account && account.error().code != "not_found") {
                co_return from_store_error(account.error());
            }
            abandoned = !account;
        } else if (abandoned) {
            auto organization = control::settled(co_await control::get_application_organization(
                records, application_id, (*claim)->holder));
            if (!organization) {
                co_return from_records_error(organization.error());
            }
            abandoned = !*organization;
        }
        if (!abandoned) {
            co_return ApiResult::failure("conflict", what + " already exists in this application");
        }
        auto taken = control::settled(co_await control::write_application_claim(
            records, collection, application_id, key, holder, (*claim)->revision));
        if (taken) {
            co_return std::nullopt;
        }
        if (taken.error().code != "conflict") {
            co_return from_records_error(taken.error());
        }
    }
    co_return ApiResult::failure("conflict", what + " is being claimed by another request");
}

// The application named by `application_id` and its records, provided `workspace_id` names the
// application's workspace (application_in_workspace).
struct ApplicationAndRecords {
    control::Application application;
    control::WorkspaceRecords records;
};
std::expected<ApplicationAndRecords, ApiResult> application_and_records(const json& input,
                                                                        ApiContext& context) {
    auto application = application_in_workspace(input, context);
    if (!application) {
        return std::unexpected(application.error());
    }
    auto records = application_records(*application, context);
    if (!records) {
        return std::unexpected(records.error());
    }
    return ApplicationAndRecords{std::move(*application), std::move(*records)};
}

// The user `user_id` of an application, or not_found.
liquid::runtime::task<std::expected<control::ApplicationUser, ApiResult>> application_user(
    control::WorkspaceRecords& records, std::string application_id, std::string user_id) {
    using Found = std::expected<control::ApplicationUser, ApiResult>;
    auto user =
        control::settled(co_await control::get_application_user(records, application_id, user_id));
    if (!user) {
        co_return Found(std::unexpected(from_records_error(user.error())));
    }
    if (!*user) {
        co_return Found(
            std::unexpected(ApiResult::failure("not_found", "application user does not exist")));
    }
    co_return Found(std::move(**user));
}

// The record of a user who signed in or up in the application's own pool, writing it when the
// account is new: a first OAuth sign-in (`created`), or a password sign-up whose email claim names
// the account (the claim was taken before the account existed, so a sign-up interrupted before its
// record was written completes here). Any other account without a record is not a user of the
// application in its workspace, which is refused.
liquid::runtime::task<std::expected<control::ApplicationUser, ApiResult>>
pool_user(control::WorkspaceRecords& records,
          const control::Application& application,
          std::string user_id,
          bool created,
          ApiContext& context) {
    using Found = std::expected<control::ApplicationUser, ApiResult>;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto existing = control::settled(
            co_await control::get_application_user(records, application.id, user_id));
        if (!existing) {
            co_return Found(std::unexpected(from_records_error(existing.error())));
        }
        if (*existing) {
            co_return Found(std::move(**existing));
        }
        const auto account = context.store->get_user(user_id);
        if (!account) {
            co_return Found(std::unexpected(from_store_error(account.error())));
        }
        bool claimed = false;
        if (!account->email.empty()) {
            const auto collection = control::ApplicationUserCodec::collection;
            if (created) {
                auto refused = co_await take_claim(records,
                                                   collection,
                                                   application.id,
                                                   control::email_claim,
                                                   account->email,
                                                   user_id,
                                                   "email " + account->email,
                                                   context);
                if (!refused) {
                    co_return Found(std::unexpected(
                        ApiResult::failure("store_error", refused.error().message)));
                }
                if (*refused) {
                    co_return Found(std::unexpected(std::move(**refused)));
                }
                claimed = true;
            } else {
                auto claim = control::settled(co_await control::read_application_claim(
                    records,
                    collection,
                    control::application_claim_key(records,
                                                   collection,
                                                   application.id,
                                                   control::email_claim,
                                                   account->email)));
                if (!claim) {
                    co_return Found(std::unexpected(from_records_error(claim.error())));
                }
                claimed = *claim && (*claim)->holder == user_id;
            }
        }
        if (!created && !claimed) {
            co_return Found(std::unexpected(ApiResult::failure(
                "unauthorized",
                "this account is not a user of the application in its workspace "
                "(an operator copies earlier users with copy_workspace_records)")));
        }
        control::ApplicationUser user;
        user.application_id = application.id;
        user.user_id = user_id;
        user.taap_id = account->taap_id;
        user.email = account->email;
        user.display_name = account->display_name;
        user.status = "active";
        user.own_pool = true;
        auto written =
            control::settled(co_await control::create_application_user(records, std::move(user)));
        if (written) {
            co_return Found(std::move(*written));
        }
        if (written.error().code != "conflict") {
            co_return Found(std::unexpected(from_records_error(written.error())));
        }
        // Another sign-in wrote it first: read it.
    }
    co_return Found(
        std::unexpected(ApiResult::failure("conflict", "the application user kept changing")));
}

// Records one event and delivers it, from a coroutine.
liquid::runtime::task<ApiResult> emit_event(const control::Application& application,
                                            std::string_view event,
                                            json data,
                                            ApiContext& context) {
    co_return completed(
        co_await emit_application_event(application, std::string(event), std::move(data), context));
}

liquid::runtime::task<ApiResult> emit_and_return(const control::Application& application,
                                                 std::string_view event,
                                                 json data,
                                                 json value,
                                                 ApiContext& context) {
    const ApiResult emitted =
        completed(co_await emit_event(application, event, std::move(data), context));
    if (!emitted.ok) {
        co_return emitted;
    }
    co_return ApiResult::success(std::move(value));
}

} // namespace

liquid::runtime::task<std::expected<control::Application, ApiResult>>
configured_application(control::Result<control::Application> routing, ApiContext& context) {
    using Found = std::expected<control::Application, ApiResult>;
    if (!routing) {
        co_return Found(std::unexpected(from_store_error(routing.error())));
    }
    auto records = application_records(*routing, context);
    if (!records) {
        co_return Found(std::unexpected(records.error()));
    }
    auto settings = control::settled(co_await control::get_application_settings(
        *records, routing->id, routing->customer_id));
    if (!settings) {
        co_return Found(std::unexpected(from_records_error(settings.error())));
    }
    if (!*settings) {
        co_return Found(std::unexpected(ApiResult::failure(
            "not_found", "application " + routing->id +
                             " has no settings in its workspace yet (an operator copies them "
                             "with copy_workspace_records)")));
    }
    control::Application application = std::move(*routing);
    control::apply_settings(application, **settings);
    co_return Found(std::move(application));
}

std::expected<control::Application, ApiResult>
configured(std::expected<std::expected<control::Application, ApiResult>, liquid::error> completed) {
    if (!completed) {
        return std::unexpected(ApiResult::failure("store_error", completed.error().message));
    }
    return std::move(*completed);
}

liquid::runtime::task<ApiResult> emit_application_event(control::Application application,
                                                        std::string event,
                                                        json data,
                                                        ApiContext& context) {
    if (!context.store) {
        co_return no_store();
    }
    if (std::ranges::find(lifecycle_events, event) == lifecycle_events.end()) {
        co_return ApiResult::failure("invalid_input", "unsupported application lifecycle event");
    }
    auto records = application_records(application, context);
    if (!records) {
        co_return records.error();
    }
    control::ApplicationWebhookEvent value;
    value.application_id = application.id;
    value.event = std::move(event);
    value.payload = std::move(data);
    co_return completed(
        co_await record_and_deliver(*records, std::move(value), context));
}

liquid::runtime::task<ApiResult>
application_session_result(control::Result<control::IssuedSession> issued,
                           ApiContext& context,
                           std::string lifecycle_event) {
    if (!issued) {
        co_return from_store_error(issued.error());
    }
    json output = {{"session", control::to_json(issued->session)}, {"secret", issued->secret}};
    if (issued->session.pool_id == "lake") {
        co_return ApiResult::success(std::move(output));
    }
    // A session issued in an application's pool is returned only to an active user of the
    // application, read in the application's workspace (lake#353). Anything else revokes it: the
    // caller never receives its secret.
    const auto refuse = [&](ApiResult failure) {
        (void)context.store->revoke_session(issued->session.user_id, issued->session.id);
        return failure;
    };
    auto application = context.store->get_application_by_pool(issued->session.pool_id);
    if (!application) {
        co_return refuse(from_store_error(application.error()));
    }
    auto records = application_records(*application, context);
    if (!records) {
        co_return refuse(records.error());
    }
    auto user = co_await pool_user(
        *records, *application, issued->session.user_id, issued->created_user, context);
    if (!user) {
        co_return refuse(ApiResult::failure("store_error", user.error().message));
    }
    if (!*user) {
        co_return refuse(std::move(user->error()));
    }
    if ((*user)->status != "active") {
        co_return refuse(ApiResult::failure("unauthorized", "application user is suspended"));
    }
    if (issued->session.kind != "api_token" && issued->session.kind != "static") {
        auto key = co_await application_signing_key(*records, application->id, context);
        if (!key) {
            co_return refuse(ApiResult::failure("store_error", key.error().message));
        }
        if (!*key) {
            co_return refuse(std::move(key->error()));
        }
        const std::string host = application->custom_sign_in_domain.empty()
                                     ? context.public_host
                                     : application->custom_sign_in_domain;
        const std::string issuer = "https://" + host + "/v1/apps/" + application->id;
        auto token = control::apps::issue_jwt(*application, **key, **user, issuer);
        if (!token) {
            co_return refuse(ApiResult::failure("store_error", token.error()));
        }
        output["access_token"] = *token;
        output["token_type"] = "Bearer";
        output["expires_in"] = 900;
    }
    if (lifecycle_event == "user.created") {
        const ApiResult created = completed(
            co_await emit_event(*application, "user.created", control::to_json(**user), context));
        if (!created.ok) {
            co_return created;
        }
    }
    const ApiResult started = completed(co_await emit_event(
        *application,
        "session.started",
        {{"user", control::to_json(**user)}, {"session", control::to_json(issued->session)}},
        context));
    if (!started.ok) {
        co_return started;
    }
    co_return ApiResult::success(std::move(output));
}

liquid::runtime::task<ApiResult> sign_up_password_session(control::NewPasswordUser user,
                                                          ApiContext& context) {
    if (!context.store) {
        co_return no_store();
    }
    if (user.pool_id.empty() || user.pool_id == "lake") {
        user.pool_id = "lake";
        co_return completed(co_await application_session_result(
            co_await authentication_work(
                [store = context.store, user] { return store->sign_up_password(user); }),
            context,
            "user.created"));
    }
    auto application = context.store->get_application_by_pool(user.pool_id);
    if (!application) {
        co_return ApiResult::failure(
            "invalid_input", "application pool does not exist", "/pool_id");
    }
    if (user.email.empty()) {
        co_return ApiResult::failure("invalid_input", "email is required", "/email");
    }
    auto records = application_records(*application, context);
    if (!records) {
        co_return records.error();
    }
    // The email is claimed in the application's workspace before lake creates the account, so two
    // sign-ups with one email cannot both proceed; the claim names the account it is for.
    user.user_id = control::new_uuid_v4();
    const auto collection = control::ApplicationUserCodec::collection;
    auto refused = co_await take_claim(*records,
                                       collection,
                                       application->id,
                                       control::email_claim,
                                       user.email,
                                       user.user_id,
                                       "email " + user.email,
                                       context);
    if (!refused) {
        co_return ApiResult::failure("store_error", refused.error().message);
    }
    if (*refused) {
        co_return std::move(**refused);
    }
    auto issued = co_await authentication_work(
        [store = context.store, user] { return store->sign_up_password(user); });
    if (!issued) {
        (void)co_await control::release_application_claim(
            *records,
            collection,
            control::application_claim_key(
                *records, collection, application->id, control::email_claim, user.email),
            user.user_id);
        co_return from_store_error(issued.error());
    }
    co_return completed(
        co_await application_session_result(std::move(issued), context, "user.created"));
}

liquid::runtime::task<ApiResult>
checked_application_session(control::Result<control::IssuedSession> issued, ApiContext& context) {
    if (!issued) {
        co_return from_store_error(issued.error());
    }
    json output = {{"session", control::to_json(issued->session)}, {"secret", issued->secret}};
    if (issued->session.pool_id == "lake") {
        co_return ApiResult::success(std::move(output));
    }
    const control::Session session = issued->session;
    const auto refuse = [&](ApiResult failure) {
        (void)context.store->revoke_session(session.user_id, session.id);
        return failure;
    };
    auto application = context.store->get_application_by_pool(session.pool_id);
    if (!application) {
        co_return refuse(from_store_error(application.error()));
    }
    auto records = application_records(*application, context);
    if (!records) {
        co_return refuse(records.error());
    }
    auto user = co_await pool_user(*records, *application, session.user_id, false, context);
    if (!user) {
        co_return refuse(ApiResult::failure("store_error", user.error().message));
    }
    if (!*user) {
        co_return refuse(std::move(user->error()));
    }
    if ((*user)->status != "active") {
        co_return refuse(ApiResult::failure("unauthorized", "application user is suspended"));
    }
    co_return ApiResult::success(std::move(output));
}

liquid::runtime::task<ApiResult> emit_application_mfa_change(ApiContext& context,
                                                             std::string pool_id,
                                                             std::string method,
                                                             bool enabled) {
    if (pool_id == "lake") {
        co_return ApiResult::success({{"recorded", false}});
    }
    auto application = context.store->get_application_by_pool(pool_id);
    if (!application) {
        co_return from_store_error(application.error());
    }
    auto records = application_records(*application, context);
    if (!records) {
        co_return records.error();
    }
    auto user = co_await application_user(*records, application->id, context.acting_user_id);
    if (!user) {
        co_return ApiResult::failure("store_error", user.error().message);
    }
    if (!*user) {
        co_return std::move(user->error());
    }
    co_return completed(co_await emit_event(
        *application,
        "mfa.changed",
        {{"user", control::to_json(**user)}, {"method", method}, {"enabled", enabled}},
        context));
}

void register_application_capabilities(Registry& registry) {
    const json application_id_input =
        input_schema({{"application_id", {{"type", "string"}}}}, json::array({"application_id"}));
    const json application_list_input =
        input_schema({{"customer_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}},
                     json::array({"customer_id"}));

    registry.add(Capability{
        .name = "register_application",
        .title = "Register application",
        .description =
            "Create an application user pool and its first Ed25519 JWT key, in one workspace of "
            "the account: that workspace's Zetta holds the application's settings (name, origins, "
            "redirect URIs, theme), signing keys, webhooks, users, organizations, lifecycle "
            "events and webhook delivery attempts. The workspace never changes.",
        .input_schema =
            input_schema({{"customer_id", {{"type", "string"}}},
                          {"name", {{"type", "string"}}},
                          {"allowed_origins", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"redirect_uris", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"custom_sign_in_domain", {{"type", "string"}}},
                          {"theme", {{"type", "object"}}},
                          {"workspace_id", {{"type", "string"}}}},
                         json::array({"customer_id", "workspace_id", "name", "allowed_origins",
                                      "redirect_uris"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer applications",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto invalid = validate_registration(input)) {
                co_return *invalid;
            }
            const std::string id = control::new_uuid_v4();
            auto key = control::apps::generate_signing_key(id, 1);
            if (!key) {
                co_return ApiResult::failure("store_error", key.error());
            }
            auto envelope = context.store->encrypt_application_credential(signing_key_purpose(id),
                                                                          key->seed);
            if (!envelope) {
                co_return from_store_error(envelope.error());
            }
            key->encrypted_seed = std::move(*envelope);
            key->seed.clear();
            // The first key goes into the chosen workspace before the application exists, so an
            // application is never registered without one (lake#352).
            control::Application routing;
            routing.id = id;
            routing.customer_id = input.at("customer_id");
            routing.workspace_id = input.at("workspace_id");
            auto records = application_records(routing, context);
            if (!records) {
                co_return records.error();
            }
            control::ApplicationSigningKeyRing ring{.application_id = id, .keys = {*key}};
            auto written = control::settled(
                co_await control::create_application_signing_keys(*records, std::move(ring)));
            if (!written) {
                co_return from_records_error(written.error());
            }
            // Its settings go there too (lake#351); the control store keeps only the routing row.
            auto settings = control::settled(co_await control::create_application_settings(
                *records, registration_settings(id, input)));
            if (!settings) {
                static_cast<void>(co_await records->remove(
                    control::ApplicationSigningKeysCodec::collection, id, written->revision));
                co_return from_records_error(settings.error());
            }
            auto created = context.store->create_application(
                {.id = id,
                 .customer_id = input.at("customer_id"),
                 .pool_id = "app:" + id,
                 .custom_sign_in_domain = input.value("custom_sign_in_domain", ""),
                 .workspace_id = input.at("workspace_id")});
            if (!created) {
                // Nothing names the unused records: take them back out of the workspace.
                static_cast<void>(co_await records->remove(
                    control::ApplicationSigningKeysCodec::collection, id, written->revision));
                static_cast<void>(co_await records->remove(
                    control::ApplicationSettingsCodec::collection, id, settings->revision));
                co_return from_store_error(created.error());
            }
            control::apply_settings(*created, *settings);
            co_return ApiResult::success(control::to_json(*created));
        }});
    registry.add(Capability{
        .name = "list_applications",
        .title = "List applications",
        .description =
            "List the applications an account has registered to sign its own users in through Lake.",
        .input_schema = application_list_input,
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer applications",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string customer_id = input.at("customer_id");
            context.audit_customer_id = customer_id;
            context.audit_kind = "application";
            context.audit_object = "customer/" + customer_id + "/applications";
            context.audit_details = {{"customer_id", customer_id}};
            auto applications = context.store->list_applications(customer_id, limit_of(input));
            if (!applications) {
                co_return from_store_error(applications.error());
            }
            std::vector<std::string> object_ids;
            std::vector<std::pair<control::Application, std::string>> candidates;
            for (const auto& application : *applications) {
                auto object = context.store->find_policy_object("application", application.id);
                if (!object) {
                    co_return from_store_error(object.error());
                }
                object_ids.push_back(object->id);
                candidates.emplace_back(application, object->id);
            }
            auto decisions = control::authorize_batch(*context.store,
                                                      context.acting_identity,
                                                      object_ids,
                                                      "read",
                                                      context.authorization_cache);
            if (!decisions) {
                co_return from_store_error(decisions.error());
            }
            // The settings of the readable applications, one list_records per workspace that
            // holds any (lake#351). An application whose settings cannot be read is still listed,
            // by its routing, with the reason.
            std::vector<control::Application> allowed;
            std::map<std::string, std::map<std::string, control::ApplicationSettings>> settings;
            std::map<std::string, std::string> unavailable; // workspace -> reason
            for (const auto& [application, object_id] : candidates) {
                if (!decisions->at(object_id).allowed) {
                    continue;
                }
                allowed.push_back(application);
                const std::string& workspace_id = application.workspace_id;
                if (workspace_id.empty() || settings.contains(workspace_id) ||
                    unavailable.contains(workspace_id)) {
                    continue;
                }
                auto records = application_records(application, context);
                if (!records) {
                    unavailable[workspace_id] =
                        records.error().error.code + ": " + records.error().error.message;
                    continue;
                }
                auto listed = control::settled(
                    co_await control::list_application_settings(*records, customer_id));
                if (!listed) {
                    unavailable[workspace_id] = listed.error().code + ": " + listed.error().message;
                    continue;
                }
                auto& held = settings[workspace_id];
                for (auto& value : *listed) {
                    const std::string key = value.application_id;
                    held.emplace(key, std::move(value));
                }
            }
            json items = json::array();
            for (control::Application& application : allowed) {
                std::string reason = "application has no workspace yet";
                if (const auto failed = unavailable.find(application.workspace_id);
                    failed != unavailable.end()) {
                    reason = failed->second;
                } else if (const auto held = settings.find(application.workspace_id);
                           held != settings.end()) {
                    const auto found = held->second.find(application.id);
                    if (found != held->second.end()) {
                        control::apply_settings(application, found->second);
                        items.push_back(control::to_json(application));
                        continue;
                    }
                    reason = "not_found: no settings in its workspace yet";
                }
                json item = control::to_json(application);
                item["settings_unavailable"] = reason;
                items.push_back(std::move(item));
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "begin_application_authorization",
        .title = "Authorize application",
        .description = "Issue a one-use OIDC authorization code with PKCE for the signed-in user.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"redirect_uri", {{"type", "string"}}},
                                      {"code_challenge", {{"type", "string"}}},
                                      {"code_challenge_method", {{"type", "string"}}},
                                      {"state", {{"type", "string"}}},
                                      {"nonce", {{"type", "string"}}}},
                                     json::array({"application_id",
                                                  "redirect_uri",
                                                  "code_challenge",
                                                  "code_challenge_method",
                                                  "state",
                                                  "nonce"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "write",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            // The registered redirect URIs are the application's settings in its workspace.
            auto application = configured(co_await configured_application(
                context.store->get_application(input.at("application_id")), context));
            if (!application) {
                co_return application.error();
            }
            const std::string redirect_uri = input.at("redirect_uri");
            if (std::ranges::find(application->redirect_uris, redirect_uri) ==
                application->redirect_uris.end()) {
                co_return ApiResult::failure(
                    "unauthorized", "redirect URI is not registered", "/redirect_uri");
            }
            if (input.at("code_challenge_method") != "S256") {
                co_return ApiResult::failure("invalid_input",
                                             "code_challenge_method must be S256",
                                             "/code_challenge_method");
            }
            if (context.acting_user_id.empty()) {
                co_return ApiResult::failure("unauthorized", "authorization requires a Lake user");
            }
            // The signed-in lake user becomes a user of the application on first authorization:
            // a record in the application's workspace, written only if absent.
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            auto existing = control::settled(co_await control::get_application_user(
                *records, application->id, context.acting_user_id));
            if (!existing) {
                co_return from_records_error(existing.error());
            }
            control::ApplicationUser user;
            if (*existing) {
                user = std::move(**existing);
            } else {
                const auto account = context.store->get_user(context.acting_user_id);
                if (!account) {
                    co_return from_store_error(account.error());
                }
                control::ApplicationUser fresh;
                fresh.application_id = application->id;
                fresh.user_id = account->id;
                fresh.taap_id = account->taap_id;
                fresh.email = account->email;
                fresh.display_name = account->display_name;
                fresh.status = "active";
                auto written = control::settled(
                    co_await control::create_application_user(*records, std::move(fresh)));
                if (!written && written.error().code == "conflict") {
                    auto again = co_await application_user(
                        *records, application->id, context.acting_user_id);
                    if (!again) {
                        co_return ApiResult::failure("store_error", again.error().message);
                    }
                    if (!*again) {
                        co_return std::move(again->error());
                    }
                    user = std::move(**again);
                } else if (!written) {
                    co_return from_records_error(written.error());
                } else {
                    user = std::move(*written);
                }
            }
            if (user.status != "active") {
                co_return ApiResult::failure("unauthorized", "application user is suspended");
            }
            // The code is issued into the application's workspace (lake#363), written only if
            // absent and expired there by the collection's retention; only its hash is kept.
            const std::string code = control::identity_crypto::random_secret();
            control::ApplicationAuthorizationCode issued;
            issued.application_id = application->id;
            issued.user_id = context.acting_user_id;
            issued.redirect_uri = redirect_uri;
            issued.code_hash = control::identity_crypto::sha256_hex(code);
            issued.code_challenge = input.at("code_challenge");
            issued.nonce = input.at("nonce");
            auto stored = control::settled(co_await control::issue_application_authorization_code(
                *records, std::move(issued)));
            if (!stored) {
                co_return from_records_error(stored.error());
            }
            context.audit_customer_id = application->customer_id;
            context.audit_kind = "authentication";
            context.audit_object = "application/" + application->id + "/authorization";
            context.audit_details = {{"application_id", application->id},
                                     {"user_id", context.acting_user_id}};
            co_return ApiResult::success(
                {{"code", code}, {"state", input.at("state")}, {"redirect_uri", redirect_uri}});
        }});
    registry.add(Capability{
        .name = "exchange_application_authorization_code",
        .title = "Exchange application authorization code",
        .description =
            "Exchange a one-use OIDC authorization code and PKCE verifier for tokens. Called by "
            "the relying party's backend with the application id and the code; the code is "
            "redeemed in the workspace Zetta of the application's workspace, at most once.",
        .input_schema =
            input_schema({{"application_id", {{"type", "string"}}},
                          {"code", {{"type", "string"}}},
                          {"redirect_uri", {{"type", "string"}}},
                          {"code_verifier", {{"type", "string"}}}},
                         json::array({"application_id", "code", "redirect_uri", "code_verifier"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            // The application's routing row names the workspace its codes live in (lake#363).
            auto application = context.store->get_application(input.at("application_id"));
            if (!application) {
                co_return ApiResult::failure(application.error().code,
                                             application.error().message, "/application_id");
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            // Read, then deleted at the revision read: of two racing exchanges only one redeems.
            auto code = control::settled(co_await control::redeem_application_authorization_code(
                *records,
                application->id,
                control::identity_crypto::sha256_hex(input.at("code").get<std::string>())));
            if (!code) {
                co_return from_records_error(code.error());
            }
            if (code->redirect_uri != input.at("redirect_uri").get<std::string>() ||
                code->code_challenge != control::identity_crypto::sha256_base64url(
                                            input.at("code_verifier").get<std::string>())) {
                co_return ApiResult::failure(
                    "unauthorized", "authorization code redirect or PKCE verifier is invalid");
            }
            auto user = co_await application_user(*records, application->id, code->user_id);
            if (!user) {
                co_return ApiResult::failure("store_error", user.error().message);
            }
            if (!*user) {
                co_return std::move(user->error());
            }
            if ((*user)->status != "active") {
                co_return ApiResult::failure("unauthorized", "application user is suspended");
            }
            auto key = co_await application_signing_key(*records, application->id, context);
            if (!key) {
                co_return ApiResult::failure("store_error", key.error().message);
            }
            if (!*key) {
                co_return std::move(key->error());
            }
            const std::string host = application->custom_sign_in_domain.empty()
                                         ? context.public_host
                                         : application->custom_sign_in_domain;
            const std::string issuer = "https://" + host + "/v1/apps/" + application->id;
            auto access_token = control::apps::issue_jwt(*application, **key, **user, issuer);
            auto id_token =
                control::apps::issue_jwt(*application, **key, **user, issuer, 900, code->nonce);
            if (!access_token || !id_token) {
                co_return ApiResult::failure("store_error", "could not issue application tokens");
            }
            context.audit_customer_id = application->customer_id;
            context.audit_kind = "authentication";
            context.audit_object = "application/" + application->id + "/token";
            context.audit_details = {{"application_id", application->id},
                                     {"user_id", (*user)->user_id}};
            co_return ApiResult::success({{"access_token", *access_token},
                                          {"id_token", *id_token},
                                          {"token_type", "Bearer"},
                                          {"expires_in", 900}});
        }});
    registry.add(Capability{
        .name = "get_application",
        .title = "Get application",
        .description =
            "Read one application registered to sign its own users in through Lake, with its "
            "settings (name, allowed origins, redirect URIs, theme) read from the workspace Zetta "
            "of the application's workspace, the `workspace_id` it returns.",
        .input_schema = input_schema({{"id", {{"type", "string"}}}}, json::array({"id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "application",
        .action = "read",
        .resolve_object = application_id_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto application = configured(co_await configured_application(
                context.store->get_application(input.at("id")), context));
            if (!application) {
                co_return application.error();
            }
            co_return ApiResult::success(control::to_json(*application));
        }});
    registry.add(Capability{
        .name = "update_application_theme",
        .title = "Update application theme",
        .description =
            "Replace the hosted identity-page theme for an application. The theme is part of the "
            "application's settings in the workspace Zetta of its workspace: `workspace_id` must "
            "name that workspace. The change is written conditionally on the revision read, so a "
            "concurrent change is reread rather than lost.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"theme", {{"type", "object"}}}},
                                     json::array({"application_id", "workspace_id", "theme"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!input.at("theme").is_object()) {
                co_return ApiResult::failure(
                    "invalid_input", "application theme must be an object", "/theme");
            }
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            const json theme = input.at("theme");
            auto updated = control::settled(co_await control::update_application_settings(
                found->records, found->application.id, found->application.customer_id,
                [&](control::ApplicationSettings& settings) { settings.theme = theme; }));
            if (!updated) {
                co_return from_records_error(updated.error());
            }
            control::Application application = std::move(found->application);
            control::apply_settings(application, *updated);
            co_return ApiResult::success(control::to_json(application));
        }});
    registry.add(Capability{
        .name = "rotate_application_signing_key",
        .title = "Rotate application signing key",
        .description =
            "Make a new Ed25519 JWT key current while retaining old public keys. The keys live in "
            "the workspace Zetta of the application's workspace: `workspace_id` must name that "
            "workspace. The current key is retired and the new one added in one conditional "
            "write, so a concurrent rotation is reread rather than lost.",
        .input_schema = input_schema(
            {{"application_id", {{"type", "string"}}}, {"workspace_id", {{"type", "string"}}}},
            json::array({"application_id", "workspace_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto application = application_in_workspace(input, context);
            if (!application) {
                co_return application.error();
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            const std::string id = application->id;
            auto rotated = control::settled(co_await control::rotate_application_signing_keys(
                *records, id,
                [&](int version) -> std::expected<control::ApplicationSigningKey,
                                                  control::ZettaControlError> {
                    auto key = control::apps::generate_signing_key(id, version);
                    if (!key) {
                        return std::unexpected(control::ZettaControlError{"store_error", key.error()});
                    }
                    auto envelope =
                        context.store->encrypt_application_credential(signing_key_purpose(id), key->seed);
                    if (!envelope) {
                        return std::unexpected(control::ZettaControlError{
                            envelope.error().code, envelope.error().message});
                    }
                    key->encrypted_seed = std::move(*envelope);
                    key->seed.clear();
                    return std::move(*key);
                }));
            if (!rotated) {
                co_return from_records_error(rotated.error());
            }
            co_return ApiResult::success({{"kid", rotated->kid}, {"version", rotated->version}});
        }});
    registry.add(Capability{
        .name = "application_jwks",
        .title = "Application JSON Web Key Set",
        .description = "Public verification keys for application JWTs, read from the workspace "
                       "Zetta of the application's workspace.",
        .input_schema = application_id_input,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "application keys",
        .action = "read",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            // Anonymous relying parties name only the application: its routing pointer names the
            // workspace, and one get_record reads the public keys.
            auto application = context.store->get_application(input.at("application_id"));
            if (!application) {
                co_return from_store_error(application.error());
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            auto ring = control::settled(
                co_await control::get_application_signing_keys(*records, application->id));
            if (!ring) {
                co_return from_records_error(ring.error());
            }
            json output = json::array();
            if (*ring) {
                for (const auto& key : (*ring)->keys) {
                    output.push_back({{"kty", "OKP"},
                                      {"crv", "Ed25519"},
                                      {"use", "sig"},
                                      {"alg", "EdDSA"},
                                      {"kid", key.kid},
                                      {"x", key.public_key}});
                }
            }
            co_return ApiResult::success({{"keys", std::move(output)}});
        }});
    // The application's users and organizations live in the workspace Zetta of its workspace
    // (lake#353, lake#354): every capability below names that workspace in `workspace_id`.
    const json workspace_property = {
        {"type", "string"},
        {"description",
         "the application's workspace, whose Zetta holds its users and "
         "organizations (get_application returns it)"}};
    const json user_input =
        input_schema({{"application_id", {{"type", "string"}}},
                      {"workspace_id", workspace_property},
                      {"user_id", {{"type", "string"}}}},
                     json::array({"application_id", "workspace_id", "user_id"}));
    registry.add(Capability{
        .name = "issue_application_jwt",
        .title = "Issue application JWT",
        .description = "Issue a short-lived JWT for an active application user.",
        .input_schema = user_input,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "write",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            auto user = co_await application_user(
                found->records, found->application.id, input.at("user_id"));
            if (!user) {
                co_return ApiResult::failure("store_error", user.error().message);
            }
            if (!*user) {
                co_return std::move(user->error());
            }
            if ((*user)->status != "active") {
                co_return ApiResult::failure("unauthorized", "application user is suspended");
            }
            auto key =
                co_await application_signing_key(found->records, found->application.id, context);
            if (!key) {
                co_return ApiResult::failure("store_error", key.error().message);
            }
            if (!*key) {
                co_return std::move(key->error());
            }
            const control::Application& application = found->application;
            const std::string host = application.custom_sign_in_domain.empty()
                                         ? context.public_host
                                         : application.custom_sign_in_domain;
            auto token = control::apps::issue_jwt(
                application, **key, **user, "https://" + host + "/v1/apps/" + application.id);
            co_return token
                ? ApiResult::success(
                      {{"access_token", *token}, {"token_type", "Bearer"}, {"expires_in", 900}})
                : ApiResult::failure("store_error", token.error());
        }});
    registry.add(Capability{
        .name = "list_application_users",
        .title = "List application users",
        .description = "List or search users in one application pool, newest first. They live "
                       "in the workspace Zetta of the application's workspace: `workspace_id` "
                       "must name that workspace.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"workspace_id", workspace_property},
                                      {"query", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}},
                                     json::array({"application_id", "workspace_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "application",
        .action = "read",
        .resolve_object = application_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto denied = authorize_application_list(input, context)) {
                co_return *denied;
            }
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            auto users = control::settled(co_await control::list_application_users(
                found->records,
                found->application.id,
                input.value("query", ""),
                static_cast<std::size_t>(limit_of(input))));
            if (!users) {
                co_return from_records_error(users.error());
            }
            json items = json::array();
            for (const auto& user : *users) {
                items.push_back(control::to_json(user));
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "suspend_application_user",
        .title = "Suspend application user",
        .description = "Suspend a pool user in the application's workspace and revoke that "
                       "pool's sessions.",
        .input_schema = user_input,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            // The status is written first: a sign-in that reads it afterwards is refused, and a
            // session issued before it is revoked here.
            auto user = control::settled(co_await control::set_application_user_status(
                found->records, found->application.id, input.at("user_id"), "suspended"));
            if (!user) {
                co_return from_records_error(user.error());
            }
            auto revoked = context.store->revoke_application_user_sessions(found->application.id,
                                                                           user->user_id);
            if (!revoked) {
                co_return from_store_error(revoked.error());
            }
            co_return completed(co_await emit_and_return(found->application,
                                                         "user.updated",
                                                         control::to_json(*user),
                                                         control::to_json(*user),
                                                         context));
        }});
    registry.add(Capability{
        .name = "delete_application_user",
        .title = "Delete application user",
        .description = "Delete a user from one application pool: the record in the application's "
                       "workspace, the email it claimed there, and the pool's credentials and "
                       "sessions.",
        .input_schema = user_input,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            auto user = co_await application_user(
                found->records, found->application.id, input.at("user_id"));
            if (!user) {
                co_return ApiResult::failure("store_error", user.error().message);
            }
            if (!*user) {
                co_return std::move(user->error());
            }
            auto removed =
                control::settled(co_await control::remove_application_user(found->records, **user));
            if (!removed) {
                co_return from_records_error(removed.error());
            }
            auto credentials = context.store->delete_application_user_credentials(
                found->application.id, (*user)->user_id);
            if (!credentials) {
                co_return from_store_error(credentials.error());
            }
            co_return completed(co_await emit_and_return(found->application,
                                                         "user.deleted",
                                                         control::to_json(**user),
                                                         {{"deleted", true}},
                                                         context));
        }});
    registry.add(
        Capability{.name = "export_application_user",
                   .title = "Export application user",
                   .description = "Export the portable profile for one application user, from the "
                                  "application's workspace.",
                   .input_schema = user_input,
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "application",
                   .action = "read",
                   .resolve_object = application_object,
                   .async_handler = [](const json& input,
                                       ApiContext& context) -> liquid::runtime::task<ApiResult> {
                       auto found = application_and_records(input, context);
                       if (!found) {
                           co_return found.error();
                       }
                       auto user = co_await application_user(
                           found->records, found->application.id, input.at("user_id"));
                       if (!user) {
                           co_return ApiResult::failure("store_error", user.error().message);
                       }
                       if (!*user) {
                           co_return std::move(user->error());
                       }
                       co_return ApiResult::success(control::to_json(**user));
                   }});
    registry.add(Capability{
        .name = "list_application_organizations",
        .title = "List application organizations",
        .description = "List organizations managed within an application, newest first, from "
                       "the application's workspace.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"workspace_id", workspace_property},
                                      {"limit", {{"type", "integer"}}}},
                                     json::array({"application_id", "workspace_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "application",
        .action = "read",
        .resolve_object = application_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto denied = authorize_application_list(input, context)) {
                co_return *denied;
            }
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            auto organizations = control::settled(co_await control::list_application_organizations(
                found->records, found->application.id, static_cast<std::size_t>(limit_of(input))));
            if (!organizations) {
                co_return from_records_error(organizations.error());
            }
            json items = json::array();
            for (const auto& organization : *organizations) {
                items.push_back(control::to_json(organization));
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "create_application_organization",
        .title = "Create application organization",
        .description = "Create an organization owned by an application, in the application's "
                       "workspace. Its TAAP identity and its name are each unique within the "
                       "application; a second create of either is a conflict.",
        .input_schema =
            input_schema({{"application_id", {{"type", "string"}}},
                          {"workspace_id", workspace_property},
                          {"taap_id", {{"type", "string"}}},
                          {"name", {{"type", "string"}}},
                          {"metadata", {{"type", "object"}}}},
                         json::array({"application_id", "workspace_id", "taap_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string taap_id = input.at("taap_id");
            const std::string name = input.at("name");
            if (taap_id.empty()) {
                co_return ApiResult::failure("invalid_input", "taap_id is required", "/taap_id");
            }
            if (name.empty()) {
                co_return ApiResult::failure("invalid_input", "name is required", "/name");
            }
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            const std::string application_id = found->application.id;
            const auto collection = control::ApplicationOrganizationCodec::collection;
            control::ApplicationOrganization organization;
            organization.id = control::new_uuid_v4();
            organization.application_id = application_id;
            organization.taap_id = taap_id;
            organization.name = name;
            organization.metadata = input.value("metadata", json::object());
            const std::string organization_id = organization.id;
            // Both unique values are claimed before the organization is written; a failure
            // releases what was claimed.
            const auto release = [&]() -> liquid::runtime::task<bool> {
                for (const auto& [kind, value] : std::array{std::pair{control::taap_claim, taap_id},
                                                            std::pair{control::name_claim, name}}) {
                    (void)co_await control::release_application_claim(
                        found->records,
                        collection,
                        control::application_claim_key(
                            found->records, collection, application_id, kind, value),
                        organization_id);
                }
                co_return true;
            };
            for (const auto& [kind, value, what] :
                 std::array{std::tuple{control::taap_claim, taap_id, "organization " + taap_id},
                            std::tuple{control::name_claim, name, "organization name " + name}}) {
                auto refused = co_await take_claim(found->records,
                                                   collection,
                                                   application_id,
                                                   kind,
                                                   value,
                                                   organization_id,
                                                   what,
                                                   context);
                if (!refused || *refused) {
                    (void)co_await release();
                    co_return refused ? std::move(**refused)
                                      : ApiResult::failure("store_error", refused.error().message);
                }
            }
            auto written = control::settled(co_await control::put_new_application_organization(
                found->records, std::move(organization)));
            if (!written) {
                (void)co_await release();
                co_return from_records_error(written.error());
            }
            co_return completed(co_await emit_and_return(found->application,
                                                         "organization.created",
                                                         control::to_json(*written),
                                                         control::to_json(*written),
                                                         context));
        }});
    registry.add(Capability{
        .name = "update_application_organization",
        .title = "Update application organization",
        .description = "Update an application organization's name and metadata, in the "
                       "application's workspace. A name another organization has is a conflict.",
        .input_schema =
            input_schema({{"application_id", {{"type", "string"}}},
                          {"workspace_id", workspace_property},
                          {"organization_id", {{"type", "string"}}},
                          {"name", {{"type", "string"}}},
                          {"metadata", {{"type", "object"}}}},
                         json::array({"application_id", "workspace_id", "organization_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            const std::string application_id = found->application.id;
            auto existing = control::settled(co_await control::get_application_organization(
                found->records, application_id, input.at("organization_id")));
            if (!existing) {
                co_return from_records_error(existing.error());
            }
            if (!*existing) {
                co_return ApiResult::failure("not_found",
                                             "application organization does not exist");
            }
            control::ApplicationOrganization next = **existing;
            const std::string name = input.value("name", "");
            if (!name.empty()) {
                next.name = name;
            }
            next.metadata = input.value("metadata", next.metadata);
            const auto collection = control::ApplicationOrganizationCodec::collection;
            const bool renamed = next.name != (*existing)->name;
            if (renamed) {
                auto refused = co_await take_claim(found->records,
                                                   collection,
                                                   application_id,
                                                   control::name_claim,
                                                   next.name,
                                                   next.id,
                                                   "organization name " + next.name,
                                                   context);
                if (!refused) {
                    co_return ApiResult::failure("store_error", refused.error().message);
                }
                if (*refused) {
                    co_return std::move(**refused);
                }
            }
            // Written over the revision read: a concurrent change is a conflict, and the new name
            // claim is released again.
            auto written = control::settled(
                co_await control::replace_application_organization(found->records, next));
            const auto release_name = [&](const std::string& value) -> liquid::runtime::task<bool> {
                (void)co_await control::release_application_claim(
                    found->records,
                    collection,
                    control::application_claim_key(
                        found->records, collection, application_id, control::name_claim, value),
                    next.id);
                co_return true;
            };
            if (!written) {
                if (renamed) {
                    (void)co_await release_name(next.name);
                }
                co_return from_records_error(written.error());
            }
            if (renamed) {
                (void)co_await release_name((*existing)->name);
            }
            co_return completed(co_await emit_and_return(found->application,
                                                         "organization.updated",
                                                         control::to_json(*written),
                                                         control::to_json(*written),
                                                         context));
        }});
    registry.add(Capability{
        .name = "delete_application_organization",
        .title = "Delete application organization",
        .description = "Delete an organization your application manages for its own users; its "
                       "memberships go with it, and its TAAP identity and name are free again.",
        .input_schema =
            input_schema({{"application_id", {{"type", "string"}}},
                          {"workspace_id", workspace_property},
                          {"organization_id", {{"type", "string"}}}},
                         json::array({"application_id", "workspace_id", "organization_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto found = application_and_records(input, context);
            if (!found) {
                co_return found.error();
            }
            auto organization = control::settled(co_await control::get_application_organization(
                found->records, found->application.id, input.at("organization_id")));
            if (!organization) {
                co_return from_records_error(organization.error());
            }
            if (!*organization) {
                co_return ApiResult::failure("not_found",
                                             "application organization does not exist");
            }
            auto removed = control::settled(
                co_await control::remove_application_organization(found->records, **organization));
            if (!removed) {
                co_return from_records_error(removed.error());
            }
            co_return completed(co_await emit_and_return(found->application,
                                                         "organization.deleted",
                                                         control::to_json(**organization),
                                                         {{"deleted", true}},
                                                         context));
        }});
    registry.add(Capability{
        .name = "create_application_webhook",
        .title = "Create application webhook",
        .description =
            "Create a signed lifecycle webhook and return its secret once. The webhook and its "
            "sealed secret live in the workspace Zetta of the application's workspace: "
            "`workspace_id` must name that workspace.",
        .input_schema =
            input_schema({{"application_id", {{"type", "string"}}},
                          {"workspace_id", {{"type", "string"}}},
                          {"url", {{"type", "string"}}},
                          {"events", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
                         json::array({"application_id", "workspace_id", "url", "events"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const auto events = input.at("events").get<std::vector<std::string>>();
            if (!valid_https_url(input.at("url"), false)) {
                co_return ApiResult::failure("invalid_input", "webhook URL must use HTTPS", "/url");
            }
            if (events.empty() || std::ranges::any_of(events, [](const std::string& value) {
                    return std::ranges::find(lifecycle_events, value) == lifecycle_events.end();
                })) {
                co_return ApiResult::failure(
                    "invalid_input", "webhook event is unsupported", "/events");
            }
            auto application = application_in_workspace(input, context);
            if (!application) {
                co_return application.error();
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            control::ApplicationWebhook webhook;
            webhook.id = control::new_uuid_v4();
            webhook.application_id = application->id;
            webhook.url = input.at("url");
            webhook.events = events;
            const std::string secret = control::identity_crypto::random_secret();
            auto envelope = context.store->encrypt_application_credential(
                webhook_secret_purpose(webhook.id), secret);
            if (!envelope) {
                co_return from_store_error(envelope.error());
            }
            webhook.encrypted_secret = std::move(*envelope);
            auto created = control::settled(
                co_await control::create_application_webhook(*records, std::move(webhook)));
            if (!created) {
                co_return from_records_error(created.error());
            }
            json output = control::to_json(*created);
            output["secret"] = secret;
            co_return ApiResult::success(std::move(output));
        }});
    registry.add(Capability{
        .name = "list_application_webhooks",
        .title = "List application webhooks",
        .description =
            "List lifecycle webhook registrations, newest first, without their secrets. They live "
            "in the workspace Zetta of the application's workspace: `workspace_id` must name that "
            "workspace.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}},
                                     json::array({"application_id", "workspace_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "application",
        .action = "read",
        .resolve_object = application_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto denied = authorize_application_list(input, context)) {
                co_return *denied;
            }
            auto application = application_in_workspace(input, context);
            if (!application) {
                co_return application.error();
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            auto webhooks = control::settled(co_await control::list_application_webhooks(
                *records, application->id, static_cast<std::size_t>(limit_of(input))));
            if (!webhooks) {
                co_return from_records_error(webhooks.error());
            }
            json items = json::array();
            for (const auto& webhook : *webhooks) {
                items.push_back(control::to_json(webhook));
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "list_application_webhook_events",
        .title = "List application webhook events",
        .description =
            "List an application's lifecycle events, newest first, with their delivery attempts. "
            "They live in the workspace Zetta of the application's workspace: `workspace_id` must "
            "name that workspace.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}},
                                     json::array({"application_id", "workspace_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "application",
        .action = "read",
        .resolve_object = application_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto denied = authorize_application_list(input, context)) {
                co_return *denied;
            }
            auto application = application_in_workspace(input, context);
            if (!application) {
                co_return application.error();
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            auto events = control::settled(co_await control::list_application_webhook_events(
                *records, application->id, static_cast<std::size_t>(limit_of(input))));
            if (!events) {
                co_return from_records_error(events.error());
            }
            // One walk of the application's attempts back to the oldest listed event (less a
            // minute for clocks that disagree), grouped by event.
            std::map<std::string, json> deliveries;
            if (!events->empty()) {
                auto attempts = control::settled(co_await control::list_application_webhook_deliveries(
                    *records, application->id, events->back().at_us - 60'000'000));
                if (!attempts) {
                    co_return from_records_error(attempts.error());
                }
                for (const auto& delivery : *attempts) {
                    auto& listed = deliveries[delivery.event_id];
                    if (!listed.is_array()) {
                        listed = json::array();
                    }
                    listed.push_back(control::to_json(delivery));
                }
            }
            json items = json::array();
            for (const auto& event : *events) {
                json value = control::to_json(event);
                const auto found = deliveries.find(event.id);
                value["deliveries"] = found == deliveries.end() ? json::array() : found->second;
                items.push_back(std::move(value));
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "retry_application_webhook",
        .title = "Retry application webhook",
        .description =
            "Retry one event against one subscribed webhook and append up to three attempts in the "
            "application's workspace. An attempt another caller is already sending is refused as "
            "a conflict and not sent twice.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"event_id", {{"type", "string"}}},
                                      {"webhook_id", {{"type", "string"}}}},
                                     json::array({"application_id", "workspace_id", "event_id",
                                                  "webhook_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto application = application_in_workspace(input, context);
            if (!application) {
                co_return application.error();
            }
            auto records = application_records(*application, context);
            if (!records) {
                co_return records.error();
            }
            auto found = control::settled(co_await control::get_application_webhook(
                *records, application->id, input.at("webhook_id")));
            if (!found) {
                co_return from_records_error(found.error());
            }
            if (!*found) {
                co_return ApiResult::failure("not_found",
                                             "event or webhook belongs to another application");
            }
            auto webhook = opened_webhook(std::move(**found), context);
            if (!webhook) {
                co_return webhook.error();
            }
            auto event = control::settled(co_await control::get_application_webhook_event(
                *records, application->id, input.at("event_id")));
            if (!event) {
                co_return from_records_error(event.error());
            }
            if (!*event) {
                co_return ApiResult::failure("not_found",
                                             "event or webhook belongs to another application");
            }
            if (!accepts_event(*webhook, (*event)->event)) {
                co_return ApiResult::failure("conflict", "webhook does not subscribe to this event");
            }
            auto attempts = control::settled(co_await control::list_application_webhook_deliveries(
                *records, application->id, (*event)->at_us - 60'000'000));
            if (!attempts) {
                co_return from_records_error(attempts.error());
            }
            int previous = 0;
            const control::ApplicationWebhookDelivery* latest = nullptr;
            for (const auto& attempt : *attempts) {
                if (attempt.event_id == (*event)->id && attempt.webhook_id == webhook->id &&
                    attempt.attempt > previous) {
                    previous = attempt.attempt;
                    latest = &attempt;
                }
            }
            // Another caller is still sending this event to this webhook: refuse rather than
            // deliver it a second time concurrently. A claim older than any send can take was
            // abandoned by a caller that stopped, and no longer holds the webhook.
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            if (latest && latest->pending && now_us - latest->at_us < abandoned_claim_us) {
                co_return ApiResult::failure(
                    "conflict", "a delivery of this event to this webhook is in progress");
            }
            co_return co_await deliver_to_webhook(*records, **event, *webhook, previous + 1,
                                                  context);
        }});
}

} // namespace lake::api
