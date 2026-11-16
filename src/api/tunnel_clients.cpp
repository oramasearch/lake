// SPDX-License-Identifier: MIT

// The tunnel client downloads (lake#745, docs/api/tunnel-clients.md): what the clients directory
// holds, and the signed links that hand one out.

#include "lake/api/tunnel_clients.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/control/identity_crypto.hpp"

namespace lake::api {
namespace {

constexpr std::array platforms = std::to_array<TunnelClientPlatform>({
    {"darwin-arm64", "darwin", "arm64", "zetta-tunnel-darwin-arm64"},
    {"linux-amd64", "linux", "amd64", "zetta-tunnel-linux-amd64"},
});

// At most this much of VERSION or SHA256SUMS is read: both are a few lines.
constexpr std::uintmax_t max_listing_bytes = 64 * 1024;

std::optional<std::string> read_small_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > max_listing_bytes) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool sha256_hex(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}

bool version_text(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '-' || c == '+' || c == '_';
    });
}

// `<sha256>  <file>` lines, as sha256sum and shasum write them (`*<file>` in binary mode).
std::map<std::string, std::string> parse_sums(std::string_view text) {
    std::map<std::string, std::string> sums;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string line = trimmed(text.substr(start, end - start));
        start = end + 1;
        const auto space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        std::string digest = line.substr(0, space);
        std::ranges::transform(digest, digest.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::string name = trimmed(std::string_view(line).substr(space + 1));
        if (name.starts_with('*')) {
            name.erase(0, 1);
        }
        if (sha256_hex(digest) && !name.empty()) {
            sums.emplace(std::move(name), std::move(digest));
        }
    }
    return sums;
}

} // namespace

std::string signed_link_signature(std::string_view key, std::string_view payload) {
    if (key.empty()) {
        return {};
    }
    // The instance key never signs a link itself (LSR-266, lake#1009): a key for this one purpose
    // is derived from it first, so a link signature says nothing about the key that seals secrets.
    constexpr std::string_view purpose = "lake signed link v1";
    std::array<unsigned char, EVP_MAX_MD_SIZE> link_key{};
    unsigned int link_key_size = 0;
    if (HMAC(EVP_sha256(),
             key.data(),
             static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(purpose.data()),
             purpose.size(),
             link_key.data(),
             &link_key_size) == nullptr) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (HMAC(EVP_sha256(),
             link_key.data(),
             static_cast<int>(link_key_size),
             reinterpret_cast<const unsigned char*>(payload.data()),
             payload.size(),
             digest.data(),
             &digest_size) == nullptr) {
        return {};
    }
    return control::identity_crypto::base64url_encode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), digest_size));
}

bool signed_link_matches(std::string_view key,
                         std::string_view payload,
                         std::string_view signature) {
    const std::string expected = signed_link_signature(key, payload);
    return !expected.empty() && expected.size() == signature.size() &&
           CRYPTO_memcmp(expected.data(), signature.data(), expected.size()) == 0;
}

std::span<const TunnelClientPlatform> tunnel_client_platforms() {
    return platforms;
}

const TunnelClientPlatform* find_tunnel_client_platform(std::string_view platform) {
    const auto found = std::ranges::find(platforms, platform, &TunnelClientPlatform::platform);
    return found == platforms.end() ? nullptr : &*found;
}

std::optional<TunnelClients> read_tunnel_clients(const std::filesystem::path& directory) {
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error)) {
        return std::nullopt;
    }
    const auto version = read_small_file(directory / "VERSION");
    const auto sums_text = read_small_file(directory / "SHA256SUMS");
    if (!version || !sums_text || !version_text(trimmed(*version))) {
        return std::nullopt;
    }
    const auto sums = parse_sums(*sums_text);
    TunnelClients clients{.version = trimmed(*version), .clients = {}};
    for (const TunnelClientPlatform& platform : platforms) {
        const std::string name(platform.file_name);
        const auto sum = sums.find(name);
        const std::filesystem::path path = directory / name;
        if (sum == sums.end() || !std::filesystem::is_regular_file(path, error)) {
            continue;
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0) {
            continue;
        }
        clients.clients.push_back({.platform = std::string(platform.platform),
                                   .file_name = name,
                                   .sha256 = sum->second,
                                   .size = static_cast<std::uint64_t>(size),
                                   .path = path});
    }
    if (clients.clients.empty()) {
        return std::nullopt;
    }
    return clients;
}

std::string tunnel_client_link_payload(std::string_view platform,
                                       std::string_view version,
                                       std::int64_t expires) {
    // The first line names what is signed, so this signature can never stand for another link's.
    return "lake-tunnel-client\n" + std::string(platform) + "\n" + std::string(version) + "\n" +
           std::to_string(expires);
}

std::string tunnel_client_signature(std::string_view key,
                                    std::string_view platform,
                                    std::string_view version,
                                    std::int64_t expires) {
    return signed_link_signature(key, tunnel_client_link_payload(platform, version, expires));
}

std::string tunnel_client_link(std::string_view platform,
                               std::int64_t expires,
                               std::string_view signature) {
    // A platform is allow-listed and a base64url signature needs no escaping.
    return "/tunnel-clients/" + std::string(platform) + "?expires=" + std::to_string(expires) +
           "&signature=" + std::string(signature);
}

TunnelClientLinkCheck check_tunnel_client_link(std::string_view key,
                                               std::string_view platform,
                                               std::string_view version,
                                               std::string_view expires,
                                               std::string_view signature,
                                               std::int64_t now) {
    std::int64_t at = 0;
    const auto [end, error] = std::from_chars(expires.data(), expires.data() + expires.size(), at);
    if (expires.empty() || error != std::errc{} || end != expires.data() + expires.size() ||
        signature.empty()) {
        return TunnelClientLinkCheck::invalid;
    }
    if (!signed_link_matches(key, tunnel_client_link_payload(platform, version, at), signature)) {
        return TunnelClientLinkCheck::invalid;
    }
    return at < now ? TunnelClientLinkCheck::expired : TunnelClientLinkCheck::valid;
}

} // namespace lake::api
