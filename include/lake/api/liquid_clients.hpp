// SPDX-License-Identifier: MIT

#pragma once

// What a liquid client (the liquid CLI, liquify) can check before it syncs with this Lake
// (lake#1324, docs/api/liquid-clients.md): the sync protocol and forge API versions Lake serves,
// and the oldest and newest liquid release it supports. `server_info` publishes them under
// `liquid`; a request whose `Liquid-Version` header names a release outside the range is refused
// with `unsupported_client_version` before it reaches a route.

#include <compare>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lake::api {

// The sync protocol: `application/vnd.liquid.sync-v1` bodies whose header frame carries `v: 1`
// (docs/api/sync.md).
inline constexpr unsigned liquid_sync_protocol_version = 1;
inline constexpr std::string_view liquid_sync_media_type = "application/vnd.liquid.sync-v1";

// The forge API: the repository, review, comment, issue and release capabilities under `/v1`
// that liquid's forge client calls (docs/api/forge.md).
inline constexpr unsigned liquid_forge_api_version = 1;

// The oldest liquid release this Lake syncs with. liquid v0.1.0 is the first release, and every
// release since speaks sync protocol 1.
inline constexpr std::string_view oldest_supported_liquid = "0.1.0";

// The header a liquid client sends its own release in (`Liquid-Version: 0.2.0`). Header names
// compare case-insensitively.
inline constexpr std::string_view liquid_version_header = "Liquid-Version";

// A liquid release, MAJOR.MINOR.PATCH. A pre-release or build suffix (`-rc.1`, `+abc`) is
// accepted and ignored.
struct LiquidRelease {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;

    auto operator<=>(const LiquidRelease&) const = default;
};

std::optional<LiquidRelease> parse_liquid_release(std::string_view text);
std::string to_string(const LiquidRelease& release);

// The newest liquid release this Lake supports: the libliquid it was built against.
LiquidRelease newest_supported_liquid();

// Empty when a request with this `Liquid-Version` value is served; otherwise the message of the
// `unsupported_client_version` refusal, which names the client's release, the supported range and
// the side to upgrade.
std::string liquid_client_refusal(std::string_view liquid_version);

// The `liquid` object of `server_info`.
nlohmann::json liquid_clients_json();

// Whether the path is one a client must always be able to read to learn the range.
bool liquid_version_exempt(std::string_view path);

} // namespace lake::api
