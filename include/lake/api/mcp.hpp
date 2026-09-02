#pragma once
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

namespace lake::api {

inline constexpr std::string_view mcp_protocol_version = "2025-06-18";

// Dispatch one JSON-RPC 2.0 request (initialize, ping, tools/list, tools/call). Returns the
// response body, or nullopt for notifications. tools/call runs Registry::call with the given
// context, so authorization is exactly the REST path's.
std::optional<std::string> mcp_dispatch(const Registry& registry, const nlohmann::json& request,
                                        ApiContext& context, std::string_view version);

}  // namespace lake::api
