// SPDX-License-Identifier: MIT

#pragma once

// The per-request JSON line lake writes to stdout (docs/serving/telemetry.md#request-logs).
//
// lake#384: request logs are instance operations data, so they carry no customer
// content. A line names the route pattern, never the raw path: a path can carry a repository,
// package, image or file name. The identifiers the route captured (`:repository_id`,
// `:workspace_id`, `:customer_id`, …) are logged as named fields when they are UUIDs; any other
// captured value (`:package`, `:repository` of an OCI name, `:digest`, `:filename`, `:token`) is
// left out. This header has no Boost types, so its tests need no server.

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::api {

struct RequestLogEntry {
    std::string ts;
    std::string method;
    std::string route; // the route pattern
    int status = 0;
    double duration_ms = 0;
    std::string node;
    std::string request_id;
    std::string capability; // when the request is a capability call
    // Captured identifiers by name, already filtered by request_log_ids.
    std::map<std::string, std::string> ids;
};

inline bool request_log_uuid(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? value[index] != '-'
                 : !std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

// A request id as a caller may send it (X-Request-Id): a short token. Anything else is not logged.
inline bool request_log_token(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
                      c == '.' || c == ':';
           });
}

// The captured route parameters worth logging: `*_id` names with UUID values.
template <class Params>
std::map<std::string, std::string> request_log_ids(const Params& params) {
    std::map<std::string, std::string> ids;
    for (const auto& [key, value] : params) {
        const std::string name(key);
        if (name.size() > 3 && name.ends_with("_id") && request_log_uuid(value)) {
            ids.emplace(name, std::string(value));
        }
    }
    return ids;
}

// The method a request is counted and logged under (lake#1315): its standard name, or `other`
// for any method HTTP does not define. A client may send any token as the method, and each new
// label value is a new metric series kept for the life of the process, so the client's text is
// never a label or a log field. Methods are case-sensitive (RFC 9110 section 9.1): `get` is
// `other`.
inline std::string_view request_method_label(std::string_view method) {
    static constexpr std::string_view standard[] = {
        "GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH"};
    for (const std::string_view name : standard) {
        if (method == name) {
            return name;
        }
    }
    return "other";
}

// Whether a route pattern is a capability call's: REST `/v1/<capability>` or MCP.
inline bool request_log_capability_route(std::string_view route) {
    return route.starts_with("/v1/") || route.starts_with("/w/:workspace_id/v1/") ||
           route == "/mcp" || route == "/w/:workspace_id/mcp";
}

inline std::string request_log_line(const RequestLogEntry& entry) {
    nlohmann::json line{{"ts", entry.ts},
                        {"method", entry.method},
                        {"route", entry.route},
                        {"status", entry.status},
                        {"duration_ms", entry.duration_ms},
                        {"node", entry.node}};
    if (request_log_token(entry.request_id)) {
        line["request_id"] = entry.request_id;
    }
    if (!entry.capability.empty()) {
        line["capability"] = entry.capability;
    }
    for (const auto& [name, value] : entry.ids) {
        if (!line.contains(name)) {
            line[name] = value;
        }
    }
    return line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace lake::api
