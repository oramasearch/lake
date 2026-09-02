#include "lake/api/mcp.hpp"

#include "lake/api/rest.hpp"

namespace lake::api {
namespace {

std::string jsonrpc_error(const nlohmann::json& id, int code, const std::string& message) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = {{"code", code}, {"message", message}};
    return response.dump();
}

std::string jsonrpc_result(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response.dump();
}

}  // namespace

std::optional<std::string> mcp_dispatch(const Registry& registry, const nlohmann::json& request,
                                        ApiContext& context, std::string_view version) {
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0") {
        return jsonrpc_error(nullptr, -32600, "invalid JSON-RPC 2.0 request");
    }
    const nlohmann::json id = request.value("id", nlohmann::json());
    const std::string method = request.value("method", "");

    if (method == "notifications/initialized") return std::nullopt;
    if (method == "initialize") {
        return jsonrpc_result(id, {{"protocolVersion", std::string(mcp_protocol_version)},
                                   {"capabilities", {{"tools", nlohmann::json::object()}}},
                                   {"serverInfo", {{"name", "lake"}, {"version", std::string(version)}}}});
    }
    if (method == "ping") return jsonrpc_result(id, nlohmann::json::object());
    if (method == "tools/list") {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& capability : registry.list()) {
            nlohmann::json tool = {{"name", capability->name},
                                   {"title", capability->title},
                                   {"description", capability->description},
                                   {"inputSchema", capability->input_schema}};
            if (capability->output_schema.is_object() && !capability->output_schema.empty()) {
                tool["outputSchema"] = capability->output_schema;
            }
            tools.push_back(std::move(tool));
        }
        return jsonrpc_result(id, {{"tools", std::move(tools)}});
    }
    if (method == "tools/call") {
        const nlohmann::json params = request.value("params", nlohmann::json::object());
        const std::string tool_name = params.value("name", "");
        const nlohmann::json arguments = params.value("arguments", nlohmann::json::object());
        if (tool_name.empty()) return jsonrpc_error(id, -32602, "params.name is required");
        const ApiResult call_result = registry.call(tool_name, arguments, context);
        nlohmann::json payload = rest_envelope(call_result);
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", payload.dump()}}});
        result["structuredContent"] = std::move(payload);
        result["isError"] = !call_result.ok;
        return jsonrpc_result(id, std::move(result));
    }
    return jsonrpc_error(id, -32601, "method not found: " + method);
}

}  // namespace lake::api
