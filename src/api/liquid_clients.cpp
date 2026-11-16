// SPDX-License-Identifier: MIT

#include "lake/api/liquid_clients.hpp"

#include <charconv>
#include <format>

#include "liquid/version.hpp"

namespace lake::api {

namespace {

std::optional<unsigned> number(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string bounded(std::string_view text) {
    constexpr std::size_t limit = 40;
    std::string out;
    for (const char c : text.substr(0, limit)) {
        out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    if (text.size() > limit) {
        out += "...";
    }
    return out;
}

} // namespace

std::optional<LiquidRelease> parse_liquid_release(std::string_view text) {
    // A leading "v" is how tags are written (v0.2.0); accept it.
    if (text.starts_with('v')) {
        text.remove_prefix(1);
    }
    if (const auto suffix = text.find_first_of("-+"); suffix != std::string_view::npos) {
        if (suffix + 1 == text.size()) {
            return std::nullopt;
        }
        text = text.substr(0, suffix);
    }
    const auto first = text.find('.');
    const auto second = first == std::string_view::npos ? first : text.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = number(text.substr(0, first));
    const auto minor = number(text.substr(first + 1, second - first - 1));
    const auto patch = number(text.substr(second + 1));
    if (!major || !minor || !patch) {
        return std::nullopt;
    }
    return LiquidRelease{*major, *minor, *patch};
}

std::string to_string(const LiquidRelease& release) {
    return std::format("{}.{}.{}", release.major, release.minor, release.patch);
}

LiquidRelease newest_supported_liquid() {
    constexpr auto linked = liquid::current_version();
    return LiquidRelease{linked.major, linked.minor, linked.patch};
}

std::string liquid_client_refusal(std::string_view liquid_version) {
    const auto oldest = parse_liquid_release(oldest_supported_liquid);
    const LiquidRelease newest = newest_supported_liquid();
    const std::string range = std::format("liquid {} to {}", to_string(*oldest), to_string(newest));
    const auto client = parse_liquid_release(liquid_version);
    if (!client) {
        return std::format("{} '{}' is not a liquid release (MAJOR.MINOR.PATCH); this Lake "
                           "supports {}",
                           liquid_version_header,
                           bounded(liquid_version),
                           range);
    }
    if (*client < *oldest) {
        return std::format("liquid {} is older than this Lake supports ({}); upgrade liquid",
                           to_string(*client),
                           range);
    }
    if (*client > newest) {
        return std::format("liquid {} is newer than this Lake supports ({}); use liquid {} or "
                           "older, or ask the Lake's administrator to upgrade it",
                           to_string(*client),
                           range,
                           to_string(newest));
    }
    return {};
}

nlohmann::json liquid_clients_json() {
    return {{"sync_protocol", liquid_sync_protocol_version},
            {"sync_media_type", std::string(liquid_sync_media_type)},
            {"forge_api", liquid_forge_api_version},
            {"oldest_release", std::string(oldest_supported_liquid)},
            {"newest_release", to_string(newest_supported_liquid())},
            {"version_header", std::string(liquid_version_header)}};
}

bool liquid_version_exempt(std::string_view path) {
    // A client reads the range here first, so it is never refused for asking; the probes are
    // never a client's.
    return path == "/v1/server_info" || path == "/healthz" || path == "/readyz";
}

} // namespace lake::api
