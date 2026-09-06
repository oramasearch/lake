#include "lake/api/registry.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"
#include "lake/transparency/log.hpp"

namespace lake::api {

ApiResult ApiResult::success(nlohmann::json output) {
    ApiResult result;
    result.ok = true;
    result.value = std::move(output);
    return result;
}

ApiResult ApiResult::failure(std::string code, std::string message, std::string path) {
    ApiResult result;
    result.ok = false;
    result.error = ApiError{std::move(code), std::move(message), std::move(path)};
    return result;
}

std::string_view to_string(CapabilityKind kind) {
    return kind == CapabilityKind::Read ? "read" : "action";
}

namespace {

bool matches_type(const nlohmann::json& value, const std::string& type) {
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

struct CallState {
    std::shared_ptr<const Capability> capability;
    ApiResult result;
    bool proceed = true;
};

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
    if (entry.requires_auth && !context.authorized && !entry.allows_anonymous_public_read) {
        state.result = ApiResult::failure("unauthorized",
                                          "a valid session or operator credential is required");
        state.proceed = false;
    } else if (ApiResult validation = validate_input(entry, input); !validation.ok) {
        state.result = validation;
        state.proceed = false;
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
    const nlohmann::json properties =
        capability.input_schema.value("properties", nlohmann::json::object());
    for (const auto& required :
         capability.input_schema.value("required", nlohmann::json::array())) {
        const std::string key = required.get<std::string>();
        if (!input.contains(key) || input[key].is_null()) {
            return ApiResult::failure("invalid_input", key + " is required", "/" + key);
        }
    }
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (!properties.contains(it.key())) {
            return ApiResult::failure(
                "invalid_input", "unexpected field " + it.key(), "/" + it.key());
        }
        if (it.value().is_null())
            continue;
        const std::string type = properties[it.key()].value("type", "");
        if (!type.empty() && !matches_type(it.value(), type)) {
            return ApiResult::failure(
                "invalid_input", it.key() + " must be a " + type, "/" + it.key());
        }
    }
    return ApiResult::success(nullptr);
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
    const auto capability = find(name);
    if (!capability)
        return ApiResult::failure("unknown_capability", "no capability named " + name);
    CallState state = prepare_call(capability, input, context);
    if (state.proceed) {
        const auto start = std::chrono::steady_clock::now();
        try {
            if (capability->handler) {
                state.result = capability->handler(input, context);
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
    const auto capability = find(name);
    if (!capability) {
        co_return ApiResult::failure("unknown_capability", "no capability named " + name);
    }
    CallState state = prepare_call(capability, input, context);
    if (state.proceed) {
        const auto start = std::chrono::steady_clock::now();
        try {
            if (capability->async_handler) {
                auto completed = co_await capability->async_handler(input, context);
                if (!completed) {
                    state.result = ApiResult::failure("store_error", completed.error().message);
                } else {
                    state.result = std::move(*completed);
                }
            } else if (capability->handler) {
                state.result = capability->handler(input, context);
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
