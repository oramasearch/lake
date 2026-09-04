#include "lake/api/registry.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"

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
    if (context.request_id.empty()) {
        context.request_id = control::new_uuid_v4();
    }
    context.audit_customer_id.clear();
    context.audit_kind.clear();
    context.audit_object.clear();
    context.audit_details = nlohmann::json::object();
    const auto capability = find(name);
    if (!capability)
        return ApiResult::failure("unknown_capability", "no capability named " + name);
    ApiResult result;
    bool proceed = true;
    if (capability->requires_auth && !context.authorized) {
        result = ApiResult::failure("unauthorized",
                                    "a valid session or operator credential is required");
        proceed = false;
    } else if (ApiResult validation = validate_input(*capability, input); !validation.ok) {
        result = validation;
        proceed = false;
    } else if (capability->requires_auth && context.session_credential &&
               std::ranges::find(context.scopes, "*") == context.scopes.end() &&
               std::ranges::find(context.scopes, capability->name) == context.scopes.end()) {
        result =
            ApiResult::failure("unauthorized", "credential does not grant " + capability->name);
        proceed = false;
    } else if (capability->requires_auth && !capability->authorizes_candidates) {
        if (capability->object.empty() || capability->action.empty() ||
            !capability->resolve_object) {
            result = ApiResult::failure("store_error",
                                        "capability authorization metadata is incomplete");
            proceed = false;
        } else if (!context.store) {
            result = ApiResult::failure(
                "store_unavailable", "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
            proceed = false;
        } else {
            std::expected<std::string, ApiError> object_id = std::string{};
            if (context.acting_identity != "admin" || capability->resolves_operator_audit_target) {
                object_id = capability->resolve_object(input, context);
            }
            if (!object_id) {
                result = ApiResult::failure(
                    object_id.error().code, object_id.error().message, object_id.error().path);
                proceed = false;
            } else if (context.acting_identity != "admin") {
                auto decision = control::authorize(*context.store,
                                                   context.acting_identity,
                                                   *object_id,
                                                   capability->action,
                                                   context.authorization_cache);
                if (!decision) {
                    result = ApiResult::failure(decision.error().code, decision.error().message);
                    proceed = false;
                } else if (!decision->allowed) {
                    result = ApiResult::failure("unauthorized",
                                                "principal is not allowed to " +
                                                    capability->action + " " + capability->object);
                    proceed = false;
                }
            }
        }
    }
    if (proceed) {
        const auto start = std::chrono::steady_clock::now();
        try {
            result = capability->handler(input, context);
        } catch (const std::exception& error) {
            result = ApiResult::failure("store_error", error.what());
        }
        result.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    if (!context.store) {
        return result;
    }
    // One audit event per call, whatever the outcome; a denial is an outcome, not an exception.
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
    return result;
}

} // namespace lake::api
