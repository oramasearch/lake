#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <liquid/identity.hpp>
#include <nlohmann/json_fwd.hpp>

namespace lake::control::identity_crypto {

std::string random_secret();
std::string hash_secret(std::string_view secret);
bool verify_secret(std::string_view encoded, std::string_view secret);
// New recovery codes are 128 random bits with a version prefix; legacy codes are refused.
std::string new_recovery_code();
std::string recovery_selector(std::string_view code);
std::string new_totp_secret();
bool verify_totp(std::string_view secret, std::string_view code);
std::optional<std::int64_t> totp_step(std::string_view secret, std::string_view code);
std::string sha256_hex(std::string_view value);
std::string sha256_base64url(std::string_view value);
std::string base64url_encode(std::string_view value);

struct VerifiedTaapSignIn {
    std::string taap_id;
    std::string assertion_id;
};

// Verifies a TAAP sign-in assertion (docs/api/identity.md#signing-in-with-a-taap-key). The
// signature must be made with the key bound to the document's identifier (bound_taap_key), so a
// document naming someone else's identifier with another key is refused (lake#917).
std::expected<VerifiedTaapSignIn, std::string>
verify_taap_sign_in(std::string_view identity_document,
                    std::string_view pool_id,
                    std::string_view nonce,
                    std::int64_t issued_at,
                    const nlohmann::json& signature,
                    std::int64_t now_unix = 0,
                    std::span<const std::string> identity_chain = {});

// Whether a TAAP/1 document is an initial document whose identifier is the one its own key
// derives (`taap:1:<algorithm>:<key fingerprint>`): the only document whose key alone proves its
// identifier (lake#917).
bool identity_document_is_self_certifying(const liquid::identity_document& document);

// The key bound to a presented identity (base64url canonical CBOR): the document's own key when it
// is self-certifying, or, given `identity_chain` (oldest first, ending with the document), the
// current key of that chain, which liquid's verify_chain accepts, whose first document is
// self-certifying, and whose every rotation the replaced key signed. Anything else is refused.
std::expected<liquid::actor, std::string>
bound_taap_key(std::string_view identity_document, std::span<const std::string> identity_chain);

// The identifier a document claims, for the audit of a refused sign-in; empty when it does not
// decode.
std::string claimed_taap_id(std::string_view identity_document);

struct OidcClaims {
    std::string subject;
    std::string email;
    std::string display_name;
    std::int64_t issued_at = 0; // Clerk rejects templates predating an upstream revocation
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
