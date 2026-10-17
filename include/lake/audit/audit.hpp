#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {
class Store;
}

namespace lake::audit {

inline constexpr int retention_floor_days = 400;

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

struct EventDraft {
    std::string customer_id;
    std::string kind;
    std::string principal;
    std::string object;
    std::string action;
    std::string outcome;
    std::string request_id;
    std::string method;
    std::string device;
    nlohmann::json details = nlohmann::json::object();
};

struct Event {
    std::string id;
    std::string occurred_at;
    EventDraft draft;
    std::string previous_digest;
    std::string signer;
    std::string signature;
    std::string digest;
};

struct Filter {
    std::string principal;
    std::string object;
    std::string outcome;
    std::string from;
    std::string to;
    int limit = 100;
    std::string request_id;
    // Continue after this event in append order, independently of timestamps.
    std::string after_id;
    // Return the newest matching events first (lake#445). Chain verification, export and
    // after_id paging keep append order.
    bool newest_first = false;
    // Leave out the successful events of these actions: the reads a browser polls, so a
    // person's activity shows what changed (lake#445). Failed and denied reads still appear.
    std::vector<std::string> omit_successful_actions;
};

struct Identity {
    std::string id;
    std::string taap_id;
    std::string public_key;
    std::string wrapped_seed;
    std::string created_at;
    std::string rotation_document;
    bool current = true;
};

struct IdentityOptions {
    std::string configured_seed;
    std::string instance_kek;
};

nlohmann::json to_json(const Event& event);
// The inverse of to_json for events read back through the registry (pages never read the
// store directly). Absent or null optional fields decode as empty strings.
Event event_from_json(const nlohmann::json& value);
nlohmann::json identity_document(const Identity& identity);
std::string json_lines(const std::vector<Event>& events);

// An event whose id and instant are fixed when its call finishes, and whose predecessor and
// signature are added when it is written: a workspace's audit, which lake writes off the request
// path (docs/api/audit.md#workspace-audit, decision 52).
struct PendingEvent {
    std::string id;
    std::string occurred_at;
    EventDraft draft;
};

// Gives `draft` its id and instant now.
PendingEvent pending(const EventDraft& draft);

// Signs a pending event after `previous_digest` with `signer` (the current identity, from
// `identity`). The event keeps the id and instant it was given when its call finished.
Result<Event> sign_pending(control::Store& store,
                           const Identity& signer,
                           const PendingEvent& event,
                           const std::string& previous_digest,
                           const IdentityOptions& options);

// Checks one event alone: its signature against lake's identity history and its digest, but not
// its predecessor. A workspace's copy of an event recorded centrally before decision 52 is
// checked this way; the central chain holds its place.
Result<void> verify_event(control::Store& store, const Event& event);

// Appends one signed event. The store enforces the predecessor supplied by this
// function.
Result<Event>
record(control::Store& store, const EventDraft& draft, const IdentityOptions& options);

// Recomputes every digest, signature and predecessor relation in oldest-first
// order.
Result<void> verify(control::Store& store, const std::vector<Event>& events);

// Returns the current instance identity, creating and persisting it when
// necessary.
Result<Identity> identity(control::Store& store, const IdentityOptions& options);

// Mints a fresh Ed25519 TAAP identity whose seed is wrapped under `instance_kek`, for a person
// lake holds an identity for (the custodial_identities table, docs/api/clerk.md). Nothing is
// stored here; the caller persists the result.
Result<Identity> mint_custodial_identity(std::string_view instance_kek);

// Signs `TAAP/1 <artifact_type> || 0x00 || canonical CBOR(body)` with an identity lake holds
// for a person (a managed key, docs/api/identity.md#your-own-identity): its seed is unwrapped
// under `instance_kek` for this one signature. Returns the base64 Ed25519 signature.
Result<std::string> sign_with_managed_key(std::string_view wrapped_seed,
                                          std::string_view instance_kek,
                                          std::string_view artifact_type,
                                          const nlohmann::json& body);
// Verifies a signature from sign_with_managed_key against the key's base64 public half.
Result<void> verify_managed_signature(std::string_view public_key,
                                      std::string_view artifact_type,
                                      const nlohmann::json& body,
                                      std::string_view signature);
// The instance key a store's held identities are wrapped under: `instance_kek`, or the memory
// store's fixed development key when it is empty.
std::string effective_instance_kek(control::Store& store, std::string_view instance_kek);

// Creates a dual-signed TAAP rotation document and makes the new identity
// current.
Result<Identity> rotate_identity(control::Store& store, const IdentityOptions& options);

// Signs one domain-separated TAAP artifact with the current lake identity. The caller supplies
// the expected signer so a concurrent identity rotation cannot bind a body to the wrong key.
Result<std::vector<std::byte>> sign_taap(control::Store& store,
                                         const std::string& expected_signer,
                                         std::string_view artifact_type,
                                         std::span<const std::byte> canonical_body,
                                         const IdentityOptions& options);

// Signs and verifies the TAAP identity binding stored with a passkey credential.
Result<nlohmann::json> sign_passkey_binding(control::Store& store,
                                            const std::string& subject_taap_id,
                                            const std::string& credential_id,
                                            const std::string& pool_id,
                                            const IdentityOptions& options);
Result<void> verify_passkey_binding(control::Store& store,
                                    const nlohmann::json& attestation,
                                    const std::string& subject_taap_id,
                                    const std::string& credential_id,
                                    const std::string& pool_id);

Result<nlohmann::json> sign_offline_grant(control::Store& store,
                                          const std::string& grant_id,
                                          const std::string& subject_taap_id,
                                          const std::string& device_id,
                                          const std::vector<std::string>& scopes,
                                          std::int64_t expires_unix,
                                          const IdentityOptions& options);
Result<void> verify_offline_grant(control::Store& store,
                                  const nlohmann::json& attestation,
                                  const std::string& scope,
                                  std::int64_t now_unix);

// Signs the canonical checkpoint body with the current lake identity. The
// returned signer and signature are retained with the immutable checkpoint.
Result<nlohmann::json> sign_transparency_checkpoint(control::Store& store,
                                                    const nlohmann::json& checkpoint,
                                                    const IdentityOptions& options);
Result<void> verify_transparency_checkpoint(control::Store& store,
                                            const nlohmann::json& checkpoint,
                                            const nlohmann::json& signature);

// Signs the canonical CBOR form of a portable-export manifest with the lake
// identity. The verification input carries the source identity document, so a
// destination lake does not need the source control-plane database.
Result<nlohmann::json> sign_export_manifest(control::Store& store,
                                            const nlohmann::json& manifest,
                                            const IdentityOptions& options);
Result<void> verify_export_manifest(const nlohmann::json& manifest,
                                    const nlohmann::json& signature);

Result<void> validate_retention_days(int days);

} // namespace lake::audit
