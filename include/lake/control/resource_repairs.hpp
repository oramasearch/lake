#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/builds.hpp"
#include "lake/control/web_apps.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// What Lake tells a person about a resource of theirs that failed, and how an agent may repair it
// (lake#1207, docs/api/notifications.md). The mechanism is general over the resource's kind: a
// notification and a repair record name a `resource_kind` and a `resource_id`. Apps (`app`, fixed
// by the Application Agent) are the first kind; custom agents (`agent`, fixed by agent-creator,
// lake#1209) are the next, with their own failure signals and turns, and the same records.
//
// Both are workspace data, so both live in the workspace Zetta of the resource's workspace as
// sealed records found by the resource's space: notifications in `lake_resource_notifications`,
// one resource's repair settings in `lake_resource_repairs` under `<kind>.<id>`. Lake's Postgres
// keeps only the resources' desired state, as before.

// The kinds a repair record or notification may name.
inline constexpr std::string_view resource_kind_app = "app";
bool known_resource_kind(std::string_view kind);

// One notification: what failed (`event`: an app's rollout_failed or build_failed, or
// auto_fix_stopped for any kind), the reason in plain words, and what the resource reported. Its
// id is derived from what it is about, so the same failure is recorded once however often it is
// seen.
struct ResourceNotification {
    std::string id;
    std::string customer_id;
    std::string workspace_id;
    std::string resource_kind;
    std::string resource_id;
    std::string resource_name;
    std::string event;
    std::string reason;
    std::string detail;
    std::string phase;
    std::int64_t generation = 0;
    std::string build_id;
    std::string recipient_user_id; // who deployed the resource; empty: the workspace
    std::string resolved_at;       // set once the resource is healthy again
    std::vector<std::string> dismissed_by;
    std::string created_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
};

struct ResourceNotificationCodec {
    using Value = ResourceNotification;
    static constexpr RecordCollection collection{"resource_notifications", 1};

    static std::string lookup(const ResourceNotification& notification);
    static nlohmann::json encode(const ResourceNotification& notification);
    static std::optional<ResourceNotification> decode(const StoredRecord& record);
};

// One resource's repair settings and history: auto-fix (opt in, off by default), its attempt
// limit and the attempts made, who deployed the resource and from which source, and the agent
// conversations that repaired or changed it.
struct ResourceRepair {
    std::string resource_kind;
    std::string resource_id;
    std::string customer_id;
    std::string deployed_by;
    // The source it was last deployed from. An app deployed with a built image keeps no source in
    // its desired state, and the agent needs the repository to repair it.
    nlohmann::json source = nlohmann::json::object();
    bool auto_fix = false;
    int max_attempts = 3;
    int attempts = 0;
    std::string auto_fix_set_by;
    bool auto_fix_stopped = false;
    std::string stopped_reason;
    std::string auto_fix_task_id;
    std::int64_t auto_fix_generation = 0; // the generation the running attempt is repairing
    std::vector<std::int64_t> attempted_generations;
    std::string repair_conversation_id;
    // The newest change conversation, whoever had it (kept for records written before lake#1240).
    std::string change_conversation_id;
    // Each person's own change conversation with the agent about this resource, by user id
    // (lake#1240): agent_turn continues only the requester's own conversation, so a second person
    // changing the same app has a thread of their own.
    std::map<std::string, std::string> change_conversations;
    std::int64_t revision = 0;
};

struct ResourceRepairCodec {
    using Value = ResourceRepair;
    static constexpr RecordCollection collection{"resource_repairs", 1};

    static std::string lookup(const ResourceRepair& repair);
    static nlohmann::json encode(const ResourceRepair& repair);
    static std::optional<ResourceRepair> decode(const StoredRecord& record);
};

inline constexpr int auto_fix_default_attempts = 3;
inline constexpr int auto_fix_most_attempts = 10;

std::string resource_records_lookup(const std::string& customer_id);
// `<kind>.<id>`, the key of a resource's repair record.
std::string resource_repair_key(std::string_view kind, std::string_view resource_id);
// `<event>.<kind>.<id>.<discriminator>` (the generation, or a build id).
std::string resource_notification_id(std::string_view event,
                                     std::string_view kind,
                                     std::string_view resource_id,
                                     std::string_view discriminator);

nlohmann::json resource_notification_json(const ResourceNotification& notification,
                                          const std::string& viewer_user_id);
// The change conversation `user` continues on this resource (lake#1240): their own, else the
// record's newest one from before per-person conversations when `theirs` says that conversation is
// that person's, else none (a new conversation starts).
std::string person_change_conversation(const ResourceRepair& repair,
                                       const std::string& user,
                                       const std::function<bool(const std::string&)>& theirs);

nlohmann::json resource_repair_json(const ResourceRepair& repair);

// Records a notification once: an id already present answers false, a new one true.
liquid::runtime::task<ZettaControlResult<bool>>
record_resource_notification(WorkspaceRecords& records, ResourceNotification notification);

// Every notification of the space in this workspace, newest first.
liquid::runtime::task<ZettaControlResult<std::vector<ResourceNotification>>>
list_resource_notifications(WorkspaceRecords& records, std::string customer_id);

// Applies `change` to one notification of the space, rereading on a conflict; `not_found` when
// this workspace holds none by that id.
liquid::runtime::task<ZettaControlResult<ResourceNotification>>
change_resource_notification(WorkspaceRecords& records,
                             std::string customer_id,
                             std::string id,
                             std::function<void(ResourceNotification&)> change);

// One resource's repair record; a default one (nothing recorded) when there is none.
liquid::runtime::task<ZettaControlResult<ResourceRepair>>
get_resource_repair(WorkspaceRecords& records,
                    std::string customer_id,
                    std::string kind,
                    std::string resource_id);

// Every repair record of the space in this workspace, of every kind.
liquid::runtime::task<ZettaControlResult<std::vector<ResourceRepair>>>
list_resource_repairs(WorkspaceRecords& records, std::string customer_id);

// Reads, changes and writes one resource's repair record (creating it), rereading on a conflict.
liquid::runtime::task<ZettaControlResult<ResourceRepair>>
change_resource_repair(WorkspaceRecords& records,
                       std::string customer_id,
                       std::string kind,
                       std::string resource_id,
                       std::function<void(ResourceRepair&)> change);

// Apps (resource kind `app`): their failure signals and the reasons said in plain words.

// Whether an app's observed state is a failed rollout: Degraded, Failed or Error, or a message
// that says the pod is crash-looping or its image cannot be pulled. A rollout still in progress
// (observed generation behind the desired one) and a stopped or deleted app are not failures.
bool app_rollout_failed(const WebApp& app);
// Whether the app is Ready at the generation it was asked for.
bool app_rollout_ready(const WebApp& app);
std::string app_failure_reason(std::string_view phase, std::string_view message);
std::string build_failure_reason(std::string_view message);

} // namespace lake::control
