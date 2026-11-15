// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "lake/api/registry.hpp"

namespace lake::control {
struct OAuthFlow;
struct OAuthProvider;
} // namespace lake::control

namespace lake::api {

struct OAuthEndpoints {
    std::string authorization;
    std::string token;
    std::string jwks;
    std::string user;
};

// What fetching a discovery document answered: an HTTP status and body, or a transport error.
struct OidcFetchResponse {
    int status = 0;
    std::string body;
    std::string error;
};
using OidcFetch = std::function<OidcFetchResponse(const std::string& url)>;

// OpenID Connect Discovery 1.0 (docs/api/identity.md#openid-connect-discovery, lake#837): a
// provider's endpoints come from `<issuer>/.well-known/openid-configuration`, whose `issuer` must
// be the configured issuer exactly. Documents are cached per issuer for `ttl`. The fetch is the
// server's outbound client with its timeouts and, unless private addresses are allowed, HTTPS to
// public addresses only (lake#876); it blocks, so callers run it on the blocking pool.
class OidcDiscovery {
public:
    OidcDiscovery(OidcFetch fetch,
                  bool allow_private_addresses,
                  std::chrono::seconds ttl = std::chrono::hours(1));

    // `refresh` skips the cache, for a retry after a token's key was not found. engine_error when
    // the document cannot be fetched, invalid_input when it is not acceptable.
    std::expected<OAuthEndpoints, ApiError> endpoints(const std::string& issuer,
                                                      bool refresh = false);
    bool allows_private_addresses() const {
        return allow_private_addresses_;
    }

private:
    struct Cached {
        OAuthEndpoints endpoints;
        std::chrono::steady_clock::time_point fetched_at;
    };
    OidcFetch fetch_;
    bool allow_private_addresses_;
    std::chrono::seconds ttl_;
    std::mutex mutex_;
    std::map<std::string, Cached> cache_;
};

std::string oauth_url_encode(std::string_view value);
std::string oauth_callback_url(std::string_view public_host, std::string_view provider);
// `<issuer>/.well-known/openid-configuration`, the issuer's trailing slash removed first.
std::string oidc_discovery_url(std::string_view issuer);
// Reads a discovery document for `issuer`: the issuer must match exactly, the three endpoints must
// be present and HTTPS (plain HTTP only to loopback, and only when private addresses are allowed),
// and RS256 must be among the ID token algorithms when the document lists them.
std::expected<OAuthEndpoints, std::string>
parse_oidc_discovery(std::string_view document, std::string_view issuer, bool allow_loopback_http);
// Google and GitHub have fixed endpoints; any other provider's come from discovery. Without a
// discovery client (a registry with no server) a generic provider has none.
std::expected<OAuthEndpoints, ApiError> oauth_endpoints(const control::OAuthProvider& provider,
                                                        OidcDiscovery* discovery,
                                                        bool refresh = false);
bool oauth_endpoints_allowed(const OAuthEndpoints& endpoints, bool allow_loopback_http);
std::string oauth_authorization_url(const control::OAuthProvider& provider,
                                    const OAuthEndpoints& endpoints,
                                    const control::OAuthFlow& flow,
                                    std::string_view public_host);

} // namespace lake::api
