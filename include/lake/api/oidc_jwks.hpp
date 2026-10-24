#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lake/control/identity_crypto.hpp"

namespace lake::api {

// Shared by Clerk and OIDC. Call on the blocking pool: Fetch may perform network I/O.
// In-flight refreshes fail closed for other callers; failures also consume the cooldown.
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
                                                               bool require_issued_at = false);

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
