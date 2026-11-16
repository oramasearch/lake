// SPDX-License-Identifier: MIT

#pragma once
// The terms of service and privacy policy that Lake's sign-up and sign-in pages link to
// (docs/serving/configuration.md). Instance configuration: LAKE_TERMS_URL and LAKE_PRIVACY_URL,
// with no default: an install that sets neither shows no legal links.

#include <expected>
#include <string>
#include <string_view>

namespace lake::api {

inline constexpr std::string_view default_terms_url = "";
inline constexpr std::string_view default_privacy_url = "";

// One link setting as read from the environment (`value` is getenv's answer, null when unset):
// unset or empty keeps `fallback`; `off` removes the link (an empty URL, so no link is shown);
// anything else must be an absolute http or https URL of at most 2048 characters with no space,
// control character, quote or angle bracket, else the answer is the reason, naming the variable.
inline std::expected<std::string, std::string> legal_url_setting(std::string_view name, const char* value,
                                                                 std::string_view fallback) {
    if (value == nullptr || *value == '\0') {
        return std::string(fallback);
    }
    const std::string_view url(value);
    if (url == "off") {
        return std::string();
    }
    const auto refuse = [&](std::string_view why) {
        return std::unexpected(std::string(name) + " " + std::string(why) +
                               " (an absolute https:// URL, or off for no link)");
    };
    if (url.size() > 2048) {
        return refuse("is longer than 2048 characters");
    }
    const bool https = url.starts_with("https://");
    const bool http = url.starts_with("http://");
    if (!https && !http) {
        return refuse("must start with https:// or http://");
    }
    if (url.size() == (https ? 8U : 7U)) {
        return refuse("has no host");
    }
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' ||
            c == '\\') {
            return refuse("contains a character a link may not carry");
        }
    }
    return std::string(url);
}

} // namespace lake::api
