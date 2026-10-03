#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/apps.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// An application's credentials, kept in the workspace Zetta of the application's workspace
// (docs/architecture/workspace-records.md): its Ed25519 signing keys (lake#352) and its lifecycle
// webhooks with their signing secrets (lake#355). Both are found by the application
// (`application:<id>`). A private key or secret is held only as the secrets subsystem's envelope
// (Store::encrypt_application_credential) inside the record's sealed body; the workspace holds
// ciphertext of that envelope and nothing in the clear.
//
// Signing keys are one record per application, keyed by the application id: the key ring. Its
// public keys are what the anonymous JWKS read returns, from one get_record. A rotation retires the
// current key and adds the next in one write conditional on the revision it read (`if_revision`),
// so there is always exactly one current key and two rotations never both win from the same ring.
// A webhook is one record keyed by its id, written once with `if_absent`.

struct ApplicationSigningKeysCodec {
    using Value = ApplicationSigningKeyRing;
    static constexpr RecordCollection collection{"auth_app_signing_keys", 1};

    static std::string lookup(const ApplicationSigningKeyRing& ring);
    static nlohmann::json encode(const ApplicationSigningKeyRing& ring);
    static std::optional<ApplicationSigningKeyRing> decode(const StoredRecord& record);
};

struct ApplicationWebhookCodec {
    using Value = ApplicationWebhook;
    static constexpr RecordCollection collection{"auth_app_webhooks", 1};

    static std::string lookup(const ApplicationWebhook& webhook);
    static nlohmann::json encode(const ApplicationWebhook& webhook);
    static std::optional<ApplicationWebhook> decode(const StoredRecord& record);
};

// The application's key ring; nullopt when there is none here (or it belongs to another
// application, or does not open in this workspace).
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationSigningKeyRing>>>
get_application_signing_keys(WorkspaceRecords& records, std::string application_id);

// Writes a new application's key ring (or a copied one) only where none exists: `conflict`
// otherwise. `ring.at_us` 0 is now.
liquid::runtime::task<ZettaControlResult<ApplicationSigningKeyRing>>
create_application_signing_keys(WorkspaceRecords& records, ApplicationSigningKeyRing ring);

// Makes the key of `version` (seed already enveloped in `encrypted_seed`), or says why it cannot.
using SigningKeyMaker = std::function<std::expected<ApplicationSigningKey, ZettaControlError>(int)>;

// Retires the current key and makes the next one current, in one write conditional on the revision
// read; on a concurrent rotation it rereads and makes the next version again, up to `attempts`
// times. `not_found` when the application has no key ring here. Answers the new current key.
liquid::runtime::task<ZettaControlResult<ApplicationSigningKey>>
rotate_application_signing_keys(WorkspaceRecords& records, std::string application_id,
                                SigningKeyMaker make_key, int attempts = 5);

// Writes a webhook only where its id is absent: `conflict` otherwise. `webhook.at_us` 0 is now.
liquid::runtime::task<ZettaControlResult<ApplicationWebhook>>
create_application_webhook(WorkspaceRecords& records, ApplicationWebhook webhook);

// One webhook by id; nullopt when it does not exist here or belongs to another application.
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationWebhook>>>
get_application_webhook(WorkspaceRecords& records, std::string application_id,
                        std::string webhook_id);

// The newest `newest` webhooks of an application (0: all), newest first.
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationWebhook>>>
list_application_webhooks(WorkspaceRecords& records, std::string application_id,
                          std::size_t newest = 0);

} // namespace lake::control
