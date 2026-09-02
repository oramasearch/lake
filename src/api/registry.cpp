#include "lake/api/registry.hpp"

#include <chrono>
#include <exception>

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
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    return true;
}

}  // namespace

ApiResult validate_input(const Capability& capability, const nlohmann::json& input) {
    if (!input.is_object()) return ApiResult::failure("invalid_input", "input must be a JSON object");
    const nlohmann::json properties = capability.input_schema.value("properties", nlohmann::json::object());
    for (const auto& required : capability.input_schema.value("required", nlohmann::json::array())) {
        const std::string key = required.get<std::string>();
        if (!input.contains(key) || input[key].is_null()) {
            return ApiResult::failure("invalid_input", key + " is required", "/" + key);
        }
    }
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (!properties.contains(it.key())) {
            return ApiResult::failure("invalid_input", "unexpected field " + it.key(), "/" + it.key());
        }
        if (it.value().is_null()) continue;
        const std::string type = properties[it.key()].value("type", "");
        if (!type.empty() && !matches_type(it.value(), type)) {
            return ApiResult::failure("invalid_input", it.key() + " must be a " + type, "/" + it.key());
        }
    }
    return ApiResult::success(nullptr);
}

bool Registry::add(Capability capability) {
    if (capability.name.empty() || find(capability.name)) return false;
    entries_.push_back(std::make_shared<const Capability>(std::move(capability)));
    return true;
}

std::shared_ptr<const Capability> Registry::find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry->name == name) return entry;
    }
    return nullptr;
}

std::vector<std::shared_ptr<const Capability>> Registry::list() const { return entries_; }

ApiResult Registry::call(const std::string& name, const nlohmann::json& input, ApiContext& context) const {
    const auto capability = find(name);
    if (!capability) return ApiResult::failure("unknown_capability", "no capability named " + name);
    if (capability->requires_auth && !context.authorized) {
        return ApiResult::failure("unauthorized", "a valid admin bearer token is required");
    }
    if (ApiResult validation = validate_input(*capability, input); !validation.ok) return validation;
    const auto start = std::chrono::steady_clock::now();
    ApiResult result;
    try {
        result = capability->handler(input, context);
    } catch (const std::exception& error) {
        result = ApiResult::failure("store_error", error.what());
    }
    result.duration_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace lake::api
