#include "lake/api/registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <print>

#include "lake/api/audit_routing.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"
#include "lake/transparency/log.hpp"

namespace lake::api {
namespace {

bool contains_database_diagnostic(std::string_view message) {
    std::string uppercase(message);
    std::ranges::transform(uppercase, uppercase.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return uppercase.contains("ERROR:") || uppercase.contains("DETAIL:");
}

std::string product_store_message(std::string_view code) {
    if (code == "conflict") {
        return "the requested change conflicts with existing data";
    }
    if (code == "invalid_input") {
        return "the request contains invalid data";
    }
    if (code == "store_unavailable") {
        return "the control-plane store is unavailable";
    }
    return "the control-plane store could not complete the request";
}

void log_database_diagnostic(std::string_view code, std::string_view message) {
    const nlohmann::json entry = {
        {"event", "api_store_error"}, {"code", code}, {"detail", message}};
    std::println(stderr, "{}", entry.dump());
}

constexpr std::array<ErrorCode, 29> documented_error_codes{{
    {"invalid_input", 400, "The input is at fault; `path` points at the field."},
    {"secret_required",
     400,
     "A value that looks like a secret was sent in plain text; store it as a Secret and pass its "
     "reference."},
    {"invalid_digest",
     400,
     "An archive or record does not match the digest it carries, so it was changed or damaged."},
    {"invalid_signature", 400, "A signature does not verify against its key."},
    {"unauthenticated",
     401,
     "No credential was accepted: `reason` says whether it was missing, invalid or expired, and "
     "`help` links to the page that makes a new key."},
    {"unauthorized", 403, "The credential is valid but does not grant this call on this object."},
    {"mfa_required", 403, "The account must finish multi-factor verification first."},
    {"consent_required",
     403,
     "The workspace requires an approval before it reaches an outside source, which lake cannot "
     "give."},
    {"not_found", 404, "The addressed object does not exist, or the caller cannot see it."},
    {"unknown_capability", 404, "No capability has this name."},
    {"method_not_allowed",
     405,
     "The capability exists but takes the other HTTP method: reads GET, actions POST."},
    {"conflict", 409, "The change conflicts with existing state, such as a name already taken."},
    {"already_exists", 409, "A repository object or reference with this identity already exists."},
    {"locked", 409, "Another writer holds the repository reference; retry."},
    {"unrecoverable",
     409,
     "The request cannot succeed as asked, such as recovering a key only a lost device held."},
    {"not_approved",
     409,
     "The workspace action needs an approval it does not have yet: approve the plan or the "
     "decision first."},
    {"gate_blocked", 409, "A pipeline gate refused the run; the message names the failing check."},
    {"invalid_chain",
     409,
     "The audit log's hash chain is broken: an event is missing or was changed."},
    {"size_invalid", 413, "The request body is larger than the limit."},
    {"engine_error", 502, "The workspace engine could not complete the call; the message is its."},
    {"not_supported", 501, "This lake or its workspace engine cannot do what was asked yet."},
    {"quota_exceeded",
     507,
     "The customer's storage quota is full; existing content stays readable."},
    {"store_error", 500, "The server failed; the message says what failed."},
    {"corrupt", 500, "Stored repository data failed an integrity check."},
    {"io", 500, "Repository storage could not be read or written."},
    {"crypto_error", 500, "A signing, hashing or certificate operation failed on the server."},
    {"store_unavailable",
     503,
     "The control-plane store, the workspace or another dependency is not reachable now."},
    {"workspace_not_ready", 503, "The workspace or service exists but is not ready yet."},
    {"busy", 503, "The resource is busy; retry."},
}};

// Other spellings of a documented code: the workspace engine's and liquid's.
std::string_view documented_spelling(std::string_view code) {
    if (code == "invalid_argument") {
        return "invalid_input";
    }
    if (code == "unsupported") {
        return "not_supported";
    }
    if (code == "unavailable" || code == "not_remote" || code == "not_configured") {
        return "store_unavailable";
    }
    if (code == "internal") {
        return "store_error";
    }
    return code;
}

} // namespace

std::span<const ErrorCode> error_codes() {
    return documented_error_codes;
}

const ErrorCode* find_error_code(std::string_view code) {
    const auto found = std::ranges::find(documented_error_codes, code, &ErrorCode::code);
    return found == documented_error_codes.end() ? nullptr : &*found;
}

ApiResult ApiResult::success(nlohmann::json output) {
    ApiResult result;
    result.ok = true;
    result.value = std::move(output);
    return result;
}

ApiResult ApiResult::failure(std::string code, std::string message, std::string path) {
    code = std::string(documented_spelling(code));
    if (find_error_code(code) == nullptr) {
        // An undocumented code never reaches a caller (lake#450): it is named in the message.
        message = code.empty() ? message : code + ": " + message;
        code = "store_error";
    }
    if (contains_database_diagnostic(message)) {
        log_database_diagnostic(code, message);
        message = product_store_message(code);
    }
    ApiResult result;
    result.ok = false;
    result.error = ApiError{std::move(code), std::move(message), std::move(path)};
    return result;
}

nlohmann::json ApiResult::envelope() const {
    nlohmann::json envelope;
    envelope["ok"] = ok;
    if (ok) {
        envelope["value"] = value;
    } else {
        envelope["error"] = {
            {"code", error.code}, {"message", error.message}, {"path", error.path}};
        if (!error.reason.empty()) {
            envelope["error"]["reason"] = error.reason;
        }
        if (!error.help.empty()) {
            envelope["error"]["help"] = error.help;
        }
    }
    envelope["duration_ms"] = duration_ms;
    return envelope;
}

nlohmann::json error_envelope_schema() {
    nlohmann::json codes = nlohmann::json::array();
    for (const ErrorCode& code : documented_error_codes) {
        codes.push_back(std::string(code.code));
    }
    return {{"type", "object"},
            {"properties",
             {{"ok", {{"const", false}}},
              {"error",
               {{"type", "object"},
                {"properties",
                 {{"code",
                   {{"type", "string"},
                    {"enum", std::move(codes)},
                    {"description", "what went wrong, one of the codes in docs/api/errors.md"}}},
                  {"message", {{"type", "string"}}},
                  {"path",
                   {{"type", "string"},
                    {"description",
                     "JSON pointer to the input field at fault; empty when the input is not"}}},
                  {"reason",
                   {{"type", "string"},
                    {"enum", {"missing", "invalid", "expired"}},
                    {"description", "unauthenticated only: why no credential was accepted"}}},
                  {"help",
                   {{"type", "string"},
                    {"description", "unauthenticated only: where to make a new key"}}}}},
                {"required", nlohmann::json::array({"code", "message"})}}},
              {"duration_ms", {{"type", "number"}}}}},
            {"required", nlohmann::json::array({"ok", "error", "duration_ms"})}};
}

nlohmann::json success_envelope_schema(const nlohmann::json& output_schema) {
    nlohmann::json value = output_schema.is_object() && !output_schema.empty()
                               ? output_schema
                               : nlohmann::json{{"type", "object"}};
    return {{"type", "object"},
            {"properties",
             {{"ok", {{"const", true}}}, {"value", value}, {"duration_ms", {{"type", "number"}}}}},
            {"required", nlohmann::json::array({"ok", "value", "duration_ms"})}};
}

std::string_view to_string(CapabilityKind kind) {
    return kind == CapabilityKind::Read ? "read" : "action";
}

namespace {

bool matches_type(const nlohmann::json& value, const std::string& type) {
    if (type == "null")
        return value.is_null();
    if (type == "string")
        return value.is_string();
    if (type == "integer")
        return value.is_number_integer();
    if (type == "number")
        return value.is_number();
    if (type == "boolean")
        return value.is_boolean();
    if (type == "object")
        return value.is_object();
    if (type == "array")
        return value.is_array();
    return true;
}

std::string pointer_child(const std::string& parent, const std::string& key) {
    std::string path = parent + "/";
    for (const char character : key) {
        if (character == '~') {
            path += "~0";
        } else if (character == '/') {
            path += "~1";
        } else {
            path += character;
        }
    }
    return path;
}

ApiResult
validate_value(const nlohmann::json& schema, const nlohmann::json& value, const std::string& path) {
    const auto type = schema.find("type");
    if (type != schema.end()) {
        const bool matches =
            type->is_array()
                ? std::ranges::any_of(*type,
                                      [&](const auto& alternative) {
                                          return matches_type(
                                              value, alternative.template get<std::string>());
                                      })
                : matches_type(value, type->get<std::string>());
        if (!matches) {
            return ApiResult::failure("invalid_input", "expected type " + type->dump(), path);
        }
    }
    if (const auto allowed = schema.find("enum");
        allowed != schema.end() && allowed->is_array() &&
        std::ranges::find(*allowed, value) == allowed->end()) {
        return ApiResult::failure("invalid_input", "expected one of " + allowed->dump(), path);
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        const auto bound = [&](const char* keyword) -> std::optional<double> {
            const auto found = schema.find(keyword);
            if (found == schema.end() || !found->is_number()) {
                return std::nullopt;
            }
            return found->get<double>();
        };
        if (const auto minimum = bound("minimum"); minimum && number < *minimum) {
            return ApiResult::failure(
                "invalid_input", "expected at least " + schema.at("minimum").dump(), path);
        }
        if (const auto maximum = bound("maximum"); maximum && number > *maximum) {
            return ApiResult::failure(
                "invalid_input", "expected at most " + schema.at("maximum").dump(), path);
        }
        if (const auto minimum = bound("exclusiveMinimum"); minimum && number <= *minimum) {
            return ApiResult::failure("invalid_input",
                                      "expected more than " + schema.at("exclusiveMinimum").dump(),
                                      path);
        }
        if (const auto maximum = bound("exclusiveMaximum"); maximum && number >= *maximum) {
            return ApiResult::failure("invalid_input",
                                      "expected less than " + schema.at("exclusiveMaximum").dump(),
                                      path);
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            auto result = validate_value(
                schema.at("items"), value[index], pointer_child(path, std::to_string(index)));
            if (!result.ok) {
                return result;
            }
        }
    }
    if (!value.is_object()) {
        return ApiResult::success(nullptr);
    }
    const auto properties = schema.value("properties", nlohmann::json::object());
    for (const auto& required : schema.value("required", nlohmann::json::array())) {
        const std::string key = required.get<std::string>();
        if (!value.contains(key) || value[key].is_null()) {
            return ApiResult::failure(
                "invalid_input", key + " is required", pointer_child(path, key));
        }
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        const std::string child = pointer_child(path, field.key());
        if (!properties.contains(field.key())) {
            if (path.empty() ||
                schema.value("additionalProperties", nlohmann::json(true)) == false) {
                return ApiResult::failure(
                    "invalid_input", "unexpected field " + field.key(), child);
            }
            continue;
        }
        // Preserve the existing optional top-level null-as-absent input convention.
        if (path.empty() && field.value().is_null()) {
            continue;
        }
        auto result = validate_value(properties.at(field.key()), field.value(), child);
        if (!result.ok) {
            return result;
        }
    }
    return ApiResult::success(nullptr);
}

struct CallState {
    std::shared_ptr<const Capability> capability;
    ApiResult result;
    bool proceed = true;
};

ApiResult
authorize_app_token(const Capability& entry, const nlohmann::json& input, ApiContext& context) {
    const auto refused = [] {
        return ApiResult::failure("unauthorized", "app token does not grant this operation");
    };
    if (!context.authorized || context.credential_workload_id.empty() || !context.store) {
        return refused();
    }
    const auto app = context.store->get_web_app(context.credential_workload_id);
    if (!app || web_app_token_purpose(*app, context.credential_token_hash) !=
                    context.credential_kind.substr(4) ||
        (!context.path_workspace_id.empty() && app->workspace_id != context.path_workspace_id)) {
        return refused();
    }
    context.audit_customer_id = app->customer_id;
    context.audit_object = "app/" + app->id;
    if (context.credential_kind == "app_source") {
        if (app->desired == "deleted" || app->mode != "runtime" ||
            (entry.name != "repository_tree" && entry.name != "repository_file") ||
            input.value("id", "") != app->source_repository ||
            input.value("ref", "") != app->source_revision) {
            return refused();
        }
        const auto repository = context.store->get_repository(app->source_repository);
        if (!repository || repository->customer_id != app->customer_id) {
            return refused();
        }
    } else if (context.credential_kind == "app_zetta") {
        if (app->desired == "deleted" ||
            input.value("workspace_id", "") != app->workspace_id ||
            (entry.name != "workspace_query" && entry.name != "workspace_sql") ||
            input.value("customer_id", "") != app->customer_id ||
            app->bindings.value("zetta", nlohmann::json()) !=
                nlohmann::json{{"scopes", nlohmann::json::array({"read"})}}) {
            return refused();
        }
    } else if (context.credential_kind == "app_events") {
        if (entry.name != "publish_app_event" || input.value("app_id", "") != app->id ||
            input.value("workspace_id", "") != app->workspace_id ||
            (input.contains("customer_id") && input.value("customer_id", "") != app->customer_id)) {
            return refused();
        }
    } else {
        return refused();
    }
    return ApiResult::success(nullptr);
}

ApiResult
authorize_build_token(const Capability& entry, const nlohmann::json& input, ApiContext& context) {
    const auto refused = [] {
        return ApiResult::failure("unauthorized", "build token does not grant this operation");
    };
    if (!context.authorized || context.credential_workload_id.empty() || !context.store) {
        return refused();
    }
    const auto build = context.store->get_build(context.credential_workload_id);
    if (!build || build_token_purpose(*build, context.credential_token_hash) !=
                      context.credential_kind.substr(6) ||
        (!context.path_workspace_id.empty() && build->workspace_id != context.path_workspace_id)) {
        return refused();
    }
    context.audit_customer_id = build->customer_id;
    context.audit_object = "build/" + build->id;
    if (context.credential_kind == "build_source") {
        const std::string repository = input.value("repository", input.value("id", ""));
        const std::string revision = input.value("revision", input.value("ref", ""));
        if ((entry.name != "repository_tree" && entry.name != "repository_file") ||
            repository != build->repository || revision != build->revision) {
            return refused();
        }
        const auto stored_repository = context.store->get_repository(build->repository);
        if (!stored_repository || stored_repository->customer_id != build->customer_id) {
            return refused();
        }
    } else if (context.credential_kind == "build_events") {
        if (entry.name != "publish_build_event" || input.value("build_id", "") != build->id ||
            input.value("workspace_id", "") != build->workspace_id ||
            (input.contains("customer_id") &&
             input.value("customer_id", "") != build->customer_id)) {
            return refused();
        }
    } else {
        return refused();
    }
    return ApiResult::success(nullptr);
}

const nlohmann::json& agent_task_input(const Capability& capability,
                                     const nlohmann::json& input,
                                     ApiContext& context,
                                     nlohmann::json& resolved) {
    constexpr std::string_view prefix = "task:";
    if (context.credential_kind != "agent_task" || !input.is_object() ||
        input.contains("workspace_id") || context.store == nullptr ||
        !context.credential_session_id.starts_with(prefix)) {
        return input;
    }
    const auto properties = capability.input_schema.find("properties");
    if (properties == capability.input_schema.end() ||
        !properties->contains("workspace_id")) {
        return input;
    }
    const auto task =
        context.store->get_agent_task(context.credential_session_id.substr(prefix.size()));
    if (!task) {
        return input;
    }
    resolved = input;
    resolved["workspace_id"] = task->workspace_id;
    return resolved;
}

const nlohmann::json& workspace_path_input(const Capability& capability,
                                           const nlohmann::json& input,
                                           const ApiContext& context,
                                           nlohmann::json& resolved) {
    if (context.path_workspace_id.empty() || !input.is_object() || input.contains("workspace_id")) {
        return input;
    }
    const auto properties = capability.input_schema.find("properties");
    if (properties == capability.input_schema.end() || !properties->contains("workspace_id")) {
        return input;
    }
    resolved = input;
    resolved["workspace_id"] = context.path_workspace_id;
    return resolved;
}

// An input refusal that leads with the field it is about ("confirm must be the service name",
// "storage_gi: too large") points at that field when the handler gave no path (lake#450).
void point_at_named_field(ApiResult& result, const nlohmann::json& input) {
    if (result.ok || result.error.code != "invalid_input" || !result.error.path.empty() ||
        !input.is_object()) {
        return;
    }
    const std::string& message = result.error.message;
    for (const auto& [key, value] : input.items()) {
        if (message.size() > key.size() && message.starts_with(key) &&
            std::string_view(" :.,").contains(message[key.size()])) {
            result.error.path = pointer_child("", key);
            return;
        }
    }
}

// No credential was accepted (lake#450): 401, saying whether one was missing, invalid or expired,
// and where a person makes a new key.
ApiResult unauthenticated(const ApiContext& context) {
    const std::string reason =
        context.credential_rejection.empty() ? "missing" : context.credential_rejection;
    const std::string message =
        reason == "expired"   ? "the credential has expired; sign in again or make a new key"
        : reason == "invalid" ? "the credential is not valid (unknown, revoked or mistyped); sign "
                                "in again or make a new key"
                              : "a credential is required: send an API key as a bearer token";
    ApiResult result = ApiResult::failure("unauthenticated", message);
    result.error.reason = reason;
    result.error.help = context.request_origin + "/developer/mcp";
    return result;
}

CallState prepare_call(std::shared_ptr<const Capability> capability,
                       const nlohmann::json& input,
                       ApiContext& context) {
    if (context.request_id.empty()) {
        context.request_id = control::new_uuid_v4();
    }
    context.audit_customer_id.clear();
    context.audit_kind.clear();
    context.audit_object.clear();
    context.audit_details = nlohmann::json::object();
    context.audit_workspace_id.clear();
    CallState state{.capability = std::move(capability)};
    if (context.authorized && context.credential_kind == "agent_task" &&
        context.credential_session_id.starts_with("task:") && context.store) {
        const auto renewed = context.store->renew_agent_task_lease(
            context.credential_session_id.substr(5), context.credential_token_hash);
        if (!renewed) {
            state.result = ApiResult::failure(renewed.error().code, renewed.error().message);
            state.proceed = false;
            return state;
        }
    }
    const Capability& entry = *state.capability;
    if (input.contains("workspace_id")) {
        context.workspace_id = input.at("workspace_id").is_string()
            ? input.at("workspace_id").get<std::string>() : std::string();
    }
    if (entry.requires_auth && !context.authorized && !entry.allows_anonymous_public_read) {
        state.result = unauthenticated(context);
        state.proceed = false;
    } else if (ApiResult validation = validate_input(entry, input); !validation.ok) {
        state.result = validation;
        state.proceed = false;
    } else if (!context.path_workspace_id.empty() && input.contains("workspace_id") &&
               context.workspace_id != context.path_workspace_id) {
        state.result = ApiResult::failure(
            "invalid_input", "workspace_id must match the workspace URL", "/workspace_id");
        state.proceed = false;
    } else if (context.credential_kind.starts_with("build_")) {
        state.result = authorize_build_token(entry, input, context);
        state.proceed = state.result.ok;
    } else if (context.credential_kind.starts_with("app_")) {
        state.result = authorize_app_token(entry, input, context);
        state.proceed = state.result.ok;
    } else if (entry.requires_auth && context.session_credential &&
               std::ranges::find(context.scopes, "*") == context.scopes.end() &&
               std::ranges::find(context.scopes, entry.name) == context.scopes.end()) {
        state.result =
            ApiResult::failure("unauthorized", "credential does not grant " + entry.name);
        state.proceed = false;
    } else if (entry.requires_auth && context.authorized && !entry.authorizes_candidates) {
        if (entry.object.empty() || entry.action.empty() || !entry.resolve_object) {
            state.result = ApiResult::failure("store_error",
                                              "capability authorization metadata is incomplete");
            state.proceed = false;
        } else if (!context.store) {
            state.result = ApiResult::failure(
                "store_unavailable", "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
            state.proceed = false;
        } else {
            std::expected<std::string, ApiError> object_id = std::string{};
            if (context.acting_identity != "admin" || entry.resolves_operator_audit_target) {
                object_id = entry.resolve_object(input, context);
            }
            if (!object_id) {
                state.result = ApiResult::failure(
                    object_id.error().code, object_id.error().message, object_id.error().path);
                state.proceed = false;
            } else if (context.acting_identity != "admin") {
                auto decision = control::authorize(*context.store,
                                                   context.acting_identity,
                                                   *object_id,
                                                   entry.action,
                                                   context.authorization_cache);
                if (!decision) {
                    state.result =
                        ApiResult::failure(decision.error().code, decision.error().message);
                    state.proceed = false;
                } else if (!decision->allowed) {
                    state.result = ApiResult::failure("unauthorized",
                                                      "principal is not allowed to " +
                                                          entry.action + " " + entry.object);
                    state.proceed = false;
                }
            }
        }
    }
    if (state.proceed && input.contains("workspace_id")) {
        const auto workspace = context.store
            ? context.store->get_workspace(context.workspace_id)
            : control::Result<control::Workspace>(std::unexpect,
                  control::StoreError{"invalid_input", "workspace_id is unknown"});
        if (!workspace || (!context.path_workspace_id.empty() &&
                           context.workspace_id != context.path_workspace_id)) {
            state.result = ApiResult::failure("invalid_input",
                "workspace_id must name a workspace Zetta in this space", "/workspace_id");
            state.proceed = false;
        } else if (input.contains("customer_id") &&
                   input.at("customer_id") != workspace->customer_id) {
            // The workspace exists, so the space named beside it is the one at fault (lake#450).
            state.result = ApiResult::failure("invalid_input",
                                              "customer_id is not the space that owns workspace " +
                                                  context.workspace_id,
                                              "/customer_id");
            state.proceed = false;
        }
        if (state.proceed && context.credential_kind == "agent_task" &&
            context.credential_session_id.starts_with("task:")) {
            const auto task = context.store->get_agent_task(context.credential_session_id.substr(5));
            if (!task || task->workspace_id != context.workspace_id) {
                state.result = ApiResult::failure("unauthorized", "task credential belongs to another workspace");
                state.proceed = false;
            }
        }
        if (state.proceed) {
            context.audit_workspace_id = workspace->id;
        }
    }
    if (state.proceed && !context.path_workspace_id.empty() &&
        context.credential_kind == "agent_task" &&
        context.credential_session_id.starts_with("task:")) {
        const auto task = context.store->get_agent_task(context.credential_session_id.substr(5));
        if (!task || task->workspace_id != context.path_workspace_id) {
            state.result =
                ApiResult::failure("unauthorized", "task credential belongs to another workspace");
            state.proceed = false;
        }
    }
    return state;
}

// The agent a task, launch or schedule credential belongs to. Those credentials act as the person
// who started the agent, so the audit principal names the person; this names the agent (lake#445).
std::string acting_agent(const ApiContext& context) {
    const auto id_after = [&](std::string_view prefix) -> std::string {
        return context.credential_session_id.starts_with(prefix)
                   ? context.credential_session_id.substr(prefix.size())
                   : std::string();
    };
    if (context.credential_kind == "agent_task") {
        if (const std::string id = id_after("task:"); !id.empty()) {
            if (const auto task = context.store->get_agent_task(id)) {
                return task->agent;
            }
        }
    } else if (context.credential_kind == "agent_launch") {
        if (const std::string id = id_after("launch:"); !id.empty()) {
            if (const auto launch = context.store->get_agent_launch(id)) {
                return launch->agent_name.empty() ? launch->agent_id : launch->agent_name;
            }
        }
    } else if (context.credential_kind == "agent_schedule") {
        if (const std::string id = id_after("schedule:"); !id.empty()) {
            if (const auto schedule = context.store->get_agent_cron_schedule(id)) {
                return schedule->agent_name.empty() ? schedule->agent_id : schedule->agent_name;
            }
        }
    }
    return {};
}

ApiResult finish_call(const Capability* capability,
                      const std::string& name,
                      const nlohmann::json& input,
                      ApiResult result,
                      ApiContext& context,
                      bool handled) {
    // An anonymous caller refused by a capability that otherwise needs a credential (a private
    // repository read) is refused for want of one.
    if (capability != nullptr && capability->requires_auth && !context.authorized && !result.ok &&
        result.error.code == "unauthorized") {
        const double duration = result.duration_ms;
        result = unauthenticated(context);
        result.duration_ms = duration;
    }
    if (!context.store) {
        return result;
    }
    const std::string kind = !context.audit_kind.empty()
                                 ? context.audit_kind
                                 : (name == "set_audit_retention" ? "retention" : "capability");
    nlohmann::json details = context.audit_details;
    details["error_code"] = result.ok ? "" : result.error.code;
    if (const std::string agent = acting_agent(context); !agent.empty()) {
        details["acting_agent"] = agent;
    }
    const audit::EventDraft event{
        .customer_id = context.audit_customer_id,
        .kind = kind,
        .principal = context.acting_identity.empty() ? "anonymous" : context.acting_identity,
        .object = context.audit_object.empty() ? "capability/" + name : context.audit_object,
        .action = name,
        .outcome = result.ok ? "success"
                             : (result.error.code == "unauthorized" ||
                                        result.error.code == "unauthenticated"
                                    ? "denied"
                                    : "failure"),
        .request_id = context.request_id,
        .method = context.authentication_method,
        .device = context.device,
        .details = std::move(details),
    };
    // Decision 52: events about a workspace's data go to that workspace's audit, queued without
    // I/O; the rest are signed into the central chain now.
    const AuditRoute route =
        audit_route(capability, name, input, context, event.outcome, handled);
    auto recorded = record_audit(context, event, route);
    if (!recorded) {
        return ApiResult::failure("store_error",
                                  "could not record audit event: " + recorded.error().message);
    }
    if (!*recorded) {
        return result;
    }
    if (kind == "dynamic_secret" && context.transparency_log) {
        auto appended = context.transparency_log->append_audit_event(**recorded);
        if (!appended) {
            return ApiResult::failure("store_error",
                                      "could not append audit event to transparency log: " +
                                          appended.error().message);
        }
    }
    return result;
}

} // namespace

SchemaAudience schema_audience(const ApiContext& context) {
    return context.credential_kind == "agent_task" ? SchemaAudience::AgentTask
                                                   : SchemaAudience::Caller;
}

nlohmann::json audience_input_schema(const Capability& capability, SchemaAudience audience) {
    nlohmann::json schema = capability.input_schema;
    if (audience != SchemaAudience::AgentTask || !schema.is_object()) {
        return schema;
    }
    const auto required = schema.find("required");
    if (required == schema.end() || !required->is_array()) {
        return schema;
    }
    const auto workspace = std::ranges::find(*required, "workspace_id");
    if (workspace != required->end()) {
        required->erase(workspace);
    }
    return schema;
}

ApiResult validate_input(const Capability& capability, const nlohmann::json& input) {
    if (!input.is_object())
        return ApiResult::failure("invalid_input", "input must be a JSON object");
    return validate_value(capability.input_schema, input, "");
}

std::expected<nlohmann::json, ApiError> canonical_input(const Capability& capability,
                                                        const nlohmann::json& input) {
    if (capability.input_aliases.empty() || !input.is_object()) {
        return input;
    }
    nlohmann::json canonical = input;
    for (const auto& [alias, name] : capability.input_aliases) {
        const auto found = canonical.find(alias);
        if (found == canonical.end()) {
            continue;
        }
        if (const auto current = canonical.find(name);
            current != canonical.end() && !current->is_null() && *current != *found) {
            return std::unexpected(
                ApiError{"invalid_input",
                         alias + " is the deprecated name of " + name + "; send only " + name,
                         pointer_child("", alias)});
        }
        nlohmann::json value = std::move(*found);
        canonical.erase(alias);
        canonical[name] = std::move(value);
    }
    return canonical;
}

bool Registry::add(Capability capability) {
    if (capability.name.empty() || find(capability.name))
        return false;
    // A deprecated alias is listed in the schema, so OpenAPI and MCP show it (docs/api/naming.md).
    for (const auto& [alias, name] : capability.input_aliases) {
        auto& properties = capability.input_schema["properties"];
        if (!properties.contains(name) || properties.contains(alias)) {
            return false;
        }
        nlohmann::json property = properties.at(name);
        property["deprecated"] = true;
        property["description"] = "Deprecated: the old name of `" + name + "`; send `" + name +
                                  "`. Accepted while callers move.";
        properties[alias] = std::move(property);
    }
    entries_.push_back(std::make_shared<const Capability>(std::move(capability)));
    return true;
}

std::shared_ptr<const Capability> Registry::find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry->name == name)
            return entry;
    }
    return nullptr;
}

std::vector<std::shared_ptr<const Capability>> Registry::list() const {
    return entries_;
}

ApiResult
Registry::call(const std::string& name, const nlohmann::json& input, ApiContext& context) const {
    context.registry = this;
    const auto capability = find(name);
    if (!capability)
        return ApiResult::failure("unknown_capability", "no capability named " + name);
    const auto canonical = canonical_input(*capability, input);
    if (!canonical) {
        return finish_call(capability.get(),
                           name,
                           input,
                           ApiResult::failure(canonical.error().code,
                                              canonical.error().message,
                                              canonical.error().path),
                           context,
                           false);
    }
    nlohmann::json workspace_resolved_input;
    const nlohmann::json& workspace_input =
        workspace_path_input(*capability, *canonical, context, workspace_resolved_input);
    nlohmann::json task_resolved_input;
    const nlohmann::json& call_input =
        agent_task_input(*capability, workspace_input, context, task_resolved_input);
    CallState state = prepare_call(capability, call_input, context);
    if (state.proceed) {
        const auto start = std::chrono::steady_clock::now();
        try {
            if (capability->handler) {
                state.result = capability->handler(call_input, context);
            } else {
                state.result =
                    ApiResult::failure("store_error", "capability requires asynchronous dispatch");
            }
        } catch (const std::exception& error) {
            state.result = ApiResult::failure("store_error", error.what());
        }
        point_at_named_field(state.result, call_input);
        state.result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    return finish_call(
        capability.get(), name, call_input, std::move(state.result), context, state.proceed);
}

liquid::runtime::task<ApiResult> Registry::call_async(const std::string& name,
                                                      const nlohmann::json& input,
                                                      ApiContext& context) const {
    context.registry = this;
    const auto capability = find(name);
    if (!capability) {
        co_return ApiResult::failure("unknown_capability", "no capability named " + name);
    }
    const auto canonical = canonical_input(*capability, input);
    if (!canonical) {
        co_return finish_call(capability.get(),
                              name,
                              input,
                              ApiResult::failure(canonical.error().code,
                                                 canonical.error().message,
                                                 canonical.error().path),
                              context,
                              false);
    }
    nlohmann::json workspace_resolved_input;
    const nlohmann::json& workspace_input =
        workspace_path_input(*capability, *canonical, context, workspace_resolved_input);
    nlohmann::json task_resolved_input;
    const nlohmann::json& call_input =
        agent_task_input(*capability, workspace_input, context, task_resolved_input);
    CallState state = prepare_call(capability, call_input, context);
    if (state.proceed) {
        const auto start = std::chrono::steady_clock::now();
        try {
            if (capability->async_handler) {
                auto completed = co_await capability->async_handler(call_input, context);
                if (!completed) {
                    state.result = ApiResult::failure("store_error", completed.error().message);
                } else {
                    state.result = std::move(*completed);
                }
            } else if (capability->handler) {
                state.result = capability->handler(call_input, context);
            } else {
                state.result = ApiResult::failure("store_error", "capability has no handler");
            }
        } catch (const std::exception& error) {
            state.result = ApiResult::failure("store_error", error.what());
        }
        point_at_named_field(state.result, call_input);
        state.result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    co_return finish_call(
        capability.get(), name, call_input, std::move(state.result), context, state.proceed);
}

} // namespace lake::api
