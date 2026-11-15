// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lake/control/identity_crypto.hpp"

namespace lake::api {

// What a refused token claimed, for the log: header and registered claims only. Never the token,
// its signature, its subject or its personal claims (lake#1115).
struct TokenSummary {
    std::string algorithm;
    std::string key_id;
    std::string issuer;
    std::string audience; // the `aud` claim as JSON: a string or an array
    std::string authorized_party;
    std::int64_t issued_at = 0;
    std::int64_t not_before = 0;
    std::int64_t expires_at = 0;
    std::map<std::string, std::string>
        claim_types; // claim name -> JSON type ("string", "null", ...)
};
TokenSummary summarize_token(std::string_view token);

// Shared by Clerk and OIDC. Call on the blocking pool: Fetch may perform network I/O.
// In-flight refreshes fail closed for other callers; failures also consume the cooldown.
// `reason`, when given, receives why a token was refused, in words safe to log.
// `audience_absent`: the token must carry no `aud` (Clerk's default session token, lake#1163).
class OidcJwks {
public:
    using Clock = std::chrono::steady_clock;
    using Fetch = std::function<std::optional<std::string>(const std::string&, bool public_only)>;
    using Now = std::function<Clock::time_point()>;
    explicit OidcJwks(Now now = Clock::now) : now_(std::move(now)) {}

    std::optional<control::identity_crypto::OidcClaims> verify(std::string_view token,
                                                               const std::string& url,
                                                               std::string_view issuer,
                                                               std::string_view audience,
                                                               std::string_view nonce,
                                                               bool public_only,
                                                               const Fetch& fetch,
                                                               bool require_issued_at = false,
                                                               std::string* reason = nullptr,
                                                               bool audience_absent = false);

private:
    struct Entry {
        std::string document;
        Clock::time_point fetched_at{};
        std::optional<Clock::time_point> attempted_at;
        bool fetching = false;
    };
    std::mutex mutex_;
    // Public-only fetch policy is part of the cache identity.
    std::map<std::pair<std::string, bool>, Entry> entries_;
    Now now_;
};

} // namespace lake::api
