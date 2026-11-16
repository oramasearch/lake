// SPDX-License-Identifier: MIT

// tunnel_client_downloads (lake#745, docs/api/tunnel-clients.md): the tunnel client a developer
// installs for Developer Access, as a signed link per platform that a plain `curl` follows.

#include "lake/api/capabilities.hpp"

#include <chrono>
#include <format>
#include <string>

#include "lake/api/tunnel_clients.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

std::expected<std::string, ApiError> self_object(const json&, ApiContext& context) {
    return "principal/" + context.acting_identity;
}

// lake's browser-facing origin, in the order `list_packages` uses: the configured external origin,
// else the public host, else the origin this request came to. Empty outside HTTP, and the link is
// then the path alone.
std::string public_origin(const ApiContext& context) {
    if (!context.external_origin.empty()) {
        return context.external_origin;
    }
    if (!context.public_host.empty()) {
        return "https://" + context.public_host;
    }
    return context.request_origin;
}

json nullable_string() {
    return {{"type", json::array({"string", "null"})}};
}

json client_schema() {
    return {{"type", "object"},
            {"properties",
             {{"platform",
               {{"type", "string"}, {"enum", json::array({"darwin-arm64", "linux-amd64"})}}},
              {"os", {{"type", "string"}}},
              {"arch", {{"type", "string"}}},
              {"file_name", {{"type", "string"}}},
              {"sha256", {{"type", "string"}}},
              {"size", {{"type", "integer"}, {"minimum", 0}}},
              {"download_url", {{"type", "string"}}}}},
            {"required",
             json::array(
                 {"platform", "os", "arch", "file_name", "sha256", "size", "download_url"})},
            {"additionalProperties", false}};
}

ApiResult tunnel_client_downloads(const json&, ApiContext& context) {
    const json unavailable = {{"available", false},
                              {"version", nullptr},
                              {"expires_at", nullptr},
                              {"clients", json::array()}};
    // Without the instance key no link can be signed, so the clients are not offered.
    if (context.instance_kek.empty()) {
        return ApiResult::success(unavailable);
    }
    const auto found = read_tunnel_clients(context.tunnel_clients_dir);
    if (!found) {
        return ApiResult::success(unavailable);
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto expires = now + std::chrono::seconds{tunnel_client_link_seconds};
    const std::int64_t expires_unix = expires.time_since_epoch().count();
    const std::string origin = public_origin(context);
    json clients = json::array();
    for (const TunnelClient& client : found->clients) {
        const TunnelClientPlatform* platform = find_tunnel_client_platform(client.platform);
        const std::string signature = tunnel_client_signature(
            context.instance_kek, client.platform, found->version, expires_unix);
        if (platform == nullptr || signature.empty()) {
            continue;
        }
        clients.push_back(
            {{"platform", client.platform},
             {"os", std::string(platform->os)},
             {"arch", std::string(platform->arch)},
             {"file_name", client.file_name},
             {"sha256", client.sha256},
             {"size", client.size},
             {"download_url",
              origin + tunnel_client_link(client.platform, expires_unix, signature)}});
    }
    if (clients.empty()) {
        return ApiResult::success(unavailable);
    }
    context.audit_object = "tunnel-clients/" + found->version;
    return ApiResult::success({{"available", true},
                               {"version", found->version},
                               {"expires_at", std::format("{:%FT%TZ}", expires)},
                               {"clients", std::move(clients)}});
}

} // namespace

void register_tunnel_client_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "tunnel_client_downloads",
        .title = "Tunnel Client Downloads",
        .description =
            "The tunnel client (`zetta-tunnel`) a developer runs to open their Developer Access "
            "tunnel: its version and, per platform, its SHA-256, size and `download_url`, a "
            "signed link that works for 15 minutes without a session, so a plain `curl` can "
            "fetch it. `available` is false, with no clients, when this Lake has none "
            "(LAKE_TUNNEL_CLIENTS_DIR) or no instance key to sign links with.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"available", {{"type", "boolean"}}},
                            {"version", nullable_string()},
                            {"expires_at", nullable_string()},
                            {"clients", {{"type", "array"}, {"items", client_schema()}}}}},
                          {"required",
                           json::array({"available", "version", "expires_at", "clients"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        // Any signed-in person: the links are what every Developer Access recipient needs.
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .handler = tunnel_client_downloads});
}

} // namespace lake::api
