#include "lake/api/oauth.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

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

std::string joined_scopes(const std::vector<std::string> &scopes) {
  std::string joined;
  for (const std::string &scope : scopes) {
    if (!joined.empty()) {
      joined += ' ';
    }
    joined += scope;
  }
  return joined;
}

bool endpoint_allowed(std::string_view endpoint) {
  return endpoint.empty() || endpoint.starts_with("https://") ||
         endpoint.starts_with("http://127.0.0.1:") ||
         endpoint.starts_with("http://localhost:") ||
         endpoint.starts_with("http://[::1]:");
}

} // namespace

std::string oauth_url_encode(std::string_view value) {
  std::string encoded;
  for (const unsigned char byte : value) {
    if (std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' ||
        byte == '~') {
      encoded += static_cast<char>(byte);
    } else {
      encoded += std::format("%{:02X}", static_cast<unsigned>(byte));
    }
  }
  return encoded;
}

std::string oauth_callback_url(std::string_view public_host,
                               std::string_view provider) {
  return "https://" + std::string(public_host) + "/v1/oauth/" +
         oauth_url_encode(provider) + "/callback";
}

OAuthEndpoints oauth_endpoints(const control::OAuthProvider &provider) {
  if (provider.provider == "github") {
    return {"https://github.com/login/oauth/authorize",
            "https://github.com/login/oauth/access_token", "",
            "https://api.github.com/user"};
  }
  if (provider.provider == "google") {
    return {"https://accounts.google.com/o/oauth2/v2/auth",
            "https://oauth2.googleapis.com/token",
            "https://www.googleapis.com/oauth2/v3/certs", ""};
  }
  const std::string issuer = without_trailing_slash(provider.issuer);
  return {issuer + "/authorize", issuer + "/token", issuer + "/jwks", ""};
}

bool oauth_endpoints_allowed(const OAuthEndpoints &endpoints) {
  return endpoint_allowed(endpoints.authorization) &&
         endpoint_allowed(endpoints.token) &&
         endpoint_allowed(endpoints.jwks) && endpoint_allowed(endpoints.user);
}

std::string oauth_authorization_url(const control::OAuthProvider &provider,
                                    const control::OAuthFlow &flow,
                                    std::string_view public_host) {
  const OAuthEndpoints endpoints = oauth_endpoints(provider);
  const std::string callback =
      oauth_callback_url(public_host, provider.provider);
  const std::string challenge =
      control::identity_crypto::sha256_base64url(flow.code_verifier);
  std::string result = endpoints.authorization;
  result += "?response_type=code";
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
