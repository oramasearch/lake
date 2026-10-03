#include "lake/control/application_settings_records.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<std::string> strings(const nlohmann::json& body, const char* field) {
    std::vector<std::string> values;
    const auto found = body.find(field);
    if (found != body.end() && found->is_array()) {
        for (const nlohmann::json& value : *found) {
            if (value.is_string()) {
                values.push_back(value.get<std::string>());
            }
        }
    }
    return values;
}

} // namespace

std::string application_settings_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string ApplicationSettingsCodec::lookup(const ApplicationSettings& settings) {
    return application_settings_lookup(settings.customer_id);
}

nlohmann::json ApplicationSettingsCodec::encode(const ApplicationSettings& settings) {
    return {{"application_id", settings.application_id},
            {"customer_id", settings.customer_id},
            {"name", settings.name},
            {"allowed_origins", settings.allowed_origins},
            {"redirect_uris", settings.redirect_uris},
            {"theme", settings.theme.is_object() ? settings.theme : nlohmann::json::object()},
            {"updated_at", settings.updated_at}};
}

std::optional<ApplicationSettings> ApplicationSettingsCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ApplicationSettings settings;
    settings.application_id = body.value("application_id", std::string());
    settings.customer_id = body.value("customer_id", std::string());
    settings.name = body.value("name", std::string());
    settings.allowed_origins = strings(body, "allowed_origins");
    settings.redirect_uris = strings(body, "redirect_uris");
    settings.theme = body.value("theme", nlohmann::json::object());
    // Keyed by its application: a body under another key is not this application's.
    if (settings.application_id.empty() || settings.application_id != record.id ||
        settings.customer_id.empty() || settings.name.empty() ||
        settings.allowed_origins.empty() || !settings.theme.is_object()) {
        return std::nullopt;
    }
    settings.at_us = record.at_us;
    settings.created_at = record_instant(record.at_us);
    settings.updated_at = body.value("updated_at", settings.created_at);
    settings.revision = record.revision;
    settings.lake_revision = record.lake_revision;
    return settings;
}

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationSettings>>>
get_application_settings(WorkspaceRecords& records, std::string application_id,
                         std::string customer_id) {
    using Found = std::optional<ApplicationSettings>;
    auto stored = settled(co_await records.get(ApplicationSettingsCodec::collection, application_id));
    // Every failure is reported, an invalid id or a workspace that is gone included: absence is
    // only what the workspace says is absent.
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto settings = ApplicationSettingsCodec::decode(**stored);
    if (!settings || settings->application_id != application_id ||
        (!customer_id.empty() && settings->customer_id != customer_id)) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(settings));
}

liquid::runtime::task<ZettaControlResult<ApplicationSettings>>
create_application_settings(WorkspaceRecords& records, ApplicationSettings settings) {
    const std::int64_t at_us = settings.at_us > 0 ? settings.at_us : now_us();
    if (settings.updated_at.empty()) {
        settings.updated_at = record_instant(at_us);
    }
    auto stored = settled(co_await records.put(
        ApplicationSettingsCodec::collection, settings.application_id,
        ApplicationSettingsCodec::lookup(settings), ApplicationSettingsCodec::encode(settings),
        RecordCondition{.if_absent = true}, 1, at_us));
    if (!stored) {
        co_return failed<ApplicationSettings>(stored.error());
    }
    auto decoded = ApplicationSettingsCodec::decode(*stored);
    if (!decoded) {
        co_return failed<ApplicationSettings>(
            ZettaControlError{"store_error", "the stored application settings did not decode"});
    }
    co_return ZettaControlResult<ApplicationSettings>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<ApplicationSettings>>
update_application_settings(WorkspaceRecords& records, std::string application_id,
                            std::string customer_id, ApplicationSettingsChange change,
                            int attempts) {
    ZettaControlError last{"conflict", "the application settings kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto found =
            settled(co_await get_application_settings(records, application_id, customer_id));
        if (!found) {
            co_return failed<ApplicationSettings>(found.error());
        }
        if (!*found) {
            co_return failed<ApplicationSettings>(ZettaControlError{
                "not_found", "application has no settings in its workspace"});
        }
        ApplicationSettings next = std::move(**found);
        change(next);
        next.application_id = application_id;
        next.customer_id = customer_id;
        next.updated_at = record_instant(now_us());
        auto written = settled(co_await records.put(
            ApplicationSettingsCodec::collection, application_id,
            ApplicationSettingsCodec::lookup(next), ApplicationSettingsCodec::encode(next),
            RecordCondition{.if_revision = next.revision}, next.lake_revision + 1, next.at_us));
        if (written) {
            auto decoded = ApplicationSettingsCodec::decode(*written);
            if (!decoded) {
                co_return failed<ApplicationSettings>(ZettaControlError{
                    "store_error", "the stored application settings did not decode"});
            }
            co_return ZettaControlResult<ApplicationSettings>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return failed<ApplicationSettings>(written.error());
        }
        last = written.error(); // another change won: reread and apply this one again
    }
    co_return failed<ApplicationSettings>(last);
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationSettings>>>
list_application_settings(WorkspaceRecords& records, std::string customer_id) {
    using Settings = std::vector<ApplicationSettings>;
    auto stored = settled(co_await records.list(ApplicationSettingsCodec::collection,
                                                application_settings_lookup(customer_id)));
    if (!stored) {
        co_return failed<Settings>(stored.error());
    }
    Settings settings;
    for (const StoredRecord& record : *stored) {
        auto decoded = ApplicationSettingsCodec::decode(record);
        if (decoded && decoded->customer_id == customer_id) {
            settings.push_back(std::move(*decoded));
        }
    }
    co_return ZettaControlResult<Settings>(std::move(settings));
}

} // namespace lake::control
