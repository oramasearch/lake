#pragma once

// Using your own identity (lake#762, liquid-bridge decision 65,
// docs/api/identity.md#your-own-identity). An account that starts with a managed key may switch to
// the person's own liquid identity. The person proves they hold that identity's key by signing a
// statement lake issues for this account with `liquid sign artifact <challenge>`; libliquid's
// public codecs decode their identity document and that signature, and libliquid verifies both.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"

namespace lake::control::own_identity {

// The artifact type of the link the managed key signs, and of the statement's purpose.
inline constexpr std::string_view link_artifact = "identity_link";
inline constexpr std::string_view link_version = "taap_identity_link/1";
inline constexpr int challenge_lifetime_seconds = 600;

// A person's public identity document, verified.
struct Document {
    std::string taap_id;         // the identity's stable identifier
    std::string key_fingerprint; // the key it is signed with now
    std::string algorithm;
    std::string encoded; // the base64url canonical CBOR, exactly as `liquid identity show
                         // --document` prints it
    std::string hash;    // SHA-512/256 of the canonical bytes, lowercase hex
};

// Decodes `liquid identity show --document` output and verifies it: canonical CBOR, a valid
// self-signature, and an initial document, whose identifier is derived from its own key. A
// rotated successor names an identifier its key alone does not prove, so it is refused.
std::expected<Document, StoreError> verify_document(std::string_view encoded);

// The statement the person signs: which account, which managed identity it leaves, which
// identity it moves to (and that document's hash), a random nonce and when it expires.
nlohmann::json statement(const std::string& user_id,
                         const std::string& account_taap_id,
                         const Document& own,
                         const std::string& nonce,
                         const std::string& expires_at);

// The challenge: `sha256:<hex>` of the statement's canonical CBOR, the digest `liquid sign
// artifact` takes.
std::string challenge(const nlohmann::json& statement);

// Verifies the person's signature over `challenge` with the key of `document`. `signed_output`
// is what `liquid sign artifact <challenge>` printed (its `signature=<hex>` line, or that hex
// alone); a `signer=` line, when present, must name the document's identity. Returns the
// signature as JSON for the link document.
std::expected<nlohmann::json, StoreError> verify_challenge_signature(
    std::string_view document, std::string_view challenge, std::string_view signed_output);

// Enrolling a document (lake#931, docs/api/identity.md#proving-you-hold-the-key). A document is
// public, so password sign-up and a first OAuth sign-in bind one to a new account only with a fresh
// signature by its key over this challenge, which Lake issues without storing it: `sha256:<hex>` of
// an HMAC-SHA256, under a key derived from the instance key, over the canonical CBOR of the pool,
// the document (identifier, key fingerprint and hash), a random nonce and the expiry. Only this
// Lake can make one, so a nonce and expiry a client sends back are ones it issued for that pool and
// document. Empty when the HMAC fails.
inline constexpr std::string_view enrollment_purpose = "lake_enroll_identity";
inline constexpr int enrollment_lifetime_seconds = 300;
std::string enrollment_challenge(std::span<const unsigned char> instance_kek,
                                 std::string_view pool_id,
                                 const Document& document,
                                 std::string_view nonce,
                                 std::int64_t expires_at);

// The link body both keys stand behind; the managed key signs it as `identity_link`.
nlohmann::json link_body(const IdentityLinkChallenge& challenge, const std::string& linked_at);

// Re-checks a stored link: the managed signature with the managed key's public half, the
// statement's challenge, and the person's signature with their document's key.
std::expected<void, StoreError> verify_link(const nlohmann::json& link_document,
                                            std::string_view managed_public_key,
                                            std::string_view own_document);

} // namespace lake::control::own_identity
