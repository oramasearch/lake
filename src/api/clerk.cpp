#include "lake/api/clerk.hpp"

#include <algorithm>
#include <array>
#include <cctype>

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

} // namespace lake::api
