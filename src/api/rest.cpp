// SPDX-License-Identifier: MIT

#include "lake/api/rest.hpp"

#include <charconv>

#include "lake/api/brand.hpp"

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

// One query value of an array input: a JSON array as written, or else a comma list, each piece
// coerced by the declared `items` schema. A value that is neither stays one string item, and the
// schema's own validation says what is wrong with it.
std::vector<nlohmann::json> array_items(const nlohmann::json& property, const std::string& raw) {
    const nlohmann::json item_schema = property.value("items", nlohmann::json::object());
    std::vector<nlohmann::json> items;
    if (!raw.empty() && raw.front() == '[') {
        const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_array()) {
            for (const auto& item : parsed) {
                items.push_back(item);
            }
            return items;
        }
    }
    std::size_t start = 0;
    while (start <= raw.size()) {
        const std::size_t comma = raw.find(',', start);
        const std::size_t end = comma == std::string::npos ? raw.size() : comma;
        if (end > start) {
            items.push_back(coerce(item_schema, raw.substr(start, end - start)));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
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
        // `/v1/server_info?` and `?&limit=5` carry an empty key; it names no field (lake#450).
        if (key.empty()) {
            continue;
        }
        if (properties.contains(key) && properties[key].value("type", "") == "array") {
            // An array input (lake#576): repeated keys (`runs=a&runs=b`), a comma list
            // (`runs=a,b`) or a JSON array (`runs=["a","b"]`), in any mix; each item is coerced
            // by the declared `items` type. An empty value adds nothing.
            nlohmann::json& items = input[key];
            if (!items.is_array()) {
                items = nlohmann::json::array();
            }
            for (nlohmann::json& item : array_items(properties[key], value)) {
                items.push_back(std::move(item));
            }
            continue;
        }
        input[key] =
            properties.contains(key) ? coerce(properties[key], value) : nlohmann::json(value);
    }
    return input;
}

int rest_status(const ApiResult& result) {
    if (result.ok) {
        return 200;
    }
    // One table (docs/api/errors.md); ApiResult::failure only ever produces a code in it.
    const ErrorCode* code = find_error_code(result.error.code);
    return code == nullptr ? 500 : code->status;
}

nlohmann::json rest_envelope(const ApiResult& result) {
    return result.envelope();
}

nlohmann::json
openapi_document(const Registry& registry, std::string_view version, SchemaAudience audience) {
    nlohmann::json document;
    document["openapi"] = "3.1.0";
    document["info"] = {{"title", "Lake control surface"},
                        {"version", std::string(version)},
                        {"description",
                         "The capability registry rendered as HTTP routes: JSON reads are GET "
                         "/v1/<name> (input in the "
                         "query string), JSON actions are POST /v1/<name> (input as the body), and "
                         "liquid sync uses "
                         "repository-scoped binary routes. JSON capabilities are also served as "
                         "MCP tools on POST /mcp."}};
    document["servers"] = nlohmann::json::array({{{"url", "/"}}});
    document["components"] = {
        {"securitySchemes",
         {{"bearer_token",
           {{"type", "http"},
            {"scheme", "bearer"},
            {"description",
             "An account token from /tokens, limited by its scopes and your "
             "repository access. Public operations do not require a token. "
             "Operator credentials are reserved for instance administration."}}}}},
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
        // What the call does, apart from how it is sent: an action (POST) may only read, like
        // workspace_query or predict. Agents decide what they may call on their own from this.
        if (!capability->action.empty()) {
            operation["x-lake-action"] = capability->action;
        }
        if (!capability->requires_auth && !capability->requires_credential)
            operation["security"] = nlohmann::json::array();
        const std::string method = rest_method(*capability);
        const nlohmann::json input_schema = audience_input_schema(*capability, audience);
        const nlohmann::json properties =
            input_schema.value("properties", nlohmann::json::object());
        const nlohmann::json required = input_schema.value("required", nlohmann::json::array());
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
                if (it.value().value("type", "") == "array") {
                    // Repeated keys, `runs=a&runs=b` (lake#576); lake also takes a comma list.
                    parameter["style"] = "form";
                    parameter["explode"] = true;
                }
                parameters.push_back(std::move(parameter));
            }
            operation["parameters"] = std::move(parameters);
        } else {
            operation["requestBody"] = {
                {"required", !required.empty()},
                {"content", {{"application/json", {{"schema", input_schema}}}}}};
        }
        nlohmann::json responses;
        responses["200"] = {{"description", "the call succeeded"},
                            {"content",
                             {{"application/json",
                               {{"schema", success_envelope_schema(capability->output_schema)}}}}}};
        if (capability->requires_auth || capability->requires_credential) {
            responses["401"] = error_response(
                "no credential was accepted: `error.reason` is missing, invalid or expired and "
                "`error.help` links to the page that makes a new key (docs/api/errors.md)");
        }
        responses["4XX"] = error_response("the input, authorization, or addressed entity is at "
                                          "fault; the error carries the reason");
        responses["5XX"] = error_response("the control-plane store is unavailable or failed");
        operation["responses"] = std::move(responses);
        paths[rest_path(*capability)][method == "GET" ? "get" : "post"] = operation;
        if (method == "GET" && capability->post_alias) {
            paths[rest_path(*capability)]["get"]["operationId"] = capability->name + "_get";
            operation.erase("parameters");
            operation["requestBody"] = {
                {"required", true},
                {"content", {{"application/json", {{"schema", input_schema}}}}}};
            paths[rest_path(*capability)]["post"] = std::move(operation);
        }
    }
    paths["/mcp"]["post"] = {
        {"operationId", "mcp"},
        {"summary", "MCP (JSON-RPC 2.0): initialize, ping, tools/list, tools/call"},
        {"description",
         "Every JSON registry entry is a tool with the input and output schemas this document "
         "gives its REST route; tools/call runs the same authorized, validated call. initialize "
         "declares tools.listChanged and answers an Mcp-Session-Id; a later request whose session "
         "began on a different tool list is answered, when the client accepts text/event-stream, "
         "with notifications/tools/list_changed before its response."},
        {"tags", nlohmann::json::array({"mcp"})},
        {"requestBody",
         {{"required", true},
          {"content", {{"application/json", {{"schema", {{"type", "object"}}}}}}}}},
        {"responses",
         {{"200", {{"description", "a JSON-RPC 2.0 response"}}},
          {"202", {{"description", "a notification was accepted"}}}}}};
    // The two drive download routes stream a file's bytes rather than answering JSON, so they are
    // not capabilities; they are described here so a client finds them (lake#587). Each runs the
    // capability named in its description as the caller, which authorizes and audits the read.
    const auto download = [](std::string id,
                             std::string summary,
                             std::string description,
                             nlohmann::json parameters) {
        nlohmann::json operation{{"operationId", std::move(id)},
                                 {"summary", std::move(summary)},
                                 {"description", std::move(description)},
                                 {"tags", nlohmann::json::array({"download"})},
                                 {"parameters", nlohmann::json::array()}};
        for (const auto& [name, required] : parameters.items()) {
            operation["parameters"].push_back({{"name", name},
                                               {"in", "query"},
                                               {"required", required.get<bool>()},
                                               {"schema", {{"type", "string"}}}});
        }
        operation["responses"] = {
            {"200",
             {{"description",
               "the file's bytes, with Content-Disposition naming the file and an ETag of its "
               "digest"},
              {"content",
               {{"application/octet-stream",
                 {{"schema", {{"type", "string"}, {"format", "binary"}}}}}}}}},
            {"302", {{"description", "no credential: redirected to /sign-in"}}},
            {"4XX", error_response("not found, or not allowed to read the file")}};
        return operation;
    };
    paths["/drive/download"]["get"] =
        download("drive_download",
                 "Download a repository-drive file",
                 "Streams one file of a repository drive as an attachment, authorized and audited "
                 "by download_drive_file (`ref` defaults to main).",
                 {{"repository_id", true}, {"ref", false}, {"path", true}});
    paths["/drive/files/download"]["get"] =
        download("drive_files_download",
                 "Download a workspace file",
                 "Streams one file of a workspace-files drive, 1 MiB at a time, after "
                 "get_drive_entry authorizes and audits the read. `inline=1` shows images, PDF, "
                 "text, CSV, Markdown, audio and video in the browser; anything else is an "
                 "attachment.",
                 {{"customer_id", true},
                  {"workspace_id", true},
                  {"drive_id", true},
                  {"id", true},
                  {"inline", false}});
    // A tunnel client (lake#745): the signed link tunnel_client_downloads answers is the
    // credential, so it needs no session.
    nlohmann::json tunnel_client{
        {"operationId", "tunnel_client_download"},
        {"summary", "Download a tunnel client"},
        {"description",
         "Streams one tunnel client binary for a link tunnel_client_downloads signed. The link "
         "works for 15 minutes and for the clients' version it was made for; no credential "
         "besides it is needed."},
        {"tags", nlohmann::json::array({"download"})},
        {"security", nlohmann::json::array()},
        {"parameters",
         nlohmann::json::array(
             {{{"name", "platform"},
               {"in", "path"},
               {"required", true},
               {"schema",
                {{"type", "string"}, {"enum", nlohmann::json::array({"darwin-arm64", "linux-amd64"})}}}},
              {{"name", "expires"}, {"in", "query"}, {"required", true}, {"schema", {{"type", "string"}}}},
              {{"name", "signature"},
               {"in", "query"},
               {"required", true},
               {"schema", {{"type", "string"}}}}})},
        {"responses",
         {{"200",
           {{"description",
             "the binary, with Content-Disposition naming the file and an ETag of its SHA-256"},
            {"content",
             {{"application/octet-stream",
               {{"schema", {{"type", "string"}, {"format", "binary"}}}}}}}}},
          {"401", error_response("the link is not valid or has expired (`reason`)")},
          {"404", error_response("no such platform, or this Lake has no tunnel clients")}}}};
    paths["/tunnel-clients/{platform}"]["get"] = std::move(tunnel_client);
    document["paths"] = std::move(paths);
    // A white-labelled Lake's document speaks its brand's names (lake#1137); unchanged for Lake's.
    instance_brand().apply_to_documentation(document);
    return document;
}

} // namespace lake::api
