#include "lake/api/oidc_jwks.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace lake::api {
namespace {
using Json = nlohmann::json;

struct InvalidDocument {};
Json parse(std::string_view text) {
    if (text.size() > 2 * 1024 * 1024) {
        return Json();
    }
    return Json::parse(
        text,
        [](int depth, Json::parse_event_t, Json&) {
            if (depth > 16) {
                throw InvalidDocument{};
            }
            return true;
        },
        false);
}

std::string decode(std::string_view input) {
    if (input.empty() || input.size() % 4 == 1 || !std::ranges::all_of(input, [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_';
        })) {
        return {};
    }
    std::string padded(input);
    std::ranges::replace(padded, '-', '+');
    std::ranges::replace(padded, '_', '/');
    while (padded.size() % 4 != 0) {
        padded += '=';
    }
    std::string bytes(padded.size(), '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
                                     reinterpret_cast<const unsigned char*>(padded.data()),
                                     static_cast<int>(padded.size()));
    if (size < 0) {
        return {};
    }
    bytes.resize(size - (padded.size() - input.size()));
    return bytes;
}

// A token that cannot verify must not cause a key fetch. These checks authorize no identity;
// the signature and all claims are checked by verify_oidc_id_token after choosing the keys.
std::optional<std::string> token_key(std::string_view token,
                                     std::string_view issuer,
                                     std::string_view audience,
                                     std::string_view nonce,
                                     bool require_issued_at) {
    if (token.size() > 16384) {
        return std::nullopt;
    }
    const auto first = token.find('.');
    const auto second = first == token.npos ? token.npos : token.find('.', first + 1);
    if (second == token.npos || token.find('.', second + 1) != token.npos) {
        return std::nullopt;
    }
    const auto header = parse(decode(token.substr(0, first)));
    const auto claims = parse(decode(token.substr(first + 1, second - first - 1)));
    if (!header.is_object() || !claims.is_object() || decode(token.substr(second + 1)).empty()) {
        return std::nullopt;
    }
    if (header.value("alg", "") != "RS256" || header.value("kid", "").empty() ||
        claims.value("iss", "") != issuer || claims.value("sub", "").empty() ||
        claims.value("nonce", "") != nonce) {
        return std::nullopt;
    }
    const auto aud = claims.value("aud", Json());
    bool matches = aud.is_string() && aud.get<std::string>() == audience;
    if (aud.is_array()) {
        matches =
            std::ranges::all_of(aud, [](const Json& item) { return item.is_string(); }) &&
            std::ranges::any_of(aud,
                                [&](const Json& item) {
                                    return item.is_string() && item.get<std::string>() == audience;
                                }) &&
            (aud.size() <= 1 || claims.value("azp", "") == audience);
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (const auto* name : {"exp", "nbf", "iat"}) {
        if (claims.contains(name) &&
            (!claims[name].is_number_integer() ||
             (claims[name].is_number_unsigned() &&
              claims[name].get<std::uint64_t>() >
                  static_cast<std::uint64_t>(std::numeric_limits<long>::max())))) {
            return std::nullopt;
        }
    }
    if ((require_issued_at && claims.value("iat", 0LL) <= 0) || !matches ||
        claims.value("exp", 0LL) <= now || claims.value("nbf", 0LL) > now + 60 ||
        claims.value("iat", 0LL) > now + 60) {
        return std::nullopt;
    }
    for (const auto* name : {"email", "name"}) {
        if (claims.contains(name) && !claims[name].is_string()) {
            return std::nullopt;
        }
    }
    return header["kid"].get<std::string>();
}

bool has_key(const std::string& document, const std::string& kid) {
    const auto keys = parse(document);
    if (!keys.is_object() || !keys.contains("keys") || !keys["keys"].is_array()) {
        return false;
    }
    return std::ranges::any_of(keys["keys"], [&](const Json& key) {
        return key.is_object() && key.value("kid", Json()) == kid;
    });
}
} // namespace

std::optional<control::identity_crypto::OidcClaims> OidcJwks::verify(std::string_view token,
                                                                     const std::string& url,
                                                                     std::string_view issuer,
                                                                     std::string_view audience,
                                                                     std::string_view nonce,
                                                                     bool public_only,
                                                                     const Fetch& fetch,
                                                                     bool require_issued_at) {
    try {
        const auto kid = token_key(token, issuer, audience, nonce, require_issued_at);
        if (!kid) {
            return std::nullopt;
        }
        std::string document;
        {
            std::lock_guard lock(mutex_);
            auto& entry = entries_[{url, public_only}];
            const auto now = now_();
            const bool known = has_key(entry.document, *kid);
            // A known key with a bad signature or claims never warrants a refresh, even when old.
            if (known) {
                auto verified = control::identity_crypto::verify_oidc_id_token(
                    token, entry.document, issuer, audience, nonce);
                if (!verified || now - entry.fetched_at < std::chrono::hours(1)) {
                    return verified;
                }
            }
            if (entry.fetching ||
                (entry.attempted_at && now - *entry.attempted_at < std::chrono::seconds(60))) {
                return std::nullopt;
            }
            entry.attempted_at = now;
            entry.fetching = true;
        }
        // No cache lock is held across network I/O. One caller owns the fetch for this URL.
        std::optional<std::string> fetched;
        try {
            fetched = fetch(url, public_only);
        } catch (...) {
            // Release the flight even if an injected transport throws.
        }
        bool valid = false;
        try {
            if (fetched) {
                const auto parsed = parse(*fetched);
                valid = parsed.is_object() && parsed.contains("keys") && parsed["keys"].is_array();
            }
        } catch (const InvalidDocument&) {
        }
        {
            std::lock_guard lock(mutex_);
            auto& entry = entries_.at({url, public_only});
            entry.fetching = false;
            if (valid) {
                entry.document = std::move(*fetched);
                entry.fetched_at = now_();
                document = entry.document;
            }
        }
        return document.empty() ? std::nullopt
                                : control::identity_crypto::verify_oidc_id_token(
                                      token, document, issuer, audience, nonce);
    } catch (const InvalidDocument&) {
        return std::nullopt;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}
} // namespace lake::api
