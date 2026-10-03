#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/apps.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// An application's lifecycle events (lake#356) and their delivery attempts (lake#357), kept in the
// workspace Zetta of the application's workspace (docs/architecture/workspace-records.md). Both are
// found by the application (`application:<id>`), so one list_records call reads an application's
// event log and another its attempts.
//
// An event is appended once. An attempt is a mutable record whose key is the blind index of
// `event:<id>\nwebhook:<id>\nattempt:<n>`: a caller claims the attempt by writing it pending with
// `if_absent`, sends only if that write succeeded, then records the outcome with `if_revision`. Two
// callers (two lake processes, or two retries) can therefore never send the same attempt; the
// second claim is `conflict`.

struct ApplicationWebhookEventCodec {
    using Value = ApplicationWebhookEvent;
    static constexpr RecordCollection collection{"auth_app_webhook_events", 1};

    static std::string lookup(const ApplicationWebhookEvent& event);
    static nlohmann::json encode(const ApplicationWebhookEvent& event);
    static std::optional<ApplicationWebhookEvent> decode(const StoredRecord& record);
};

struct ApplicationWebhookDeliveryCodec {
    using Value = ApplicationWebhookDelivery;
    static constexpr RecordCollection collection{"auth_app_webhook_deliveries", 1};

    static std::string lookup(const ApplicationWebhookDelivery& delivery);
    static nlohmann::json encode(const ApplicationWebhookDelivery& delivery);
    static std::optional<ApplicationWebhookDelivery> decode(const StoredRecord& record);
};

using ApplicationWebhookEvents = RecordSet<ApplicationWebhookEventCodec>;

// The lookup of every event and attempt of one application.
std::string application_webhook_lookup(const std::string& application_id);

// The record key of one attempt in `records`' workspace.
std::string application_webhook_attempt_key(const WorkspaceRecords& records,
                                            const ApplicationWebhookDelivery& delivery);

// One event by id; nullopt when it does not exist here or belongs to another application.
liquid::runtime::task<ZettaControlResult<std::optional<ApplicationWebhookEvent>>>
get_application_webhook_event(WorkspaceRecords& records, std::string application_id,
                              std::string event_id);

// The newest `newest` events of an application, newest first.
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationWebhookEvent>>>
list_application_webhook_events(WorkspaceRecords& records, std::string application_id,
                                std::size_t newest);

// The attempts of an application made at or after `since_us`, oldest first.
liquid::runtime::task<ZettaControlResult<std::vector<ApplicationWebhookDelivery>>>
list_application_webhook_deliveries(WorkspaceRecords& records, std::string application_id,
                                    std::int64_t since_us);

// A claimed attempt and the engine revision its outcome is written at.
struct ClaimedApplicationWebhookDelivery {
    ApplicationWebhookDelivery delivery;
    std::int64_t revision = 0;
};

// Writes `delivery` as its attempt's record only if no caller has claimed that attempt: pending for
// a new attempt, or finished for a copied one. `conflict` when the attempt is already recorded.
liquid::runtime::task<ZettaControlResult<ClaimedApplicationWebhookDelivery>>
claim_application_webhook_delivery(WorkspaceRecords& records, ApplicationWebhookDelivery delivery);

// Records the outcome of a claimed attempt, conditional on the revision the claim wrote.
liquid::runtime::task<ZettaControlResult<ApplicationWebhookDelivery>>
settle_application_webhook_delivery(WorkspaceRecords& records,
                                    ClaimedApplicationWebhookDelivery claimed);

} // namespace lake::control
