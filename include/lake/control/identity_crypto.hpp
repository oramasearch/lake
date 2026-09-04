#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lake::control::identity_crypto {

std::string random_secret();
std::string hash_secret(std::string_view secret);
bool verify_secret(std::string_view encoded, std::string_view secret);
std::string new_totp_secret();
bool verify_totp(std::string_view secret, std::string_view code);
std::string sha256_hex(std::string_view value);
std::string sha256_base64url(std::string_view value);

struct OidcClaims {
  std::string subject;
  std::string email;
  std::string display_name;
};

std::optional<OidcClaims>
verify_oidc_id_token(std::string_view token, std::string_view jwks,
                     std::string_view issuer, std::string_view client_id,
                     std::string_view nonce, long now_unix = 0);

} // namespace lake::control::identity_crypto
