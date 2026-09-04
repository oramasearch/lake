#pragma once

#include <string>
#include <string_view>

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

std::string oauth_url_encode(std::string_view value);
std::string oauth_callback_url(std::string_view public_host, std::string_view provider);
OAuthEndpoints oauth_endpoints(const control::OAuthProvider& provider);
bool oauth_endpoints_allowed(const OAuthEndpoints& endpoints);
std::string oauth_authorization_url(const control::OAuthProvider& provider,
                                    const control::OAuthFlow& flow,
                                    std::string_view public_host);

} // namespace lake::api
