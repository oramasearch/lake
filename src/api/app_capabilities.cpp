#include "lake/api/apps.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <ranges>
#include <string>
#include <vector>

#include "lake/control/identity_crypto.hpp"

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

ApiResult deliver_to_webhook(const control::ApplicationWebhookEvent& event,
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
        WebhookDeliveryResult sent;
        if (context.webhook_sender) {
            sent = context.webhook_sender(webhook.url,
                                          {{"Content-Type", "application/json"},
                                           {"X-Lake-Event", event.event},
                                           {"X-Lake-Event-Id", event.id},
                                           {"X-Lake-Signature", signature}},
                                          payload);
        } else {
            sent.error = "webhook sender is not configured";
        }
        auto stored = context.store->create_application_webhook_delivery(
            {event.id, webhook.id, attempt, sent.status, signature, sent.error});
        if (!stored) {
            return from_store_error(stored.error());
        }
        last = *stored;
        if (sent.status >= 200 && sent.status < 300) {
            break;
        }
    }
    return ApiResult::success(control::to_json(last));
}

std::expected<control::ApplicationOrganization, ApiResult> application_organization(
    const std::string& application_id, const std::string& organization_id, ApiContext& context) {
    auto organizations = context.store->list_application_organizations(application_id, 1000);
    if (!organizations) {
        return std::unexpected(from_store_error(organizations.error()));
    }
    const auto found = std::ranges::find_if(
        *organizations, [&](const auto& value) { return value.id == organization_id; });
    if (found == organizations->end()) {
        return std::unexpected(
            ApiResult::failure("not_found", "application organization does not exist"));
    }
    return *found;
}

ApiResult emit_and_return(const control::Application& application,
                          std::string_view event,
                          const json& data,
                          json value,
                          ApiContext& context) {
    const ApiResult emitted = emit_application_event(application, event, data, context);
    if (!emitted.ok) {
        return emitted;
    }
    return ApiResult::success(std::move(value));
}

} // namespace

ApiResult emit_application_event(const control::Application& application,
                                 std::string_view event,
                                 const json& data,
                                 ApiContext& context) {
    if (!context.store) {
        return no_store();
    }
    if (std::ranges::find(lifecycle_events, event) == lifecycle_events.end()) {
        return ApiResult::failure("invalid_input", "unsupported application lifecycle event");
    }
    auto stored =
        context.store->create_application_webhook_event(application.id, std::string(event), data);
    if (!stored) {
        return from_store_error(stored.error());
    }
    auto webhooks = context.store->list_application_webhooks(application.id, 1000);
    if (!webhooks) {
        return from_store_error(webhooks.error());
    }
    for (const control::ApplicationWebhook& webhook : *webhooks) {
        if (!accepts_event(webhook, event)) {
            continue;
        }
        const ApiResult delivered = deliver_to_webhook(*stored, webhook, 1, context);
        if (!delivered.ok) {
            return delivered;
        }
    }
    return ApiResult::success(control::to_json(*stored));
}

ApiResult application_session_result(control::Result<control::IssuedSession> issued,
                                     ApiContext& context,
                                     std::string_view lifecycle_event) {
    if (!issued) {
        return from_store_error(issued.error());
    }
    json output = {{"session", control::to_json(issued->session)}, {"secret", issued->secret}};
    if (issued->session.pool_id == "lake") {
        return ApiResult::success(std::move(output));
    }
    auto application = context.store->get_application_by_pool(issued->session.pool_id);
    if (!application) {
        return from_store_error(application.error());
    }
    auto user = context.store->get_application_user(application->id, issued->session.user_id);
    if (!user) {
        return from_store_error(user.error());
    }
    if (issued->session.kind != "api_token" && issued->session.kind != "static") {
        auto key = context.store->current_application_signing_key(application->id);
        if (!key) {
            return from_store_error(key.error());
        }
        const std::string host = application->custom_sign_in_domain.empty()
                                     ? context.public_host
                                     : application->custom_sign_in_domain;
        const std::string issuer = "https://" + host + "/v1/apps/" + application->id;
        auto token = control::apps::issue_jwt(*application, *key, *user, issuer);
        if (!token) {
            return ApiResult::failure("store_error", token.error());
        }
        output["access_token"] = *token;
        output["token_type"] = "Bearer";
        output["expires_in"] = 900;
    }
    if (lifecycle_event == "user.created") {
        const ApiResult created =
            emit_application_event(*application, "user.created", control::to_json(*user), context);
        if (!created.ok) {
            return created;
        }
    }
    const ApiResult started = emit_application_event(
        *application,
        "session.started",
        {{"user", control::to_json(*user)}, {"session", control::to_json(issued->session)}},
        context);
    if (!started.ok) {
        return started;
    }
    return ApiResult::success(std::move(output));
}

ApiResult emit_application_mfa_change(ApiContext& context,
                                      std::string_view pool_id,
                                      std::string_view method,
                                      bool enabled) {
    if (pool_id == "lake") {
        return ApiResult::success({{"recorded", false}});
    }
    auto application = context.store->get_application_by_pool(std::string(pool_id));
    if (!application) {
        return from_store_error(application.error());
    }
    auto user = context.store->get_application_user(application->id, context.acting_user_id);
    if (!user) {
        return from_store_error(user.error());
    }
    return emit_application_event(
        *application,
        "mfa.changed",
        {{"user", control::to_json(*user)}, {"method", method}, {"enabled", enabled}},
        context);
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
        .description = "Create an application user pool and its first Ed25519 JWT key.",
        .input_schema =
            input_schema({{"customer_id", {{"type", "string"}}},
                          {"name", {{"type", "string"}}},
                          {"allowed_origins", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"redirect_uris", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"custom_sign_in_domain", {{"type", "string"}}},
                          {"theme", {{"type", "object"}}}},
                         json::array({"customer_id", "name", "allowed_origins", "redirect_uris"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer applications",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (auto invalid = validate_registration(input)) {
                return *invalid;
            }
            const std::string id = control::new_uuid_v4();
            auto key = control::apps::generate_signing_key(id, 1);
            if (!key) {
                return ApiResult::failure("store_error", key.error());
            }
            return one_result(context.store->create_application(
                {.id = id,
                 .customer_id = input.at("customer_id"),
                 .pool_id = "app:" + id,
                 .name = input.at("name"),
                 .allowed_origins = input.at("allowed_origins").get<std::vector<std::string>>(),
                 .redirect_uris = input.at("redirect_uris").get<std::vector<std::string>>(),
                 .custom_sign_in_domain = input.value("custom_sign_in_domain", ""),
                 .theme = input.value("theme", json::object()),
                 .signing_key = *key}));
        }});
    registry.add(Capability{
        .name = "list_applications",
        .title = "List applications",
        .description =
            "List the applications a space has registered to sign its own users in through lake.",
        .input_schema = application_list_input,
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer applications",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            const std::string customer_id = input.at("customer_id");
            context.audit_customer_id = customer_id;
            context.audit_kind = "application";
            context.audit_object = "customer/" + customer_id + "/applications";
            context.audit_details = {{"customer_id", customer_id}};
            auto applications = context.store->list_applications(customer_id, limit_of(input));
            if (!applications) {
                return from_store_error(applications.error());
            }
            std::vector<std::string> object_ids;
            std::vector<std::pair<control::Application, std::string>> candidates;
            for (const auto& application : *applications) {
                auto object = context.store->find_policy_object("application", application.id);
                if (!object) {
                    return from_store_error(object.error());
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
                return from_store_error(decisions.error());
            }
            json items = json::array();
            for (const auto& [application, object_id] : candidates) {
                if (decisions->at(object_id).allowed) {
                    items.push_back(control::to_json(application));
                }
            }
            return ApiResult::success({{"items", std::move(items)}});
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
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            if (!application) {
                return from_store_error(application.error());
            }
            const std::string redirect_uri = input.at("redirect_uri");
            if (std::ranges::find(application->redirect_uris, redirect_uri) ==
                application->redirect_uris.end()) {
                return ApiResult::failure(
                    "unauthorized", "redirect URI is not registered", "/redirect_uri");
            }
            if (input.at("code_challenge_method") != "S256") {
                return ApiResult::failure("invalid_input",
                                          "code_challenge_method must be S256",
                                          "/code_challenge_method");
            }
            if (context.acting_user_id.empty()) {
                return ApiResult::failure("unauthorized", "authorization requires a lake user");
            }
            auto application_user =
                context.store->get_application_user(application->id, context.acting_user_id);
            if (!application_user && application_user.error().code == "not_found") {
                application_user = context.store->register_application_user(application->id,
                                                                            context.acting_user_id);
            }
            if (!application_user) {
                return from_store_error(application_user.error());
            }
            if (application_user->status != "active") {
                return ApiResult::failure("unauthorized", "application user is suspended");
            }
            const std::string code = control::identity_crypto::random_secret();
            auto stored = context.store->create_application_authorization_code(
                {application->id,
                 context.acting_user_id,
                 redirect_uri,
                 control::identity_crypto::sha256_hex(code),
                 input.at("code_challenge"),
                 input.at("nonce")});
            if (!stored) {
                return from_store_error(stored.error());
            }
            context.audit_customer_id = application->customer_id;
            context.audit_kind = "authentication";
            context.audit_object = "application/" + application->id + "/authorization";
            context.audit_details = {{"application_id", application->id},
                                     {"user_id", context.acting_user_id}};
            return ApiResult::success(
                {{"code", code}, {"state", input.at("state")}, {"redirect_uri", redirect_uri}});
        }});
    registry.add(Capability{
        .name = "exchange_application_authorization_code",
        .title = "Exchange application authorization code",
        .description = "Exchange a one-use OIDC authorization code and PKCE verifier for tokens.",
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
        .handler = [](const json& input, ApiContext& context) {
            auto code = context.store->consume_application_authorization_code(
                input.at("application_id"),
                control::identity_crypto::sha256_hex(input.at("code").get<std::string>()));
            if (!code) {
                return from_store_error(code.error());
            }
            if (code->redirect_uri != input.at("redirect_uri").get<std::string>() ||
                code->code_challenge != control::identity_crypto::sha256_base64url(
                                            input.at("code_verifier").get<std::string>())) {
                return ApiResult::failure(
                    "unauthorized", "authorization code redirect or PKCE verifier is invalid");
            }
            auto application = context.store->get_application(code->application_id);
            auto user = context.store->get_application_user(code->application_id, code->user_id);
            auto key = context.store->current_application_signing_key(code->application_id);
            if (!application) {
                return from_store_error(application.error());
            }
            if (!user) {
                return from_store_error(user.error());
            }
            if (!key) {
                return from_store_error(key.error());
            }
            const std::string host = application->custom_sign_in_domain.empty()
                                         ? context.public_host
                                         : application->custom_sign_in_domain;
            const std::string issuer = "https://" + host + "/v1/apps/" + application->id;
            auto access_token = control::apps::issue_jwt(*application, *key, *user, issuer);
            auto id_token =
                control::apps::issue_jwt(*application, *key, *user, issuer, 900, code->nonce);
            if (!access_token || !id_token) {
                return ApiResult::failure("store_error", "could not issue application tokens");
            }
            context.audit_customer_id = application->customer_id;
            context.audit_kind = "authentication";
            context.audit_object = "application/" + application->id + "/token";
            context.audit_details = {{"application_id", application->id},
                                     {"user_id", user->user_id}};
            return ApiResult::success({{"access_token", *access_token},
                                       {"id_token", *id_token},
                                       {"token_type", "Bearer"},
                                       {"expires_in", 900}});
        }});
    registry.add(Capability{.name = "get_application",
                            .title = "Get application",
                            .description = "Read one application registered to sign its own users "
                                           "in through lake, with its settings.",
                            .input_schema =
                                input_schema({{"id", {{"type", "string"}}}}, json::array({"id"})),
                            .output_schema = object_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "application",
                            .action = "read",
                            .resolve_object = application_id_object,
                            .handler = [](const json& input, ApiContext& context) {
                                return one_result(context.store->get_application(input.at("id")));
                            }});
    registry.add(
        Capability{.name = "update_application_theme",
                   .title = "Update application theme",
                   .description = "Replace the hosted identity-page theme for an application.",
                   .input_schema = input_schema(
                       {{"application_id", {{"type", "string"}}}, {"theme", {{"type", "object"}}}},
                       json::array({"application_id", "theme"})),
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "application",
                   .action = "admin",
                   .resolve_object = application_object,
                   .handler = [](const json& input, ApiContext& context) {
                       return one_result(context.store->update_application_theme(
                           input.at("application_id"), input.at("theme")));
                   }});
    registry.add(Capability{
        .name = "rotate_application_signing_key",
        .title = "Rotate application signing key",
        .description = "Make a new Ed25519 JWT key current while retaining old public keys.",
        .input_schema = application_id_input,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string id = input.at("application_id");
            auto keys = context.store->list_application_signing_keys(id);
            if (!keys) {
                return from_store_error(keys.error());
            }
            auto key = control::apps::generate_signing_key(id, static_cast<int>(keys->size()) + 1);
            if (!key) {
                return ApiResult::failure("store_error", key.error());
            }
            auto rotated = context.store->rotate_application_signing_key(*key);
            if (!rotated) {
                return from_store_error(rotated.error());
            }
            return ApiResult::success({{"kid", rotated->kid}, {"version", rotated->version}});
        }});
    registry.add(Capability{.name = "application_jwks",
                            .title = "Application JSON Web Key Set",
                            .description = "Public verification keys for application JWTs.",
                            .input_schema = application_id_input,
                            .output_schema = object_schema(),
                            .kind = CapabilityKind::Read,
                            .requires_auth = false,
                            .object = "application keys",
                            .action = "read",
                            .resolve_object = application_object,
                            .handler = [](const json& input, ApiContext& context) {
                                auto keys = context.store->list_application_signing_keys(
                                    input.at("application_id"));
                                if (!keys) {
                                    return from_store_error(keys.error());
                                }
                                json output = json::array();
                                for (const auto& key : *keys) {
                                    output.push_back({{"kty", "OKP"},
                                                      {"crv", "Ed25519"},
                                                      {"use", "sig"},
                                                      {"alg", "EdDSA"},
                                                      {"kid", key.kid},
                                                      {"x", key.public_key}});
                                }
                                return ApiResult::success({{"keys", std::move(output)}});
                            }});
    registry.add(Capability{
        .name = "issue_application_jwt",
        .title = "Issue application JWT",
        .description = "Issue a short-lived JWT for an active application user.",
        .input_schema = input_schema(
            {{"application_id", {{"type", "string"}}}, {"user_id", {{"type", "string"}}}},
            json::array({"application_id", "user_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "write",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            auto user = context.store->get_application_user(input.at("application_id"),
                                                            input.at("user_id"));
            auto key = context.store->current_application_signing_key(input.at("application_id"));
            if (!application) {
                return from_store_error(application.error());
            }
            if (!user) {
                return from_store_error(user.error());
            }
            if (user->status != "active") {
                return ApiResult::failure("unauthorized", "application user is suspended");
            }
            if (!key) {
                return from_store_error(key.error());
            }
            const std::string host = application->custom_sign_in_domain.empty()
                                         ? context.public_host
                                         : application->custom_sign_in_domain;
            auto token = control::apps::issue_jwt(
                *application, *key, *user, "https://" + host + "/v1/apps/" + application->id);
            return token ? ApiResult::success({{"access_token", *token},
                                               {"token_type", "Bearer"},
                                               {"expires_in", 900}})
                         : ApiResult::failure("store_error", token.error());
        }});
    registry.add(
        Capability{.name = "list_application_users",
                   .title = "List application users",
                   .description = "List or search users in one application pool.",
                   .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                                 {"query", {{"type", "string"}}},
                                                 {"limit", {{"type", "integer"}}}},
                                                json::array({"application_id"})),
                   .output_schema = items_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "application",
                   .action = "read",
                   .resolve_object = application_object,
                   .authorizes_candidates = true,
                   .handler = [](const json& input, ApiContext& context) {
                       if (auto denied = authorize_application_list(input, context)) {
                           return *denied;
                       }
                       return list_result(context.store->list_application_users(
                           input.at("application_id"), input.value("query", ""), limit_of(input)));
                   }});
    registry.add(Capability{
        .name = "suspend_application_user",
        .title = "Suspend application user",
        .description = "Suspend a pool user and revoke that pool's sessions.",
        .input_schema = input_schema(
            {{"application_id", {{"type", "string"}}}, {"user_id", {{"type", "string"}}}},
            json::array({"application_id", "user_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            auto user = context.store->set_application_user_status(
                input.at("application_id"), input.at("user_id"), "suspended");
            if (!application) {
                return from_store_error(application.error());
            }
            if (!user) {
                return from_store_error(user.error());
            }
            return emit_and_return(*application,
                                   "user.updated",
                                   control::to_json(*user),
                                   control::to_json(*user),
                                   context);
        }});
    registry.add(Capability{
        .name = "delete_application_user",
        .title = "Delete application user",
        .description = "Delete a user's credentials and sessions from one application pool.",
        .input_schema = input_schema(
            {{"application_id", {{"type", "string"}}}, {"user_id", {{"type", "string"}}}},
            json::array({"application_id", "user_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            auto user = context.store->get_application_user(input.at("application_id"),
                                                            input.at("user_id"));
            if (!application) {
                return from_store_error(application.error());
            }
            if (!user) {
                return from_store_error(user.error());
            }
            auto removed = context.store->delete_application_user(input.at("application_id"),
                                                                  input.at("user_id"));
            if (!removed) {
                return from_store_error(removed.error());
            }
            return emit_and_return(*application,
                                   "user.deleted",
                                   control::to_json(*user),
                                   {{"deleted", true}},
                                   context);
        }});
    registry.add(
        Capability{.name = "export_application_user",
                   .title = "Export application user",
                   .description = "Export the portable profile for one application user.",
                   .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                                 {"user_id", {{"type", "string"}}}},
                                                json::array({"application_id", "user_id"})),
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "application",
                   .action = "read",
                   .resolve_object = application_object,
                   .handler = [](const json& input, ApiContext& context) {
                       return one_result(context.store->get_application_user(
                           input.at("application_id"), input.at("user_id")));
                   }});
    registry.add(Capability{.name = "list_application_organizations",
                            .title = "List application organizations",
                            .description = "List organizations managed within an application.",
                            .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                                          {"limit", {{"type", "integer"}}}},
                                                         json::array({"application_id"})),
                            .output_schema = items_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "application",
                            .action = "read",
                            .resolve_object = application_object,
                            .authorizes_candidates = true,
                            .handler = [](const json& input, ApiContext& context) {
                                if (auto denied = authorize_application_list(input, context)) {
                                    return *denied;
                                }
                                return list_result(context.store->list_application_organizations(
                                    input.at("application_id"), limit_of(input)));
                            }});
    registry.add(Capability{
        .name = "create_application_organization",
        .title = "Create application organization",
        .description = "Create an organization owned by an application.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"taap_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"metadata", {{"type", "object"}}}},
                                     json::array({"application_id", "taap_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            if (!application) {
                return from_store_error(application.error());
            }
            auto organization = context.store->create_application_organization(
                {input.at("application_id"),
                 input.at("taap_id"),
                 input.at("name"),
                 input.value("metadata", json::object())});
            if (!organization) {
                return from_store_error(organization.error());
            }
            return emit_and_return(*application,
                                   "organization.created",
                                   control::to_json(*organization),
                                   control::to_json(*organization),
                                   context);
        }});
    registry.add(Capability{
        .name = "update_application_organization",
        .title = "Update application organization",
        .description = "Update an application organization's name and metadata.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"organization_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"metadata", {{"type", "object"}}}},
                                     json::array({"application_id", "organization_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            auto existing = application_organization(
                input.at("application_id"), input.at("organization_id"), context);
            if (!application) {
                return from_store_error(application.error());
            }
            if (!existing) {
                return existing.error();
            }
            auto organization = context.store->update_application_organization(
                input.at("organization_id"),
                input.value("name", ""),
                input.value("metadata", existing->metadata));
            if (!organization) {
                return from_store_error(organization.error());
            }
            return emit_and_return(*application,
                                   "organization.updated",
                                   control::to_json(*organization),
                                   control::to_json(*organization),
                                   context);
        }});
    registry.add(Capability{
        .name = "delete_application_organization",
        .title = "Delete application organization",
        .description = "Delete an organization your application manages for its own users; its "
                       "memberships go with it.",
        .input_schema = input_schema(
            {{"application_id", {{"type", "string"}}}, {"organization_id", {{"type", "string"}}}},
            json::array({"application_id", "organization_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto application = context.store->get_application(input.at("application_id"));
            auto organization = application_organization(
                input.at("application_id"), input.at("organization_id"), context);
            if (!application) {
                return from_store_error(application.error());
            }
            if (!organization) {
                return organization.error();
            }
            auto removed =
                context.store->delete_application_organization(input.at("organization_id"));
            if (!removed) {
                return from_store_error(removed.error());
            }
            return emit_and_return(*application,
                                   "organization.deleted",
                                   control::to_json(*organization),
                                   {{"deleted", true}},
                                   context);
        }});
    registry.add(Capability{
        .name = "create_application_webhook",
        .title = "Create application webhook",
        .description = "Create a signed lifecycle webhook and return its secret once.",
        .input_schema =
            input_schema({{"application_id", {{"type", "string"}}},
                          {"url", {{"type", "string"}}},
                          {"events", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
                         json::array({"application_id", "url", "events"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto events = input.at("events").get<std::vector<std::string>>();
            if (!valid_https_url(input.at("url"), false)) {
                return ApiResult::failure("invalid_input", "webhook URL must use HTTPS", "/url");
            }
            if (events.empty() || std::ranges::any_of(events, [](const std::string& value) {
                    return std::ranges::find(lifecycle_events, value) == lifecycle_events.end();
                })) {
                return ApiResult::failure(
                    "invalid_input", "webhook event is unsupported", "/events");
            }
            const std::string secret = control::identity_crypto::random_secret();
            auto webhook = context.store->create_application_webhook(
                {input.at("application_id"), input.at("url"), events, secret});
            if (!webhook) {
                return from_store_error(webhook.error());
            }
            json output = control::to_json(*webhook);
            output["secret"] = secret;
            return ApiResult::success(std::move(output));
        }});
    registry.add(
        Capability{.name = "list_application_webhooks",
                   .title = "List application webhooks",
                   .description = "List lifecycle webhook registrations without their secrets.",
                   .input_schema = input_schema(
                       {{"application_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}},
                       json::array({"application_id"})),
                   .output_schema = items_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "application",
                   .action = "read",
                   .resolve_object = application_object,
                   .authorizes_candidates = true,
                   .handler = [](const json& input, ApiContext& context) {
                       if (auto denied = authorize_application_list(input, context)) {
                           return *denied;
                       }
                       return list_result(context.store->list_application_webhooks(
                           input.at("application_id"), limit_of(input)));
                   }});
    registry.add(Capability{
        .name = "list_application_webhook_events",
        .title = "List application webhook events",
        .description = "List lifecycle events and their recorded delivery attempts.",
        .input_schema = input_schema(
            {{"application_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}},
            json::array({"application_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "application",
        .action = "read",
        .resolve_object = application_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (auto denied = authorize_application_list(input, context)) {
                return *denied;
            }
            auto events = context.store->list_application_webhook_events(input.at("application_id"),
                                                                         limit_of(input));
            if (!events) {
                return from_store_error(events.error());
            }
            json items = json::array();
            for (const auto& event : *events) {
                json value = control::to_json(event);
                auto deliveries = context.store->list_application_webhook_deliveries(event.id);
                if (!deliveries) {
                    return from_store_error(deliveries.error());
                }
                value["deliveries"] = json::array();
                for (const auto& delivery : *deliveries) {
                    value["deliveries"].push_back(control::to_json(delivery));
                }
                items.push_back(std::move(value));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "retry_application_webhook",
        .title = "Retry application webhook",
        .description = "Retry one event against one subscribed webhook and append attempts.",
        .input_schema = input_schema({{"application_id", {{"type", "string"}}},
                                      {"event_id", {{"type", "string"}}},
                                      {"webhook_id", {{"type", "string"}}}},
                                     json::array({"application_id", "event_id", "webhook_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "application",
        .action = "admin",
        .resolve_object = application_object,
        .handler = [](const json& input, ApiContext& context) {
            auto event = context.store->get_application_webhook_event(input.at("event_id"));
            auto webhook = context.store->get_application_webhook(input.at("webhook_id"));
            if (!event) {
                return from_store_error(event.error());
            }
            if (!webhook) {
                return from_store_error(webhook.error());
            }
            const std::string application_id = input.at("application_id");
            if (event->application_id != application_id ||
                webhook->application_id != application_id) {
                return ApiResult::failure("not_found",
                                          "event or webhook belongs to another application");
            }
            if (!accepts_event(*webhook, event->event)) {
                return ApiResult::failure("conflict", "webhook does not subscribe to this event");
            }
            auto attempts = context.store->list_application_webhook_deliveries(event->id);
            if (!attempts) {
                return from_store_error(attempts.error());
            }
            return deliver_to_webhook(
                *event, *webhook, static_cast<int>(attempts->size()) + 1, context);
        }});
}

} // namespace lake::api
