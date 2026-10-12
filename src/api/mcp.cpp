#include "lake/api/mcp.hpp"

#include "lake/api/mcp_connections.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace lake::api {
namespace {

std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        out.push_back(digits[data[index] >> 4]);
        out.push_back(digits[data[index] & 0x0f]);
    }
    return out;
}

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

} // namespace

nlohmann::json mcp_tools(const Registry& registry, SchemaAudience audience) {
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& capability : registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
        nlohmann::json tool = {{"name", capability->name},
                               {"title", capability->title},
                               {"description", capability->description},
                               {"inputSchema", audience_input_schema(*capability, audience)}};
        // lake#594: only a delete or destroy is destructive, so a client warns for those alone.
        const bool writes = capability_writes(*capability);
        tool["annotations"] = {{"readOnlyHint", !writes},
                               {"destructiveHint", capability_destroys(*capability)},
                               {"idempotentHint", capability_idempotent(*capability)}};
        if (capability->output_schema.is_object() && !capability->output_schema.empty()) {
            tool["outputSchema"] = success_envelope_schema(capability->output_schema);
        }
        tools.push_back(std::move(tool));
    }
    return tools;
}

std::string mcp_tool_list_digest(const Registry& registry) {
    const std::string canonical = nlohmann::json{
        {"caller", mcp_tools(registry, SchemaAudience::Caller)},
        {"agent_task", mcp_tools(registry, SchemaAudience::AgentTask)}}.dump();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(canonical.data(),
                   canonical.size(),
                   digest.data(),
                   &size,
                   EVP_sha256(),
                   nullptr) != 1) {
        throw std::runtime_error("could not hash the MCP tool list");
    }
    return hex(digest.data(), size);
}

std::string mcp_session_id(std::string_view tool_list_digest) {
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        throw std::runtime_error("could not draw an MCP session identifier");
    }
    return std::string(tool_list_digest) + "." + hex(random.data(), random.size());
}

bool mcp_session_tool_list_changed(std::string_view session_id,
                                   std::string_view tool_list_digest) {
    if (session_id.empty()) {
        return false;
    }
    return !(session_id.size() > tool_list_digest.size() &&
             session_id.starts_with(tool_list_digest) &&
             session_id[tool_list_digest.size()] == '.');
}

std::string mcp_workspace_tool_list_digest(std::string_view lake_digest,
                                           std::vector<McpExportedDigest> exported) {
    std::erase_if(exported, [](const McpExportedDigest& entry) { return entry.digest.empty(); });
    if (exported.empty()) {
        return std::string(lake_digest);
    }
    std::ranges::sort(exported, {}, &McpExportedDigest::connection_id);
    std::string canonical(lake_digest);
    for (const McpExportedDigest& entry : exported) {
        canonical += "\n" + entry.connection_id + " " + entry.digest;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(canonical.data(),
                   canonical.size(),
                   digest.data(),
                   &size,
                   EVP_sha256(),
                   nullptr) != 1) {
        throw std::runtime_error("could not hash the MCP tool list");
    }
    return hex(digest.data(), size);
}

void McpSessionLists::listed(const std::string& session_id, const std::string& digest) {
    if (session_id.empty()) {
        return;
    }
    std::lock_guard lock(mutex_);
    const auto [entry, inserted] = listed_.insert_or_assign(session_id, digest);
    if (inserted) {
        order_.push_back(session_id);
    }
    while (order_.size() > capacity_) {
        listed_.erase(order_.front());
        order_.pop_front();
    }
}

bool McpSessionLists::changed(const std::string& session_id, std::string_view digest) const {
    {
        std::lock_guard lock(mutex_);
        const auto found = listed_.find(session_id);
        if (found != listed_.end()) {
            return found->second != digest;
        }
    }
    return mcp_session_tool_list_changed(session_id, digest);
}

nlohmann::json mcp_tool_list_changed_notification() {
    return {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}};
}

std::optional<std::string> mcp_dispatch(const Registry& registry,
                                        const nlohmann::json& request,
                                        ApiContext& context,
                                        std::string_view version) {
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0") {
        return jsonrpc_error(nullptr, -32600, "invalid JSON-RPC 2.0 request");
    }
    const nlohmann::json id = request.value("id", nlohmann::json());
    const std::string method = request.value("method", "");

    if (method == "notifications/initialized")
        return std::nullopt;
    if (method == "initialize") {
        return jsonrpc_result(
            id,
            {{"protocolVersion", std::string(mcp_protocol_version)},
             {"capabilities", {{"tools", {{"listChanged", true}}}}},
             {"serverInfo", {{"name", "lake"}, {"version", std::string(version)}}}});
    }
    if (method == "ping")
        return jsonrpc_result(id, nlohmann::json::object());
    if (method == "tools/list") {
        return jsonrpc_result(id, {{"tools", mcp_tools(registry, schema_audience(context))}});
    }
    if (method == "tools/call") {
        const nlohmann::json params = request.value("params", nlohmann::json::object());
        const std::string tool_name = params.value("name", "");
        const nlohmann::json arguments = params.value("arguments", nlohmann::json::object());
        if (tool_name.empty())
            return jsonrpc_error(id, -32602, "params.name is required");
        const ApiResult call_result = registry.call(tool_name, arguments, context);
        nlohmann::json payload = call_result.envelope();
        nlohmann::json result;
        result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", payload.dump()}}});
        result["structuredContent"] = std::move(payload);
        result["isError"] = !call_result.ok;
        return jsonrpc_result(id, std::move(result));
    }
    return jsonrpc_error(id, -32601, "method not found: " + method);
}

liquid::runtime::task<std::optional<std::string>> mcp_dispatch_async(const Registry& registry,
                                                                     const nlohmann::json& request,
                                                                     ApiContext& context,
                                                                     std::string_view version) {
    const std::string method = request.is_object() && request.value("jsonrpc", "") == "2.0"
                                   ? request.value("method", "")
                                   : "";
    // A workspace's MCP endpoint (/w/<workspace>/mcp) also lists the allowed tools of the
    // connections its people chose to expose, as `<prefix>__<tool>` (decision 60, lake#652).
    const std::string& workspace = context.path_workspace_id;
    if (method == "tools/list" && !workspace.empty() && context.authorized) {
        nlohmann::json tools = mcp_tools(registry, schema_audience(context));
        auto exported = co_await exported_mcp_tools(registry, context, workspace);
        if (exported) {
            for (nlohmann::json& tool : *exported) {
                tools.push_back(std::move(tool));
            }
        }
        co_return jsonrpc_result(request.value("id", nlohmann::json()),
                                 {{"tools", std::move(tools)}});
    }
    if (method != "tools/call") {
        co_return mcp_dispatch(registry, request, context, version);
    }
    const nlohmann::json id = request.value("id", nlohmann::json());
    const nlohmann::json params = request.value("params", nlohmann::json::object());
    const std::string tool_name = params.value("name", "");
    const nlohmann::json arguments = params.value("arguments", nlohmann::json::object());
    if (tool_name.empty()) {
        co_return jsonrpc_error(id, -32602, "params.name is required");
    }
    if (is_mcp_exported_tool_name(tool_name)) {
        // A connected tool answers as its server does, through read_mcp_tool or call_mcp_tool.
        auto answered =
            co_await call_exported_mcp_tool(registry, context, workspace, tool_name, arguments);
        co_return answered ? jsonrpc_result(id, std::move(*answered))
                           : jsonrpc_error(id, -32603, answered.error().message);
    }
    auto completed = co_await registry.call_async(tool_name, arguments, context);
    const ApiResult call_result =
        completed ? std::move(*completed)
                  : ApiResult::failure("store_error", completed.error().message);
    nlohmann::json payload = call_result.envelope();
    nlohmann::json result;
    result["content"] = nlohmann::json::array({{{"type", "text"}, {"text", payload.dump()}}});
    result["structuredContent"] = std::move(payload);
    result["isError"] = !call_result.ok;
    co_return jsonrpc_result(id, std::move(result));
}

} // namespace lake::api
