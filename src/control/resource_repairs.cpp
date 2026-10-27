#include "lake/control/resource_repairs.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool mentions(std::string_view haystack, std::string_view needle) {
    return folded(haystack).contains(folded(needle));
}

std::vector<std::string> strings(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const auto& item : value) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

std::vector<std::int64_t> integers(const nlohmann::json& value) {
    std::vector<std::int64_t> out;
    if (value.is_array()) {
        for (const auto& item : value) {
            if (item.is_number_integer()) {
                out.push_back(item.get<std::int64_t>());
            }
        }
    }
    return out;
}

} // namespace

bool known_resource_kind(std::string_view kind) {
    return kind == resource_kind_app;
}

std::string resource_records_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string resource_repair_key(std::string_view kind, std::string_view resource_id) {
    return std::string(kind) + "." + std::string(resource_id);
}

std::string resource_notification_id(std::string_view event,
                                     std::string_view kind,
                                     std::string_view resource_id,
                                     std::string_view discriminator) {
    return std::string(event) + "." + std::string(kind) + "." + std::string(resource_id) + "." +
           std::string(discriminator);
}

std::string ResourceNotificationCodec::lookup(const ResourceNotification& notification) {
    return resource_records_lookup(notification.customer_id);
}

nlohmann::json ResourceNotificationCodec::encode(const ResourceNotification& notification) {
    return {{"customer_id", notification.customer_id},
            {"workspace_id", notification.workspace_id},
            {"resource_kind", notification.resource_kind},
            {"resource_id", notification.resource_id},
            {"resource_name", notification.resource_name},
            {"event", notification.event},
            {"reason", notification.reason},
            {"detail", notification.detail},
            {"phase", notification.phase},
            {"generation", notification.generation},
            {"build_id", notification.build_id},
            {"recipient_user_id", notification.recipient_user_id},
            {"resolved_at", notification.resolved_at},
            {"dismissed_by", notification.dismissed_by}};
}

std::optional<ResourceNotification> ResourceNotificationCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ResourceNotification value;
    value.id = record.id;
    value.customer_id = body.value("customer_id", std::string());
    value.workspace_id = body.value("workspace_id", std::string());
    value.resource_kind = body.value("resource_kind", std::string());
    value.resource_id = body.value("resource_id", std::string());
    value.resource_name = body.value("resource_name", std::string());
    value.event = body.value("event", std::string());
    value.reason = body.value("reason", std::string());
    value.detail = body.value("detail", std::string());
    value.phase = body.value("phase", std::string());
    value.generation = body.value("generation", std::int64_t{0});
    value.build_id = body.value("build_id", std::string());
    value.recipient_user_id = body.value("recipient_user_id", std::string());
    value.resolved_at = body.value("resolved_at", std::string());
    value.dismissed_by = strings(body.value("dismissed_by", nlohmann::json::array()));
    value.created_at = record_instant(record.at_us);
    value.at_us = record.at_us;
    value.revision = record.revision;
    if (value.customer_id.empty() || value.resource_kind.empty() || value.resource_id.empty() ||
        value.event.empty()) {
        return std::nullopt;
    }
    return value;
}

std::string ResourceRepairCodec::lookup(const ResourceRepair& repair) {
    return resource_records_lookup(repair.customer_id);
}

nlohmann::json ResourceRepairCodec::encode(const ResourceRepair& repair) {
    return {{"customer_id", repair.customer_id},
            {"resource_kind", repair.resource_kind},
            {"resource_id", repair.resource_id},
            {"deployed_by", repair.deployed_by},
            {"source", repair.source.is_object() ? repair.source : nlohmann::json::object()},
            {"auto_fix", repair.auto_fix},
            {"max_attempts", repair.max_attempts},
            {"attempts", repair.attempts},
            {"auto_fix_set_by", repair.auto_fix_set_by},
            {"auto_fix_stopped", repair.auto_fix_stopped},
            {"stopped_reason", repair.stopped_reason},
            {"auto_fix_task_id", repair.auto_fix_task_id},
            {"auto_fix_generation", repair.auto_fix_generation},
            {"attempted_generations", repair.attempted_generations},
            {"repair_conversation_id", repair.repair_conversation_id},
            {"change_conversation_id", repair.change_conversation_id},
            {"change_conversations", repair.change_conversations}};
}

std::optional<ResourceRepair> ResourceRepairCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ResourceRepair value;
    value.resource_kind = body.value("resource_kind", std::string());
    value.resource_id = body.value("resource_id", std::string());
    value.customer_id = body.value("customer_id", std::string());
    value.deployed_by = body.value("deployed_by", std::string());
    value.source = body.value("source", nlohmann::json::object());
    value.auto_fix = body.value("auto_fix", false);
    value.max_attempts = body.value("max_attempts", auto_fix_default_attempts);
    value.attempts = body.value("attempts", 0);
    value.auto_fix_set_by = body.value("auto_fix_set_by", std::string());
    value.auto_fix_stopped = body.value("auto_fix_stopped", false);
    value.stopped_reason = body.value("stopped_reason", std::string());
    value.auto_fix_task_id = body.value("auto_fix_task_id", std::string());
    value.auto_fix_generation = body.value("auto_fix_generation", std::int64_t{0});
    value.attempted_generations =
        integers(body.value("attempted_generations", nlohmann::json::array()));
    value.repair_conversation_id = body.value("repair_conversation_id", std::string());
    value.change_conversation_id = body.value("change_conversation_id", std::string());
    if (const auto found = body.find("change_conversations");
        found != body.end() && found->is_object()) {
        for (const auto& [user, conversation] : found->items()) {
            if (conversation.is_string()) {
                value.change_conversations[user] = conversation.get<std::string>();
            }
        }
    }
    value.revision = record.revision;
    if (value.customer_id.empty() || value.resource_kind.empty() || value.resource_id.empty() ||
        record.id != resource_repair_key(value.resource_kind, value.resource_id)) {
        return std::nullopt;
    }
    return value;
}

nlohmann::json resource_notification_json(const ResourceNotification& notification,
                                          const std::string& viewer_user_id) {
    const bool dismissed =
        !viewer_user_id.empty() && std::ranges::find(notification.dismissed_by, viewer_user_id) !=
                                       notification.dismissed_by.end();
    nlohmann::json value{{"id", notification.id},
                         {"workspace_id", notification.workspace_id},
                         {"resource_kind", notification.resource_kind},
                         {"resource_id", notification.resource_id},
                         {"resource_name", notification.resource_name},
                         {"event", notification.event},
                         {"reason", notification.reason},
                         {"detail", notification.detail},
                         {"phase", notification.phase},
                         {"generation", notification.generation},
                         {"created_at", notification.created_at},
                         {"for_you", !viewer_user_id.empty() &&
                                         notification.recipient_user_id == viewer_user_id},
                         {"recipient_user_id", notification.recipient_user_id},
                         {"resolved", !notification.resolved_at.empty()},
                         {"dismissed", dismissed}};
    if (!notification.build_id.empty()) {
        value["build_id"] = notification.build_id;
    }
    if (!notification.resolved_at.empty()) {
        value["resolved_at"] = notification.resolved_at;
    }
    return value;
}

std::string person_change_conversation(const ResourceRepair& repair,
                                       const std::string& user,
                                       const std::function<bool(const std::string&)>& theirs) {
    if (const auto found = repair.change_conversations.find(user);
        found != repair.change_conversations.end() && !found->second.empty()) {
        return found->second;
    }
    if (!repair.change_conversation_id.empty() && repair.change_conversations.empty() && theirs &&
        theirs(repair.change_conversation_id)) {
        return repair.change_conversation_id;
    }
    return {};
}

nlohmann::json resource_repair_json(const ResourceRepair& repair) {
    nlohmann::json value{
        {"resource_kind", repair.resource_kind},
        {"resource_id", repair.resource_id},
        {"auto_fix", repair.auto_fix},
        {"max_attempts", repair.max_attempts},
        {"attempts", repair.attempts},
        {"auto_fix_stopped", repair.auto_fix_stopped},
        {"deployed_by", repair.deployed_by},
        {"source", repair.source.is_object() ? repair.source : nlohmann::json::object()}};
    if (!repair.stopped_reason.empty()) {
        value["stopped_reason"] = repair.stopped_reason;
    }
    if (!repair.auto_fix_task_id.empty()) {
        value["auto_fix_task_id"] = repair.auto_fix_task_id;
    }
    if (!repair.repair_conversation_id.empty()) {
        value["repair_conversation_id"] = repair.repair_conversation_id;
    }
    if (!repair.change_conversation_id.empty()) {
        value["change_conversation_id"] = repair.change_conversation_id;
    }
    return value;
}

liquid::runtime::task<ZettaControlResult<bool>>
record_resource_notification(WorkspaceRecords& records, ResourceNotification notification) {
    if (notification.customer_id.empty() || notification.resource_kind.empty() ||
        notification.resource_id.empty() || notification.event.empty() ||
        notification.id.empty()) {
        co_return refused<bool>("invalid_input", "a notification is incomplete");
    }
    const std::string id = notification.id;
    auto stored = settled(co_await records.append(ResourceNotificationCodec::collection,
                                                  ResourceNotificationCodec::lookup(notification),
                                                  ResourceNotificationCodec::encode(notification),
                                                  RecordWrite{.id = id}));
    if (!stored) {
        if (stored.error().code == "conflict") {
            co_return ZettaControlResult<bool>(false);
        }
        co_return ZettaControlResult<bool>(std::unexpected(stored.error()));
    }
    co_return ZettaControlResult<bool>(true);
}

liquid::runtime::task<ZettaControlResult<std::vector<ResourceNotification>>>
list_resource_notifications(WorkspaceRecords& records, std::string customer_id) {
    using Rows = std::vector<ResourceNotification>;
    RecordSet<ResourceNotificationCodec> set(records);
    auto listed = settled(co_await set.list(resource_records_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<Rows>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed,
                  [&](const ResourceNotification& row) { return row.customer_id != customer_id; });
    std::ranges::sort(*listed, [](const ResourceNotification& a, const ResourceNotification& b) {
        return a.at_us != b.at_us ? a.at_us > b.at_us : a.id > b.id;
    });
    co_return ZettaControlResult<Rows>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<ResourceNotification>>
change_resource_notification(WorkspaceRecords& records,
                             std::string customer_id,
                             std::string id,
                             std::function<void(ResourceNotification&)> change) {
    bool missing = false;
    auto written = settled(co_await records.update(
        ResourceNotificationCodec::collection,
        id,
        resource_records_lookup(customer_id),
        [&](const std::optional<StoredRecord>& current) -> std::optional<nlohmann::json> {
            missing = false;
            std::optional<ResourceNotification> value;
            if (current) {
                value = ResourceNotificationCodec::decode(*current);
            }
            if (!value || value->customer_id != customer_id) {
                missing = true;
                // Absent: nothing is written. Another space's record is written back unchanged,
                // never deleted (update deletes a record whose change answers nothing).
                return current ? std::optional<nlohmann::json>(current->body) : std::nullopt;
            }
            change(*value);
            return ResourceNotificationCodec::encode(*value);
        }));
    if (!written) {
        co_return ZettaControlResult<ResourceNotification>(std::unexpected(written.error()));
    }
    if (missing || !*written) {
        co_return refused<ResourceNotification>("not_found",
                                                "no such notification in this workspace");
    }
    auto decoded = ResourceNotificationCodec::decode(**written);
    if (!decoded) {
        co_return refused<ResourceNotification>("store_error", "the stored record did not decode");
    }
    co_return ZettaControlResult<ResourceNotification>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<ResourceRepair>>
get_resource_repair(WorkspaceRecords& records,
                    std::string customer_id,
                    std::string kind,
                    std::string resource_id) {
    ResourceRepair fresh;
    fresh.resource_kind = kind;
    fresh.resource_id = resource_id;
    fresh.customer_id = customer_id;
    auto got = settled(co_await records.get(ResourceRepairCodec::collection,
                                            resource_repair_key(kind, resource_id)));
    if (!got) {
        if (got.error().code == "not_found" || got.error().code == "invalid_input") {
            co_return ZettaControlResult<ResourceRepair>(std::move(fresh));
        }
        co_return ZettaControlResult<ResourceRepair>(std::unexpected(got.error()));
    }
    if (*got) {
        if (auto decoded = ResourceRepairCodec::decode(**got);
            decoded && decoded->customer_id == customer_id) {
            co_return ZettaControlResult<ResourceRepair>(std::move(*decoded));
        }
    }
    co_return ZettaControlResult<ResourceRepair>(std::move(fresh));
}

liquid::runtime::task<ZettaControlResult<std::vector<ResourceRepair>>>
list_resource_repairs(WorkspaceRecords& records, std::string customer_id) {
    using Rows = std::vector<ResourceRepair>;
    RecordSet<ResourceRepairCodec> set(records);
    auto listed = settled(co_await set.list(resource_records_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<Rows>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed,
                  [&](const ResourceRepair& row) { return row.customer_id != customer_id; });
    co_return ZettaControlResult<Rows>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<ResourceRepair>>
change_resource_repair(WorkspaceRecords& records,
                       std::string customer_id,
                       std::string kind,
                       std::string resource_id,
                       std::function<void(ResourceRepair&)> change) {
    bool foreign = false;
    auto written = settled(co_await records.update(
        ResourceRepairCodec::collection,
        resource_repair_key(kind, resource_id),
        resource_records_lookup(customer_id),
        [&](const std::optional<StoredRecord>& current) -> std::optional<nlohmann::json> {
            foreign = false;
            ResourceRepair value;
            value.resource_kind = kind;
            value.resource_id = resource_id;
            value.customer_id = customer_id;
            if (current) {
                auto decoded = ResourceRepairCodec::decode(*current);
                if (decoded && decoded->customer_id != customer_id) {
                    foreign = true;
                    return std::optional<nlohmann::json>(current->body);
                }
                if (decoded) {
                    value = std::move(*decoded);
                }
            }
            change(value);
            return ResourceRepairCodec::encode(value);
        }));
    if (!written) {
        co_return ZettaControlResult<ResourceRepair>(std::unexpected(written.error()));
    }
    if (foreign || !*written) {
        co_return refused<ResourceRepair>("not_found", "no such resource in this workspace");
    }
    auto decoded = ResourceRepairCodec::decode(**written);
    if (!decoded) {
        co_return refused<ResourceRepair>("store_error", "the stored record did not decode");
    }
    co_return ZettaControlResult<ResourceRepair>(std::move(*decoded));
}

bool app_rollout_failed(const WebApp& app) {
    if (app.desired != "running" || app.observed_generation < app.generation) {
        return false;
    }
    const std::string phase = folded(app.phase);
    if (phase == "degraded" || phase == "failed" || phase == "error") {
        return true;
    }
    return !app.ready && (mentions(app.message, "CrashLoopBackOff") ||
                          mentions(app.message, "ImagePullBackOff") ||
                          mentions(app.message, "ErrImagePull"));
}

bool app_rollout_ready(const WebApp& app) {
    return app.desired == "running" && app.ready && app.observed_generation >= app.generation;
}

std::string app_failure_reason(std::string_view phase, std::string_view message) {
    if (mentions(message, "CrashLoopBackOff") || mentions(message, "back-off restarting")) {
        return "The app starts and then stops again, over and over. This usually means it fails "
               "while starting: an error in its code, a missing setting or secret, or it listens "
               "on a different port than the one Lake sends traffic to.";
    }
    if (mentions(message, "ImagePullBackOff") || mentions(message, "ErrImagePull")) {
        return "Lake cannot fetch the app's image, so the app never started.";
    }
    if (mentions(message, "OOMKilled") || mentions(message, "out of memory")) {
        return "The app ran out of memory and was stopped.";
    }
    if (mentions(message, "CreateContainerConfigError")) {
        return "The app's settings could not be applied, so it never started. A secret or "
               "setting it needs is often missing.";
    }
    if (mentions(message, "probe") || mentions(message, "readiness")) {
        return "The app is running but does not answer on its port, so it never became ready.";
    }
    if (folded(phase) == "failed" || folded(phase) == "error") {
        return "The app's rollout failed, so the new version is not running.";
    }
    return "The app was deployed but is not running correctly.";
}

std::string build_failure_reason(std::string_view message) {
    if (mentions(message, "pip") || mentions(message, "requirement") ||
        mentions(message, "dependenc")) {
        return "The app's image could not be built: one of its packages could not be installed, "
               "so it was not deployed.";
    }
    return "The app's image could not be built, so it was not deployed.";
}

} // namespace lake::control
