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

// A claim that is absent, null or a string. JWT issuers render an unset value as null.
bool text_or_absent(const Json& object, const char* name) {
    return !object.contains(name) || object[name].is_null() || object[name].is_string();
}

// A string claim's value; absent, null or another type reads as empty.
std::string text(const Json& object, const char* name) {
    return object.contains(name) && object[name].is_string() ? object[name].get<std::string>()
                                                             : std::string();
}

// A token that cannot verify must not cause a key fetch. These checks authorize no identity;
// the signature and all claims are checked by verify_oidc_id_token after choosing the keys.
// `reason` names the first check that failed, in words safe to log (no claim values but the
// issuer and audience Lake expected).
std::optional<std::string> token_key(std::string_view token,
                                     std::string_view issuer,
                                     std::string_view audience,
                                     std::string_view nonce,
                                     bool require_issued_at,
                                     std::string& reason) {
    const auto refuse = [&](std::string why) -> std::optional<std::string> {
        reason = std::move(why);
        return std::nullopt;
    };
    if (token.size() > 16384) {
        return refuse("token longer than 16384 bytes");
    }
    const auto first = token.find('.');
    const auto second = first == token.npos ? token.npos : token.find('.', first + 1);
    if (second == token.npos || token.find('.', second + 1) != token.npos) {
        return refuse("not a three-part JWT");
    }
    const auto header = parse(decode(token.substr(0, first)));
    const auto claims = parse(decode(token.substr(first + 1, second - first - 1)));
    if (!header.is_object() || !claims.is_object() || decode(token.substr(second + 1)).empty()) {
        return refuse("header, claims or signature do not decode");
    }
    if (!text_or_absent(header, "alg") || !text_or_absent(header, "kid") ||
        !text_or_absent(claims, "iss") || !text_or_absent(claims, "sub") ||
        !text_or_absent(claims, "nonce") || !text_or_absent(claims, "azp")) {
        return refuse("alg, kid, iss, sub, nonce or azp is not a string");
    }
    if (text(header, "alg") != "RS256") {
        return refuse("alg is not RS256");
    }
    if (text(header, "kid").empty()) {
        return refuse("no kid");
    }
    if (text(claims, "iss") != issuer) {
        return refuse("iss is not " + std::string(issuer));
    }
    if (text(claims, "sub").empty()) {
        return refuse("no sub");
    }
    if (text(claims, "nonce") != nonce) {
        return refuse("nonce does not match");
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
            (aud.size() <= 1 || text(claims, "azp") == audience);
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
            return refuse(std::string(name) + " is not an integer");
        }
    }
    if (!matches) {
        return refuse("aud does not name " + std::string(audience));
    }
    if (require_issued_at && claims.value("iat", 0LL) <= 0) {
        return refuse("no iat");
    }
    if (claims.value("exp", 0LL) <= now) {
        return refuse("expired (exp " + std::to_string(claims.value("exp", 0LL)) + ", now " +
                      std::to_string(now) + ")");
    }
    if (claims.value("nbf", 0LL) > now + 60 || claims.value("iat", 0LL) > now + 60) {
        return refuse("nbf or iat more than 60 s in the future (now " + std::to_string(now) + ")");
    }
    // Clerk renders a template claim whose value is unset as null: `{{user.full_name}}` for a
    // person without a name (lake#1115). Null means absent; any other non-string is refused.
    for (const auto* name : {"email", "name"}) {
        if (!text_or_absent(claims, name)) {
            return refuse(std::string(name) + " is neither a string nor null");
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
                                                                     bool require_issued_at,
                                                                     std::string* reason) {
    std::string why;
    const auto refuse =
        [&](std::string message) -> std::optional<control::identity_crypto::OidcClaims> {
        if (reason) {
            *reason = std::move(message);
        }
        return std::nullopt;
    };
    const auto checked = [&](std::optional<control::identity_crypto::OidcClaims> verified,
                             const std::string& document,
                             const std::string& kid) {
        if (!verified) {
            return refuse(has_key(document, kid)
                              ? "signature or claims do not verify with key " + kid
                              : "key " + kid + " is not in the JWKS");
        }
        return verified;
    };
    try {
        const auto kid = token_key(token, issuer, audience, nonce, require_issued_at, why);
        if (!kid) {
            return refuse(why);
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
                    return checked(std::move(verified), entry.document, *kid);
                }
            }
            if (entry.fetching) {
                return refuse("key " + *kid +
                              " is unknown and another request is fetching the JWKS");
            }
            if (entry.attempted_at && now - *entry.attempted_at < std::chrono::seconds(60)) {
                return refuse("key " + *kid +
                              " is unknown and the JWKS was fetched less than 60 s ago");
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
        if (document.empty()) {
            return refuse("the JWKS could not be fetched from " + url);
        }
        return checked(control::identity_crypto::verify_oidc_id_token(
                           token, document, issuer, audience, nonce),
                       document,
                       *kid);
    } catch (const InvalidDocument&) {
        return refuse("a JSON document is nested too deeply");
    } catch (const Json::exception& error) {
        return refuse(std::string("JSON error: ") + error.what());
    }
}

TokenSummary summarize_token(std::string_view token) {
    TokenSummary summary;
    try {
        const auto first = token.find('.');
        const auto second = first == token.npos ? token.npos : token.find('.', first + 1);
        if (token.size() > 16384 || second == token.npos) {
            return summary;
        }
        const auto header = parse(decode(token.substr(0, first)));
        const auto claims = parse(decode(token.substr(first + 1, second - first - 1)));
        if (header.is_object()) {
            summary.algorithm = text(header, "alg");
            summary.key_id = text(header, "kid");
        }
        if (claims.is_object()) {
            summary.issuer = text(claims, "iss");
            summary.audience = claims.contains("aud") ? claims["aud"].dump() : std::string();
            summary.authorized_party = text(claims, "azp");
            for (const auto& [name, field] : {std::pair{"iat", &summary.issued_at},
                                              std::pair{"nbf", &summary.not_before},
                                              std::pair{"exp", &summary.expires_at}}) {
                if (claims.contains(name) && claims[name].is_number_integer()) {
                    *field = claims[name].get<long long>();
                }
            }
            for (const auto& [name, value] : claims.items()) {
                summary.claim_types[name] = value.type_name();
            }
        }
    } catch (const InvalidDocument&) {
    } catch (const Json::exception&) {
    }
    return summary;
}
} // namespace lake::api
