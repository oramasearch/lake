#include "lake/api/rest.hpp"

#include <charconv>

namespace lake::api {

std::string rest_path(const Capability& capability) {
    return "/v1/" + capability.name;
}

std::string rest_method(const Capability& capability) {
    return capability.kind == CapabilityKind::Read ? "GET" : "POST";
}

namespace {

nlohmann::json coerce(const nlohmann::json& property, const std::string& raw) {
    const std::string type = property.value("type", "");
    if (type == "integer") {
        long long value = 0;
        auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (ec == std::errc() && end == raw.data() + raw.size())
            return value;
        return raw;
    }
    if (type == "number") {
        try {
            std::size_t used = 0;
            const double value = std::stod(raw, &used);
            if (used == raw.size())
                return value;
        } catch (...) {
        }
        return raw;
    }
    if (type == "boolean") {
        if (raw == "true")
            return true;
        if (raw == "false")
            return false;
        return raw;
    }
    if (type == "array" || type == "object") {
        const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
        if (!parsed.is_discarded())
            return parsed;
        return raw;
    }
    return raw;
}

nlohmann::json error_response(std::string_view description) {
    return {
        {"description", std::string(description)},
        {"content",
         {{"application/json", {{"schema", {{"$ref", "#/components/schemas/error_envelope"}}}}}}}};
}

std::string sync_path(const Capability& capability) {
    const std::string base = "/v1/repositories/{repository_id}/sync/";
    if (capability.name == "find_missing") {
        return base + "find-missing";
    }
    if (capability.name == "upload_chunks") {
        return base + "chunks";
    }
    if (capability.name == "read_chunk") {
        return base + "chunks/{digest}";
    }
    return base + capability.name;
}

nlohmann::json sync_parameter(std::string name, std::string location, bool required) {
    return {{"name", std::move(name)},
            {"in", std::move(location)},
            {"required", required},
            {"schema", {{"type", "string"}}}};
}

nlohmann::json sync_ref_outcome_schema() {
    return {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"outcome", {{"type", "string"}, {"enum", {"applied", "refused"}}}},
          {"reason", {{"type", "string"}}}}},
        {"required", {"name", "outcome"}},
        {"additionalProperties", false},
    };
}

nlohmann::json sync_binary_schema() {
    return {{"type", "string"}, {"format", "binary"}};
}

nlohmann::json sync_operation(const Capability& capability) {
    nlohmann::json operation;
    operation["operationId"] = capability.name;
    operation["summary"] = capability.title;
    operation["description"] = capability.description;
    operation["tags"] = nlohmann::json::array({"liquid-sync"});
    operation["parameters"] =
        nlohmann::json::array({sync_parameter("repository_id", "path", true)});
    if (capability.name == "read_chunk") {
        operation["parameters"].push_back(sync_parameter("digest", "path", true));
        auto offset = sync_parameter("offset", "query", true);
        offset["schema"]["type"] = "integer";
        offset["schema"]["minimum"] = 0;
        operation["parameters"].push_back(std::move(offset));
        auto length = sync_parameter("length", "query", true);
        length["schema"]["type"] = "integer";
        length["schema"]["minimum"] = 0;
        operation["parameters"].push_back(std::move(length));
    } else {
        nlohmann::json request_schema = sync_binary_schema();
        if (capability.name == "push") {
            request_schema["description"] =
                "A liquid sync-v1 metadata stream. Its canonical-CBOR header may set atomic true.";
            request_schema["x-liquid-sync-header-schema"] = {
                {"type", "object"},
                {"properties", {{"atomic", {{"type", "boolean"}}}}},
            };
        }
        operation["requestBody"] = {
            {"required", true},
            {"content",
             {{"application/vnd.liquid.sync-v1", {{"schema", std::move(request_schema)}}}}},
        };
    }
    nlohmann::json success = {{"description", "the sync operation succeeded"}};
    if (capability.name != "upload_chunks") {
        nlohmann::json response_schema = sync_binary_schema();
        if (capability.name == "push") {
            success["description"] =
                "The push was accepted and reports every requested reference outcome.";
            response_schema["description"] =
                "A liquid sync-v1 acknowledgement stream. Its canonical-CBOR header carries "
                "outcomes in request-reference order.";
            response_schema["x-liquid-sync-header-schema"] = {
                {"type", "object"},
                {"properties",
                 {{"outcomes", {{"type", "array"}, {"items", sync_ref_outcome_schema()}}}}},
                {"required", {"outcomes"}},
            };
        }
        success["content"] = {
            {"application/vnd.liquid.sync-v1", {{"schema", std::move(response_schema)}}},
        };
    }
    operation["responses"] = {
        {"200", std::move(success)},
        {"4XX",
         {{"description", "the request or authorization was rejected"},
          {"content",
           {{"application/vnd.liquid.error-v1",
             {{"schema", {{"type", "string"}, {"format", "binary"}}}}}}}}},
        {"5XX",
         {{"description", "the repository store failed"},
          {"content",
           {{"application/vnd.liquid.error-v1",
             {{"schema", {{"type", "string"}, {"format", "binary"}}}}}}}}},
    };
    return operation;
}

} // namespace

nlohmann::json
rest_input_from_query(const Capability& capability,
                      const std::vector<std::pair<std::string, std::string>>& query) {
    nlohmann::json input = nlohmann::json::object();
    const nlohmann::json properties =
        capability.input_schema.value("properties", nlohmann::json::object());
    for (const auto& [key, value] : query) {
        input[key] =
            properties.contains(key) ? coerce(properties[key], value) : nlohmann::json(value);
    }
    return input;
}

int rest_status(const ApiResult& result) {
    if (result.ok) {
        return 200;
    }
    const std::string& code = result.error.code;
    if (code == "invalid_input") {
        return 400;
    }
    if (code == "unauthorized" || code == "mfa_required") {
        return 403;
    }
    if (code == "not_found" || code == "unknown_capability") {
        return 404;
    }
    if (code == "conflict" || code == "unrecoverable") {
        return 409;
    }
    if (code == "store_unavailable") {
        return 503;
    }
    return 500;
}

nlohmann::json rest_envelope(const ApiResult& result) {
    return result.envelope();
}

nlohmann::json openapi_document(const Registry& registry, std::string_view version) {
    nlohmann::json document;
    document["openapi"] = "3.1.0";
    document["info"] = {{"title", "lake control surface"},
                        {"version", std::string(version)},
                        {"description",
                         "The capability registry rendered as HTTP routes: JSON reads are GET "
                         "/v1/<name> (input in the "
                         "query string), JSON actions are POST /v1/<name> (input as the body), and "
                         "liquid sync uses "
                         "repository-scoped binary routes. JSON capabilities are also served as "
                         "MCP tools on POST /mcp."}};
    document["servers"] = nlohmann::json::array({{{"url", "/"}}});
    document["components"] = {{"securitySchemes",
                               {{"bearer_token",
                                 {{"type", "http"},
                                  {"scheme", "bearer"},
                                  {"description",
                                   "the Lake's admin token (Secret lake-<name>-admin, injected as "
                                   "LAKE_ADMIN_TOKEN). Required on "
                                   "every capability except server_info."}}}}},
                              {"schemas", {{"error_envelope", error_envelope_schema()}}}};
    document["security"] = nlohmann::json::array({{{"bearer_token", nlohmann::json::array()}}});

    nlohmann::json paths = nlohmann::json::object();
    for (const auto& capability : registry.list()) {
        if (capability->transport == CapabilityTransport::Acme) {
            continue;
        }
        if (capability->transport == CapabilityTransport::LiquidSync) {
            const std::string method = capability->name == "read_chunk" ? "get" : "post";
            paths[sync_path(*capability)][method] = sync_operation(*capability);
            continue;
        }
        nlohmann::json operation;
        operation["operationId"] = capability->name;
        operation["summary"] = capability->title;
        operation["description"] = capability->description;
        operation["tags"] = nlohmann::json::array({std::string(to_string(capability->kind))});
        if (!capability->requires_auth)
            operation["security"] = nlohmann::json::array();
        const std::string method = rest_method(*capability);
        const nlohmann::json properties =
            capability->input_schema.value("properties", nlohmann::json::object());
        const nlohmann::json required =
            capability->input_schema.value("required", nlohmann::json::array());
        if (method == "GET") {
            nlohmann::json parameters = nlohmann::json::array();
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                bool is_required = false;
                for (const auto& r : required)
                    is_required = is_required || r == it.key();
                nlohmann::json parameter = {{"name", it.key()},
                                            {"in", "query"},
                                            {"required", is_required},
                                            {"schema", it.value()}};
                if (it.value().contains("description"))
                    parameter["description"] = it.value()["description"];
                parameters.push_back(std::move(parameter));
            }
            operation["parameters"] = std::move(parameters);
        } else {
            operation["requestBody"] = {
                {"required", !required.empty()},
                {"content", {{"application/json", {{"schema", capability->input_schema}}}}}};
        }
        nlohmann::json responses;
        responses["200"] = {{"description", "the call succeeded"},
                            {"content",
                             {{"application/json",
                               {{"schema", success_envelope_schema(capability->output_schema)}}}}}};
        responses["4XX"] = error_response("the input, authorization, or addressed entity is at "
                                          "fault; the error carries the reason");
        responses["5XX"] = error_response("the control-plane store is unavailable or failed");
        operation["responses"] = std::move(responses);
        paths[rest_path(*capability)][method == "GET" ? "get" : "post"] = std::move(operation);
    }
    paths["/mcp"]["post"] = {
        {"operationId", "mcp"},
        {"summary", "MCP (JSON-RPC 2.0): initialize, ping, tools/list, tools/call"},
        {"description",
         "Every JSON registry entry is a tool; tools/call runs the same authorized, validated call "
         "as its REST route."},
        {"tags", nlohmann::json::array({"mcp"})},
        {"requestBody",
         {{"required", true},
          {"content", {{"application/json", {{"schema", {{"type", "object"}}}}}}}}},
        {"responses",
         {{"200", {{"description", "a JSON-RPC 2.0 response"}}},
          {"202", {{"description", "a notification was accepted"}}}}}};
    document["paths"] = std::move(paths);
    return document;
}

} // namespace lake::api
