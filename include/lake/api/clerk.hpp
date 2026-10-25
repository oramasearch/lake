#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lake::api {

// Clerk as lake's browser sign-in (docs/api/clerk.md). The public issuer comes from the
// publishable key: `pk_test_` or `pk_live_` followed by base64 of the instance's
// Frontend API host and a trailing `$`. lake verifies Clerk's tokens against that host's JWKS, so
// its secret key is used separately for the current account-status check.
struct ClerkConfig {
    std::string publishable_key;
    std::string frontend_api; // host, e.g. example-123.clerk.accounts.dev
    std::string issuer;       // https://<frontend_api>
    std::string jwks_url;     // <issuer>/.well-known/jwks.json
};

struct ClerkResponse {
    int status = 0;
    std::string body, error;
};
using ClerkGet = std::function<ClerkResponse(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers)>;
struct ClerkUserStatus {
    std::int64_t updated_at = 0;
    bool banned = false;
    bool locked = false;
    bool deprovisioned = false;
};
// Authoritative, uncached account state. All missing configuration, transport and shape failures
// refuse exchange. The fixed Backend API origin never comes from the token or the browser.
std::expected<ClerkUserStatus, std::string>
clerk_user_status(const std::string& subject, const std::string& secret_key, const ClerkGet& get);

// The JWT template the browser asks Clerk for; it must set `aud` to this value.
inline constexpr std::string_view clerk_token_template = "lake";

// nullopt when the key is empty, the `xxx` placeholder, or not a Clerk publishable key; lake then
// signs people in with its own identity pages, as an on-prem install does.
std::optional<ClerkConfig> clerk_config(std::string_view publishable_key);

// Whether Lake's own password entries serve the `lake` pool: sign_up_password, sign_in_password,
// verify_mfa and recover_with_code. They do only when Clerk is not configured; under Clerk, Clerk
// is the only way into the `lake` pool, sign-up included (lake#1098). `viewer` reports this as
// `password_sign_up` and the entries refuse with it, so the two cannot disagree. Application
// pools keep their own password sign-in either way.
bool lake_pool_password_sign_in(std::string_view clerk_publishable_key);

} // namespace lake::api
