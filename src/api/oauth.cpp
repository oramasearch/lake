#include "lake/api/oauth.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

#include <nlohmann/json.hpp>

#include "lake/control/identity_crypto.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

std::string without_trailing_slash(std::string value) {
    while (value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

std::string joined_scopes(const std::vector<std::string>& scopes) {
    std::string joined;
    for (const std::string& scope : scopes) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += scope;
    }
    return joined;
}

bool loopback_http(std::string_view endpoint) {
    return endpoint.starts_with("http://127.0.0.1:") || endpoint.starts_with("http://localhost:") ||
           endpoint.starts_with("http://[::1]:");
}

bool endpoint_allowed(std::string_view endpoint, bool allow_loopback_http) {
    return endpoint.empty() || endpoint.starts_with("https://") ||
           (allow_loopback_http && loopback_http(endpoint));
}

// A URL Lake may call or send a browser to: HTTPS (or loopback HTTP for tests), with a host and
// nothing a browser or the client would read as another target.
bool usable_url(std::string_view url, bool allow_loopback_http) {
    if (url.empty() || url.size() > 2048 || !endpoint_allowed(url, allow_loopback_http) ||
        std::ranges::any_of(url, [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
        return false;
    }
    const std::size_t host = url.find("://") + 3;
    return host < url.size() && url[host] != '/' && url.find('#') == std::string_view::npos &&
           url.find('@') == std::string_view::npos;
}

} // namespace

OidcDiscovery::OidcDiscovery(OidcFetch fetch,
                             bool allow_private_addresses,
                             std::chrono::seconds ttl)
    : fetch_(std::move(fetch)), allow_private_addresses_(allow_private_addresses), ttl_(ttl) {}

std::expected<OAuthEndpoints, ApiError> OidcDiscovery::endpoints(const std::string& issuer,
                                                                 bool refresh) {
    const auto failure = [](std::string code, std::string message) {
        return std::unexpected(ApiError{.code = std::move(code), .message = std::move(message)});
    };
    {
        std::lock_guard lock(mutex_);
        const auto found = cache_.find(issuer);
        if (!refresh && found != cache_.end() &&
            std::chrono::steady_clock::now() - found->second.fetched_at < ttl_) {
            return found->second.endpoints;
        }
    }
    if (!fetch_) {
        return failure("not_supported", "OpenID Connect discovery is not available");
    }
    const std::string url = oidc_discovery_url(issuer);
    if (!usable_url(url, allow_private_addresses_)) {
        return failure("invalid_input",
                       "the issuer must be an HTTPS URL for OpenID Connect discovery");
    }
    const OidcFetchResponse response = fetch_(url);
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return failure("engine_error",
                       "the issuer's OpenID Connect discovery document could not be read" +
                           (response.error.empty()
                                ? " (HTTP " + std::to_string(response.status) + ")"
                                : ": " + response.error));
    }
    auto parsed = parse_oidc_discovery(response.body, issuer, allow_private_addresses_);
    if (!parsed) {
        return failure("invalid_input", parsed.error());
    }
    std::lock_guard lock(mutex_);
    cache_[issuer] = {*parsed, std::chrono::steady_clock::now()};
    return std::move(*parsed);
}

std::string oauth_url_encode(std::string_view value) {
    std::string encoded;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            encoded += static_cast<char>(byte);
        } else {
            encoded += std::format("%{:02X}", static_cast<unsigned>(byte));
        }
    }
    return encoded;
}

std::string oauth_callback_url(std::string_view public_host, std::string_view provider) {
    return "https://" + std::string(public_host) + "/v1/oauth/" + oauth_url_encode(provider) +
           "/callback";
}

std::string oidc_discovery_url(std::string_view issuer) {
    return without_trailing_slash(std::string(issuer)) + "/.well-known/openid-configuration";
}

std::expected<OAuthEndpoints, std::string>
parse_oidc_discovery(std::string_view document, std::string_view issuer, bool allow_loopback_http) {
    const nlohmann::json value = nlohmann::json::parse(document, nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return std::unexpected("the issuer's OpenID Connect discovery document is not JSON");
    }
    const auto text = [&](const char* name) {
        const auto found = value.find(name);
        return found != value.end() && found->is_string() ? found->get<std::string>()
                                                          : std::string();
    };
    // OpenID Connect Discovery 1.0 §4.3: the document's issuer is the configured one, exactly.
    if (text("issuer") != issuer) {
        return std::unexpected("the discovery document's issuer \"" + text("issuer") +
                               "\" is not the configured issuer \"" + std::string(issuer) + "\"");
    }
    OAuthEndpoints endpoints{
        text("authorization_endpoint"), text("token_endpoint"), text("jwks_uri"), ""};
    for (const auto& [name, url] : {std::pair{"authorization_endpoint", endpoints.authorization},
                                    std::pair{"token_endpoint", endpoints.token},
                                    std::pair{"jwks_uri", endpoints.jwks}}) {
        if (!usable_url(url, allow_loopback_http)) {
            return std::unexpected(std::string("the discovery document's ") + name +
                                   " is missing or is not an HTTPS URL");
        }
    }
    // Lake verifies RS256 ID tokens; a provider that says it signs with nothing else cannot work.
    if (const auto algorithms = value.find("id_token_signing_alg_values_supported");
        algorithms != value.end()) {
        if (!algorithms->is_array() ||
            std::ranges::none_of(*algorithms, [](const nlohmann::json& item) {
                return item.is_string() && item.get<std::string>() == "RS256";
            })) {
            return std::unexpected("the provider does not sign ID tokens with RS256");
        }
    }
    return endpoints;
}

std::expected<OAuthEndpoints, ApiError>
oauth_endpoints(const control::OAuthProvider& provider, OidcDiscovery* discovery, bool refresh) {
    if (provider.provider == "github") {
        return OAuthEndpoints{"https://github.com/login/oauth/authorize",
                              "https://github.com/login/oauth/access_token",
                              "",
                              "https://api.github.com/user"};
    }
    if (provider.provider == "google") {
        return OAuthEndpoints{"https://accounts.google.com/o/oauth2/v2/auth",
                              "https://oauth2.googleapis.com/token",
                              "https://www.googleapis.com/oauth2/v3/certs",
                              ""};
    }
    if (discovery == nullptr) {
        return std::unexpected(ApiError{.code = "not_supported",
                                        .message = "OpenID Connect discovery is not available"});
    }
    return discovery->endpoints(provider.issuer, refresh);
}

bool oauth_endpoints_allowed(const OAuthEndpoints& endpoints, bool allow_loopback_http) {
    return endpoint_allowed(endpoints.authorization, allow_loopback_http) &&
           endpoint_allowed(endpoints.token, allow_loopback_http) &&
           endpoint_allowed(endpoints.jwks, allow_loopback_http) &&
           endpoint_allowed(endpoints.user, allow_loopback_http);
}

std::string oauth_authorization_url(const control::OAuthProvider& provider,
                                    const OAuthEndpoints& endpoints,
                                    const control::OAuthFlow& flow,
                                    std::string_view public_host) {
    const std::string callback = oauth_callback_url(public_host, provider.provider);
    const std::string challenge = control::identity_crypto::sha256_base64url(flow.code_verifier);
    std::string result = endpoints.authorization;
    // Discovery may name an authorization endpoint that already carries a query (Entra's p=).
    result += endpoints.authorization.find('?') == std::string::npos ? "?" : "&";
    result += "response_type=code";
    result += "&client_id=" + oauth_url_encode(provider.client_id);
    result += "&redirect_uri=" + oauth_url_encode(callback);
    result += "&scope=" + oauth_url_encode(joined_scopes(provider.scopes));
    result += "&state=" + oauth_url_encode(flow.state);
    result += "&code_challenge=" + oauth_url_encode(challenge);
    result += "&code_challenge_method=S256";
    if (provider.provider != "github") {
        result += "&nonce=" + oauth_url_encode(flow.nonce);
    }
    return result;
}

} // namespace lake::api
