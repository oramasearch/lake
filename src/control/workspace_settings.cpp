#include "lake/control/workspace_settings.hpp"

#include <chrono>
#include <utility>

namespace lake::control {
namespace {

using json = nlohmann::json;

std::string text(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string settings_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

} // namespace

std::string WorkspaceSettingsCodec::lookup(const WorkspaceSettings& settings) {
    return settings_lookup(settings.customer_id);
}

json WorkspaceSettingsCodec::encode(const WorkspaceSettings& settings) {
    return {{"customer_id", settings.customer_id},
            {"agent_model", settings.agent_model},
            {"set_by", settings.set_by},
            {"set_by_identity", settings.set_by_identity},
            {"updated_at", settings.updated_at}};
}

std::optional<WorkspaceSettings> WorkspaceSettingsCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    WorkspaceSettings settings;
    settings.customer_id = text(record.body, "customer_id");
    settings.agent_model = text(record.body, "agent_model");
    settings.set_by = text(record.body, "set_by");
    settings.set_by_identity = text(record.body, "set_by_identity");
    settings.updated_at = text(record.body, "updated_at");
    settings.at_us = record.at_us;
    if (settings.customer_id.empty()) {
        return std::nullopt;
    }
    return settings;
}

liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceSettings>>>
get_workspace_settings(WorkspaceRecords& records, std::string customer_id) {
    using Result = ZettaControlResult<std::optional<WorkspaceSettings>>;
    auto got = settled(co_await records.get(WorkspaceSettingsCodec::collection,
                                            std::string(workspace_settings_key)));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    if (!*got) {
        co_return Result(std::optional<WorkspaceSettings>());
    }
    auto settings = WorkspaceSettingsCodec::decode(**got);
    if (!settings || settings->customer_id != customer_id) {
        co_return Result(std::optional<WorkspaceSettings>());
    }
    co_return Result(std::move(settings));
}

liquid::runtime::task<ZettaControlResult<WorkspaceSettings>>
put_workspace_settings(WorkspaceRecords& records, WorkspaceSettings settings) {
    using Result = ZettaControlResult<WorkspaceSettings>;
    if (settings.customer_id.empty()) {
        co_return Result(std::unexpected(
            ZettaControlError{"invalid_input", "workspace settings name their account"}));
    }
    // The record keeps its first instant across updates, so the change's own instant is in the
    // body.
    settings.updated_at = record_instant(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
    const json body = WorkspaceSettingsCodec::encode(settings);
    auto written = settled(
        co_await records.update(WorkspaceSettingsCodec::collection,
                                std::string(workspace_settings_key),
                                WorkspaceSettingsCodec::lookup(settings),
                                [&](const std::optional<StoredRecord>&) -> std::optional<json> {
                                    return std::optional<json>(std::in_place, body);
                                }));
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    if (!*written) {
        co_return Result(std::unexpected(
            ZettaControlError{"store_error", "the workspace's settings were not written"}));
    }
    auto decoded = WorkspaceSettingsCodec::decode(**written);
    if (!decoded) {
        co_return Result(std::unexpected(
            ZettaControlError{"store_error", "the stored settings did not decode"}));
    }
    co_return Result(std::move(*decoded));
}

} // namespace lake::control
