#pragma once

// Delivery of a user's workspace secret to a Kubernetes Secret (lake#625): a secret's source of
// truth is lake's secret store, sealed in its workspace Zetta; a Kubernetes Secret is only a
// projection, written by the orama-operator when an app, service or deployment must mount it.
//
// The operator asks `mint_sealed_secret` for a `LakeSealedSecret` with a `lake` source. Lake reads
// the secret version and answers each of its fields sealed to the cluster's X25519 delivery key
// (envelope version 1, orama-operator docs/lake/sealed-secrets.md), so plaintext exists only inside
// lake and in the produced Secret. No value is written to PostgreSQL, a log or the custom resource.

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "lake/api/registry.hpp"

namespace lake::api {

// The produced Secret a record is bound to, and the cluster key it is sealed to.
struct SealingRecipient {
    std::string cluster;        // the Lake object's Kubernetes UID
    std::string namespace_name; // the produced Secret's namespace
    std::string name;           // the produced Secret's name
    std::string kid;            // the recipient key id
    std::array<std::uint8_t, 32> public_key{};
};

// Fixed ephemeral key and nonce: tests only, to reproduce the operator's reference fixture.
struct SealingRandomness {
    std::array<std::uint8_t, 32> ephemeral_private{};
    std::array<std::uint8_t, 24> nonce{};
};

// The key id of a recipient public key: unpadded base64url of the first 16 bytes of its SHA-256.
std::string sealing_kid(const std::array<std::uint8_t, 32>& public_key);

// An unpadded base64url raw X25519 public key, as Lake status.sealing.publicKey carries it.
std::expected<std::array<std::uint8_t, 32>, std::string>
decode_sealing_public_key(std::string_view base64url);

// One version-1 record: standard padded base64 of the canonical-CBOR envelope
// {v, kid, epk, nonce, aad: {cluster, namespace, name}, ct}, with X25519, HKDF-SHA-256
// (zero salt, info "orama lake sealed secret v1" || epk || recipient) and XChaCha20-Poly1305 over
// the canonically encoded aad map. `fixed` is for tests; null draws from the system's CSPRNG.
std::expected<std::string, std::string> seal_for_cluster(const SealingRecipient& recipient,
                                                         std::string_view plaintext,
                                                         const SealingRandomness* fixed = nullptr);

// Whether a secret field can be a Kubernetes Secret data key ([-._a-zA-Z0-9]+, at most 253).
bool kubernetes_secret_key(std::string_view key);

// A reference to one field of one secret in a lake secret mount, as an app's secret environment
// stores it: `lake-secret://<mount id>/<secret name>#<field>`. The name may contain `/` and `#`;
// the field is what follows the last `#` and is a Kubernetes Secret key.
struct LakeSecretRef {
    std::string mount_id;
    std::string name;
    std::string field;
};
inline constexpr std::string_view lake_secret_scheme = "lake-secret://";
std::optional<LakeSecretRef> parse_lake_secret_ref(std::string_view reference);
std::string lake_secret_ref(const LakeSecretRef& reference);

// mint_sealed_secret: the operator's delivery of a `lake` source (and a `dynamic` one, through the
// mount's dynamic leases). Operator credential only.
void register_sealed_delivery_capabilities(Registry& registry);

} // namespace lake::api
