#pragma once
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

// The REST rendering of the registry: route table, query coercion, status mapping and the
// OpenAPI document, all derived from the entries. HTTP-free; the Beast2 adapter is its only
// wire-facing consumer.
namespace lake::api {

std::string rest_path(const Capability& capability);    // "/v1/<name>"
std::string rest_method(const Capability& capability);  // "GET" for reads, "POST" for actions

// Query parameters → input document, each value coerced by its declared type; undeclared keys
// stay strings so validation rejects them with the schema's own reason.
nlohmann::json rest_input_from_query(const Capability& capability,
                                     const std::vector<std::pair<std::string, std::string>>& query);

// 200 on ok; invalid_input 400, unauthorized 403, not_found / unknown_capability 404, conflict
// 409, store_unavailable 503, anything else 500.
int rest_status(const ApiResult& result);

// { ok, value | error{code,message,path}, duration_ms } — the same envelope MCP carries as
// structuredContent.
nlohmann::json rest_envelope(const ApiResult& result);

nlohmann::json openapi_document(const Registry& registry, std::string_view version);

}  // namespace lake::api
