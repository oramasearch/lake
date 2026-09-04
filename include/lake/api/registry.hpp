#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {
class Store;
}
namespace lake::search {
class Service;
}

// The capability registry — lake's control surface in Zetta's shape: one registry, rendered as
// REST (/v1/<name>), OpenAPI (/v1/openapi.json) and MCP tools (POST /mcp), zero drift.
namespace lake::api {

struct ApiError {
    std::string code;     // invalid_input | unauthorized | not_found | conflict | unknown_capability
                          // | store_unavailable | store_error
    std::string message;  // the real reason, verbatim where one exists
    std::string path;     // JSON pointer when the input is at fault
};

struct ApiResult {
    bool ok = false;
    nlohmann::json value;
    ApiError error;
    double duration_ms = 0.0;
    static ApiResult success(nlohmann::json output);
    static ApiResult failure(std::string code, std::string message, std::string path = {});
};

enum class CapabilityKind { Read, Action };
std::string_view to_string(CapabilityKind kind);

struct ApiContext {
    control::Store* store = nullptr;  // null = no control-plane store configured
    search::Service* search = nullptr;
    std::string node_name;
    std::string acting_identity;  // "admin" for the bearer, empty otherwise
    bool authorized = false;
};

using CapabilityHandler = std::function<ApiResult(const nlohmann::json& input, ApiContext& context)>;

struct Capability {
    std::string name;  // snake_case: verb-first actions, noun reads
    std::string title;
    std::string description;
    nlohmann::json input_schema;   // object schema: properties{type}, required, additionalProperties false
    nlohmann::json output_schema;  // the success value's shape
    CapabilityKind kind = CapabilityKind::Read;
    bool requires_auth = true;
    CapabilityHandler handler;
};

// Boundary validation against the schema subset lake declares: required keys, declared keys
// only, declared JSON types. Returns ok on success, invalid_input (path-qualified) otherwise.
ApiResult validate_input(const Capability& capability, const nlohmann::json& input);

class Registry {
public:
    bool add(Capability capability);
    std::shared_ptr<const Capability> find(const std::string& name) const;
    std::vector<std::shared_ptr<const Capability>> list() const;
    // Authorize (requires_auth vs context.authorized), validate, run, time.
    ApiResult call(const std::string& name, const nlohmann::json& input, ApiContext& context) const;

private:
    std::vector<std::shared_ptr<const Capability>> entries_;
};

}  // namespace lake::api
