#include "lake/api/registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <print>

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

} // namespace

ApiResult ApiResult::success(nlohmann::json output) {
    ApiResult result;
    result.ok = true;
    result.value = std::move(output);
    return result;
}

ApiResult ApiResult::failure(std::string code, std::string message, std::string path) {
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
    }
    envelope["duration_ms"] = duration_ms;
    return envelope;
}

nlohmann::json error_envelope_schema() {
    return {{"type", "object"},
            {"properties",
             {{"ok", {{"const", false}}},
              {"error",
               {{"type", "object"},
                {"properties",
                 {{"code", {{"type", "string"}}},
                  {"message", {{"type", "string"}}},
                  {"path", {{"type", "string"}}}}},
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
                    context.credential_kind.substr(4)) {
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
                      context.credential_kind.substr(6)) {
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
    CallState state{.capability = std::move(capability)};
    const Capability& entry = *state.capability;
    if (input.contains("workspace_id")) {
        context.workspace_id = input.at("workspace_id").is_string()
            ? input.at("workspace_id").get<std::string>() : std::string();
    }
    if (entry.requires_auth && !context.authorized && !entry.allows_anonymous_public_read) {
        state.result = ApiResult::failure("unauthorized",
                                          "a valid session or operator credential is required");
        state.proceed = false;
    } else if (ApiResult validation = validate_input(entry, input); !validation.ok) {
        state.result = validation;
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
        if (!workspace ||
            (input.contains("customer_id") && input.at("customer_id") != workspace->customer_id) ||
            (!context.path_workspace_id.empty() && context.workspace_id != context.path_workspace_id)) {
            state.result = ApiResult::failure("invalid_input",
                "workspace_id must name a workspace Zetta in this space", "/workspace_id");
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
    }
    return state;
}

ApiResult finish_call(const std::string& name, ApiResult result, ApiContext& context) {
    if (!context.store) {
        return result;
    }
    const std::string kind = !context.audit_kind.empty()
                                 ? context.audit_kind
                                 : (name == "set_audit_retention" ? "retention" : "capability");
    nlohmann::json details = context.audit_details;
    details["error_code"] = result.ok ? "" : result.error.code;
    const audit::EventDraft event{
        .customer_id = context.audit_customer_id,
        .kind = kind,
        .principal = context.acting_identity.empty() ? "anonymous" : context.acting_identity,
        .object = context.audit_object.empty() ? "capability/" + name : context.audit_object,
        .action = name,
        .outcome =
            result.ok ? "success" : (result.error.code == "unauthorized" ? "denied" : "failure"),
        .request_id = context.request_id,
        .method = context.authentication_method,
        .device = context.device,
        .details = std::move(details),
    };
    const audit::IdentityOptions options{
        .configured_seed = context.identity_seed,
        .instance_kek = context.instance_kek,
    };
    auto recorded = audit::record(*context.store, event, options);
    if (!recorded) {
        return ApiResult::failure("store_error",
                                  "could not record audit event: " + recorded.error().message);
    }
    if (kind == "dynamic_secret" && context.transparency_log) {
        auto appended = context.transparency_log->append_audit_event(*recorded);
        if (!appended) {
            return ApiResult::failure("store_error",
                                      "could not append audit event to transparency log: " +
                                          appended.error().message);
        }
    }
    return result;
}

} // namespace

ApiResult validate_input(const Capability& capability, const nlohmann::json& input) {
    if (!input.is_object())
        return ApiResult::failure("invalid_input", "input must be a JSON object");
    return validate_value(capability.input_schema, input, "");
}

bool Registry::add(Capability capability) {
    if (capability.name.empty() || find(capability.name))
        return false;
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
    nlohmann::json resolved_input;
    const nlohmann::json& call_input =
        agent_task_input(*capability, input, context, resolved_input);
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
        state.result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    return finish_call(name, std::move(state.result), context);
}

liquid::runtime::task<ApiResult> Registry::call_async(const std::string& name,
                                                      const nlohmann::json& input,
                                                      ApiContext& context) const {
    context.registry = this;
    const auto capability = find(name);
    if (!capability) {
        co_return ApiResult::failure("unknown_capability", "no capability named " + name);
    }
    nlohmann::json resolved_input;
    const nlohmann::json& call_input =
        agent_task_input(*capability, input, context, resolved_input);
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
        state.result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    co_return finish_call(name, std::move(state.result), context);
}

} // namespace lake::api
