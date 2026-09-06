#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace lake::control::identity_crypto {

std::string random_secret();
std::string hash_secret(std::string_view secret);
bool verify_secret(std::string_view encoded, std::string_view secret);
std::string new_totp_secret();
bool verify_totp(std::string_view secret, std::string_view code);
std::string sha256_hex(std::string_view value);
std::string sha256_base64url(std::string_view value);
std::string base64url_encode(std::string_view value);

struct VerifiedTaapSignIn {
    std::string taap_id;
    std::string assertion_id;
};

std::expected<VerifiedTaapSignIn, std::string>
verify_taap_sign_in(std::string_view identity_document,
                    std::string_view pool_id,
                    std::string_view nonce,
                    std::int64_t issued_at,
                    const nlohmann::json& signature,
                    std::int64_t now_unix = 0);

struct OidcClaims {
    std::string subject;
    std::string email;
    std::string display_name;
};

std::optional<OidcClaims> verify_oidc_id_token(std::string_view token,
                                               std::string_view jwks,
                                               std::string_view issuer,
                                               std::string_view client_id,
                                               std::string_view nonce,
                                               long now_unix = 0);

struct VerifiedPasskeyRegistration {
    std::string credential_id;
    std::string public_key_pem;
    std::uint32_t sign_count = 0;
};

std::expected<VerifiedPasskeyRegistration, std::string>
verify_passkey_registration(std::string_view client_data_json,
                            std::string_view attestation_object,
                            std::string_view challenge,
                            std::string_view rp_id,
                            std::string_view origin);
std::expected<std::uint32_t, std::string>
verify_passkey_assertion(std::string_view client_data_json,
                         std::string_view authenticator_data,
                         std::string_view signature,
                         std::string_view public_key_pem,
                         std::string_view challenge,
                         std::string_view rp_id,
                         std::string_view origin);

} // namespace lake::control::identity_crypto
