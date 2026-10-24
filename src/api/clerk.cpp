#include "lake/api/clerk.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>

namespace lake::api {
namespace {

std::optional<std::string> decode_base64(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    unsigned buffer = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=') {
            break;
        }
        const std::size_t value = alphabet.find(character);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }
        buffer = (buffer << 6U) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return output;
}

bool valid_host(std::string_view host) {
    return !host.empty() && host.size() <= 253 && host.find('.') != std::string_view::npos &&
           std::ranges::all_of(host, [](char character) {
               return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                      character == '.' || character == '-';
           });
}

} // namespace

std::optional<ClerkConfig> clerk_config(std::string_view publishable_key) {
    static constexpr std::array<std::string_view, 2> prefixes{"pk_test_", "pk_live_"};
    for (const std::string_view prefix : prefixes) {
        if (!publishable_key.starts_with(prefix)) {
            continue;
        }
        const auto decoded = decode_base64(publishable_key.substr(prefix.size()));
        if (!decoded || !decoded->ends_with('$')) {
            return std::nullopt;
        }
        const std::string host = decoded->substr(0, decoded->size() - 1);
        if (!valid_host(host)) {
            return std::nullopt;
        }
        const std::string issuer = "https://" + host;
        return ClerkConfig{
            std::string(publishable_key), host, issuer, issuer + "/.well-known/jwks.json"};
    }
    return std::nullopt;
}

bool lake_pool_password_sign_in(std::string_view clerk_publishable_key) {
    return !clerk_config(clerk_publishable_key).has_value();
}

std::expected<ClerkUserStatus, std::string>
clerk_user_status(const std::string& subject, const std::string& secret_key, const ClerkGet& get) {
    if (secret_key.empty() || !get) {
        return std::unexpected("Lake cannot check the Clerk account right now.");
    }
    // Clerk identifiers are opaque path components; refuse delimiters and non-ASCII bytes.
    if (subject.empty() || subject.size() > 256 ||
        !std::ranges::all_of(subject, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-';
        })) {
        return std::unexpected("The Clerk user identifier is invalid.");
    }
    ClerkResponse response;
    try {
        response = get("https://api.clerk.com/v1/users/" + subject,
                       {{"Authorization", "Bearer " + secret_key}, {"Accept", "application/json"}});
    } catch (...) {
        return std::unexpected("Lake cannot check the Clerk account right now.");
    }
    if (!response.error.empty() || response.status != 200) {
        return std::unexpected("Lake cannot check the Clerk account right now.");
    }
    const auto user = nlohmann::json::parse(response.body, nullptr, false);
    if (!user.is_object() || !user.contains("id") || user.at("id") != subject ||
        !user.contains("banned") || !user.at("banned").is_boolean() || !user.contains("locked") ||
        !user.at("locked").is_boolean() ||
        (user.contains("deprovisioned") && !user.at("deprovisioned").is_boolean()) ||
        !user.contains("updated_at") || !user.at("updated_at").is_number_integer() ||
        user.at("updated_at").get<std::int64_t>() < 0) {
        return std::unexpected("Clerk returned an incomplete account status.");
    }
    return ClerkUserStatus{user.at("updated_at").get<std::int64_t>(),
                           user.at("banned").get<bool>(),
                           user.at("locked").get<bool>(),
                           user.value("deprovisioned", false)};
}

} // namespace lake::api
