#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lake::api {

// Clerk as lake's browser sign-in (docs/api/clerk.md). Everything lake needs comes from the
// publishable key, which is public: `pk_test_` or `pk_live_` followed by base64 of the instance's
// Frontend API host and a trailing `$`. lake verifies Clerk's tokens against that host's JWKS, so
// it holds no Clerk secret.
struct ClerkConfig {
    std::string publishable_key;
    std::string frontend_api; // host, e.g. example-123.clerk.accounts.dev
    std::string issuer;       // https://<frontend_api>
    std::string jwks_url;     // <issuer>/.well-known/jwks.json
};

// The JWT template the browser asks Clerk for; it must set `aud` to this value.
inline constexpr std::string_view clerk_token_template = "lake";

// nullopt when the key is empty, the `xxx` placeholder, or not a Clerk publishable key; lake then
// signs people in with its own identity pages, as an on-prem install does.
std::optional<ClerkConfig> clerk_config(std::string_view publishable_key);

} // namespace lake::api
