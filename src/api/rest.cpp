#include "lake/api/rest.hpp"

#include <charconv>

namespace lake::api {

std::string rest_path(const Capability& capability) { return "/v1/" + capability.name; }

std::string rest_method(const Capability& capability) {
    return capability.kind == CapabilityKind::Read ? "GET" : "POST";
}

namespace {

nlohmann::json coerce(const nlohmann::json& property, const std::string& raw) {
    const std::string type = property.value("type", "");
    if (type == "integer") {
        long long value = 0;
        auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (ec == std::errc() && end == raw.data() + raw.size()) return value;
        return raw;
    }
    if (type == "number") {
        try {
            std::size_t used = 0;
            const double value = std::stod(raw, &used);
            if (used == raw.size()) return value;
        } catch (...) {
        }
        return raw;
    }
    if (type == "boolean") {
        if (raw == "true") return true;
        if (raw == "false") return false;
        return raw;
    }
    if (type == "array" || type == "object") {
        const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
        if (!parsed.is_discarded()) return parsed;
        return raw;
    }
    return raw;
}

nlohmann::json error_envelope_schema() {
    return {{"type", "object"},
            {"properties",
             {{"ok", {{"const", false}}},
              {"error",
               {{"type", "object"},
                {"properties",
                 {{"code", {{"type", "string"}}}, {"message", {{"type", "string"}}}, {"path", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"code", "message"})}}},
              {"duration_ms", {{"type", "number"}}}}},
            {"required", nlohmann::json::array({"ok", "error"})}};
}

nlohmann::json success_envelope_schema(const nlohmann::json& output_schema) {
    nlohmann::json value = output_schema.is_object() && !output_schema.empty() ? output_schema : nlohmann::json{{"type", "object"}};
    return {{"type", "object"},
            {"properties", {{"ok", {{"const", true}}}, {"value", value}, {"duration_ms", {{"type", "number"}}}}},
            {"required", nlohmann::json::array({"ok", "value"})}};
}

nlohmann::json error_response(std::string_view description) {
    return {{"description", std::string(description)},
            {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/error_envelope"}}}}}}}};
}

}  // namespace

nlohmann::json rest_input_from_query(const Capability& capability,
                                     const std::vector<std::pair<std::string, std::string>>& query) {
    nlohmann::json input = nlohmann::json::object();
    const nlohmann::json properties = capability.input_schema.value("properties", nlohmann::json::object());
    for (const auto& [key, value] : query) {
        input[key] = properties.contains(key) ? coerce(properties[key], value) : nlohmann::json(value);
    }
    return input;
}

int rest_status(const ApiResult& result) {
    if (result.ok) return 200;
    const std::string& code = result.error.code;
    if (code == "invalid_input") return 400;
    if (code == "unauthorized") return 403;
    if (code == "not_found" || code == "unknown_capability") return 404;
    if (code == "conflict") return 409;
    if (code == "store_unavailable") return 503;
    return 500;
}

nlohmann::json rest_envelope(const ApiResult& result) {
    nlohmann::json envelope;
    envelope["ok"] = result.ok;
    if (result.ok) {
        envelope["value"] = result.value;
    } else {
        envelope["error"] = {{"code", result.error.code}, {"message", result.error.message}, {"path", result.error.path}};
    }
    envelope["duration_ms"] = result.duration_ms;
    return envelope;
}

nlohmann::json openapi_document(const Registry& registry, std::string_view version) {
    nlohmann::json document;
    document["openapi"] = "3.1.0";
    document["info"] = {
        {"title", "lake control surface"},
        {"version", std::string(version)},
        {"description",
         "The capability registry rendered as HTTP routes: reads are GET /v1/<name> (input in the query "
         "string), actions are POST /v1/<name> (input as the JSON body). The same registry is served as "
         "MCP tools on POST /mcp — one registry, two renderings, zero drift."}};
    document["servers"] = nlohmann::json::array({{{"url", "/"}}});
    document["components"] = {
        {"securitySchemes",
         {{"bearer_token",
           {{"type", "http"},
            {"scheme", "bearer"},
            {"description",
             "the Lake's admin token (Secret lake-<name>-admin, injected as LAKE_ADMIN_TOKEN). Required on "
             "every capability except server_info."}}}}},
        {"schemas", {{"error_envelope", error_envelope_schema()}}}};
    document["security"] = nlohmann::json::array({{{"bearer_token", nlohmann::json::array()}}});

    nlohmann::json paths = nlohmann::json::object();
    for (const auto& capability : registry.list()) {
        nlohmann::json operation;
        operation["operationId"] = capability->name;
        operation["summary"] = capability->title;
        operation["description"] = capability->description;
        operation["tags"] = nlohmann::json::array({std::string(to_string(capability->kind))});
        if (!capability->requires_auth) operation["security"] = nlohmann::json::array();
        const std::string method = rest_method(*capability);
        const nlohmann::json properties = capability->input_schema.value("properties", nlohmann::json::object());
        const nlohmann::json required = capability->input_schema.value("required", nlohmann::json::array());
        if (method == "GET") {
            nlohmann::json parameters = nlohmann::json::array();
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                bool is_required = false;
                for (const auto& r : required) is_required = is_required || r == it.key();
                nlohmann::json parameter = {{"name", it.key()}, {"in", "query"}, {"required", is_required}, {"schema", it.value()}};
                if (it.value().contains("description")) parameter["description"] = it.value()["description"];
                parameters.push_back(std::move(parameter));
            }
            operation["parameters"] = std::move(parameters);
        } else {
            operation["requestBody"] = {{"required", !required.empty()},
                                        {"content", {{"application/json", {{"schema", capability->input_schema}}}}}};
        }
        nlohmann::json responses;
        responses["200"] = {{"description", "the call succeeded"},
                            {"content", {{"application/json", {{"schema", success_envelope_schema(capability->output_schema)}}}}}};
        responses["4XX"] = error_response("the input, authorization, or addressed entity is at fault; the error carries the reason");
        responses["5XX"] = error_response("the control-plane store is unavailable or failed");
        operation["responses"] = std::move(responses);
        paths[rest_path(*capability)][method == "GET" ? "get" : "post"] = std::move(operation);
    }
    paths["/mcp"]["post"] = {
        {"operationId", "mcp"},
        {"summary", "MCP (JSON-RPC 2.0): initialize, ping, tools/list, tools/call"},
        {"description", "Every registry entry is a tool; tools/call runs the same authorized, validated call as its REST route."},
        {"tags", nlohmann::json::array({"mcp"})},
        {"requestBody", {{"required", true}, {"content", {{"application/json", {{"schema", {{"type", "object"}}}}}}}}},
        {"responses", {{"200", {{"description", "a JSON-RPC 2.0 response"}}}, {"202", {{"description", "a notification was accepted"}}}}}};
    document["paths"] = std::move(paths);
    return document;
}

}  // namespace lake::api
