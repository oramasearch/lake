#include "lake/control/repository_records.hpp"

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

ZettaControlResult<RepositorySettings> decoded_or_error(const StoredRecord& record) {
    auto decoded = RepositorySettingsCodec::decode(record);
    if (!decoded) {
        return failed<RepositorySettings>(
            ZettaControlError{"store_error", "the stored repository settings did not decode"});
    }
    return ZettaControlResult<RepositorySettings>(std::move(*decoded));
}

} // namespace

std::string repository_settings_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string RepositorySettingsCodec::lookup(const RepositorySettings& settings) {
    return repository_settings_lookup(settings.customer_id);
}

nlohmann::json RepositorySettingsCodec::encode(const RepositorySettings& settings) {
    return {{"repository_id", settings.repository_id},
            {"customer_id", settings.customer_id},
            {"description", settings.description},
            {"updated_at", settings.updated_at}};
}

std::optional<RepositorySettings> RepositorySettingsCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far. Later settings are added fields (missing ones
    // default here); a change that is not additive raises the version and decodes this one.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    RepositorySettings settings;
    settings.repository_id = body.value("repository_id", std::string());
    settings.customer_id = body.value("customer_id", std::string());
    settings.description = body.value("description", std::string());
    // Keyed by its repository: a body under another key is not this repository's.
    if (settings.repository_id.empty() || settings.repository_id != record.id ||
        settings.customer_id.empty()) {
        return std::nullopt;
    }
    settings.at_us = record.at_us;
    settings.created_at = record_instant(record.at_us);
    settings.updated_at = body.value("updated_at", settings.created_at);
    settings.revision = record.revision;
    settings.lake_revision = record.lake_revision;
    return settings;
}

liquid::runtime::task<ZettaControlResult<std::optional<RepositorySettings>>>
get_repository_settings(WorkspaceRecords& records, std::string repository_id,
                        std::string customer_id) {
    using Found = std::optional<RepositorySettings>;
    auto stored = settled(co_await records.get(RepositorySettingsCodec::collection, repository_id));
    // Every failure is reported, an invalid id or a workspace that is gone included: absence is
    // only what the workspace says is absent.
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto settings = RepositorySettingsCodec::decode(**stored);
    if (!settings || settings->repository_id != repository_id ||
        (!customer_id.empty() && settings->customer_id != customer_id)) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(settings));
}

liquid::runtime::task<ZettaControlResult<RepositorySettings>>
create_repository_settings(WorkspaceRecords& records, RepositorySettings settings) {
    const std::int64_t at_us = settings.at_us > 0 ? settings.at_us : now_us();
    if (settings.updated_at.empty()) {
        settings.updated_at = record_instant(at_us);
    }
    auto stored = settled(co_await records.put(
        RepositorySettingsCodec::collection, settings.repository_id,
        RepositorySettingsCodec::lookup(settings), RepositorySettingsCodec::encode(settings),
        RecordCondition{.if_absent = true}, 1, at_us));
    if (!stored) {
        co_return failed<RepositorySettings>(stored.error());
    }
    co_return decoded_or_error(*stored);
}

liquid::runtime::task<ZettaControlResult<RepositorySettings>>
update_repository_settings(WorkspaceRecords& records, std::string repository_id,
                           std::string customer_id, RepositorySettingsChange change,
                           int attempts) {
    ZettaControlError last{"conflict", "the repository settings kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto found = settled(co_await get_repository_settings(records, repository_id, customer_id));
        if (!found) {
            co_return failed<RepositorySettings>(found.error());
        }
        const bool absent = !*found;
        RepositorySettings next = absent ? RepositorySettings{} : std::move(**found);
        change(next);
        next.repository_id = repository_id;
        next.customer_id = customer_id;
        const std::int64_t now = now_us();
        next.updated_at = record_instant(now);
        auto written = settled(co_await records.put(
            RepositorySettingsCodec::collection, repository_id,
            RepositorySettingsCodec::lookup(next), RepositorySettingsCodec::encode(next),
            absent ? RecordCondition{.if_absent = true}
                   : RecordCondition{.if_revision = next.revision},
            absent ? 1 : next.lake_revision + 1, absent ? now : next.at_us));
        if (written) {
            co_return decoded_or_error(*written);
        }
        if (written.error().code != "conflict") {
            co_return failed<RepositorySettings>(written.error());
        }
        last = written.error(); // another change won: reread and apply this one again
    }
    co_return failed<RepositorySettings>(last);
}

liquid::runtime::task<ZettaControlResult<std::vector<RepositorySettings>>>
list_repository_settings(WorkspaceRecords& records, std::string customer_id) {
    using Settings = std::vector<RepositorySettings>;
    auto stored = settled(co_await records.list(RepositorySettingsCodec::collection,
                                                repository_settings_lookup(customer_id)));
    if (!stored) {
        co_return failed<Settings>(stored.error());
    }
    Settings settings;
    for (const StoredRecord& record : *stored) {
        auto decoded = RepositorySettingsCodec::decode(record);
        if (decoded && decoded->customer_id == customer_id) {
            settings.push_back(std::move(*decoded));
        }
    }
    co_return ZettaControlResult<Settings>(std::move(settings));
}

} // namespace lake::control
