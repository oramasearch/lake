#include "lake/control/platform_records.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <tuple>

#include <openssl/evp.h>

namespace lake::control {
namespace {

using nlohmann::json;

std::string md5_uuid(const std::string& joined) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(joined.data(), joined.size(), digest.data(), &size, EVP_md5(), nullptr) != 1 ||
        size != 16) {
        return {};
    }
    std::string id;
    for (unsigned int index = 0; index < size; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            id += '-';
        }
        id += std::format("{:02x}", static_cast<unsigned>(digest[index]));
    }
    return id;
}

std::string text_of(const json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || found->is_null()) {
        return {};
    }
    return found->is_string() ? found->get<std::string>() : found->dump();
}

std::string normalized_updated_at(const json& value) {
    std::string updated_at = text_of(value, "updated_at");
    if (updated_at.ends_with('Z') && updated_at.find('.') == std::string::npos) {
        updated_at.insert(updated_at.size() - 1, ".000000");
    }
    return updated_at;
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

template <class T> ZettaControlResult<T> failure(std::string code, std::string message) {
    return ZettaControlResult<T>(
        std::unexpected(ZettaControlError{std::move(code), std::move(message)}));
}

// An environment record that opened here, sits under its own derived key and names `repository`.
std::optional<PlatformEnvironment> environment_of(const StoredRecord& record,
                                                  const std::string& repository_id) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    PlatformEnvironment environment{record.id,
                                    record.body.value("repository_id", std::string()),
                                    record.body.value("name", std::string()),
                                    record.body.value("object_id", std::string()),
                                    record_instant(record.at_us),
                                    record.at_us};
    if (environment.repository_id != repository_id || environment.name.empty() ||
        environment.object_id.empty() ||
        record.id != platform_environment_id(repository_id, environment.name)) {
        return std::nullopt;
    }
    return environment;
}

// A mirror record that opened here, sits under its own key and names `repository`.
std::optional<PlatformMirrorRow> mirror_of(const StoredRecord& record,
                                           const std::string& repository_id) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    PlatformMirrorRow row{record.body.value("kind", std::string()),
                          record.body.value("key", std::string()),
                          record.body.value("value", json::object()),
                          record.body.value("seen_at", std::string()),
                          record.revision,
                          record.lake_revision};
    if (!is_platform_mirror_kind(row.kind) || row.key.empty() || !row.value.is_object() ||
        text_of(row.value, "repository") != repository_id ||
        record.id != platform_mirror_id(row.kind, row.key)) {
        return std::nullopt;
    }
    return row;
}

} // namespace

std::string platform_environment_id(const std::string& repository_id, const std::string& name) {
    return md5_uuid(repository_id + "\n" + name);
}

std::string platform_mirror_id(const std::string& kind, const std::string& key) {
    return md5_uuid(kind + "\n" + key);
}

std::string platform_record_lookup(const std::string& repository_id) {
    return "repository:" + repository_id;
}

bool is_platform_mirror_kind(std::string_view kind) {
    return kind == "deployment" || kind == "environment_env" || kind == "domain";
}

std::tuple<std::string, std::string, std::string> platform_row_order(const json& row) {
    return {normalized_updated_at(row), text_of(row, "ordering_token"), row.dump()};
}

bool platform_row_is_newer(const json& candidate, const json& current) {
    return platform_row_order(current) < platform_row_order(candidate);
}

liquid::runtime::task<ZettaControlResult<std::vector<PlatformEnvironment>>>
list_platform_environment_records(WorkspaceRecords& records, std::string repository_id) {
    using Environments = std::vector<PlatformEnvironment>;
    auto stored = settled(co_await records.list(PlatformEnvironmentCodec::collection,
                                                platform_record_lookup(repository_id)));
    if (!stored) {
        co_return ZettaControlResult<Environments>(std::unexpected(stored.error()));
    }
    Environments environments;
    for (const StoredRecord& record : *stored) {
        if (auto environment = environment_of(record, repository_id)) {
            environments.push_back(std::move(*environment));
        }
    }
    std::ranges::sort(environments, {}, &PlatformEnvironment::name);
    co_return ZettaControlResult<Environments>(std::move(environments));
}

liquid::runtime::task<ZettaControlResult<std::optional<PlatformEnvironment>>>
get_platform_environment_record(WorkspaceRecords& records, std::string repository_id,
                                std::string name) {
    using Found = std::optional<PlatformEnvironment>;
    const std::string id = platform_environment_id(repository_id, name);
    if (id.empty() || name.empty()) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto stored = settled(co_await records.get(PlatformEnvironmentCodec::collection, id));
    if (!stored) {
        co_return ZettaControlResult<Found>(std::unexpected(stored.error()));
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(environment_of(**stored, repository_id));
}

liquid::runtime::task<ZettaControlResult<PlatformEnvironment>>
put_platform_environment_record(WorkspaceRecords& records, PlatformEnvironment environment) {
    using Result = ZettaControlResult<PlatformEnvironment>;
    if (environment.repository_id.empty() || environment.name.empty() ||
        environment.object_id.empty()) {
        co_return failure<PlatformEnvironment>("invalid_input",
                                               "an environment needs its repository, name and "
                                               "policy object");
    }
    environment.id = platform_environment_id(environment.repository_id, environment.name);
    const std::int64_t at_us = environment.at_us > 0 ? environment.at_us : now_us();
    auto written = settled(co_await records.put(PlatformEnvironmentCodec::collection,
                                                environment.id,
                                                platform_record_lookup(environment.repository_id),
                                                {{"repository_id", environment.repository_id},
                                                 {"name", environment.name},
                                                 {"object_id", environment.object_id}},
                                                RecordCondition{.if_absent = true},
                                                1,
                                                at_us));
    if (written) {
        auto stored = environment_of(*written, environment.repository_id);
        if (!stored) {
            co_return failure<PlatformEnvironment>("store_error",
                                                   "the stored environment did not decode");
        }
        co_return Result(std::move(*stored));
    }
    if (written.error().code != "conflict") {
        co_return Result(std::unexpected(written.error()));
    }
    // Another writer created it first: answer theirs.
    auto existing = settled(co_await get_platform_environment_record(
        records, environment.repository_id, environment.name));
    if (!existing) {
        co_return Result(std::unexpected(existing.error()));
    }
    if (!*existing) {
        co_return failure<PlatformEnvironment>(
            "conflict", "the environment's key holds a record that does not open here");
    }
    co_return Result(std::move(**existing));
}

liquid::runtime::task<ZettaControlResult<std::vector<PlatformMirrorRow>>>
list_platform_mirror_records(WorkspaceRecords& records, std::string repository_id) {
    using Rows = std::vector<PlatformMirrorRow>;
    auto stored = settled(co_await records.list(PlatformMirrorCodec::collection,
                                                platform_record_lookup(repository_id)));
    if (!stored) {
        co_return ZettaControlResult<Rows>(std::unexpected(stored.error()));
    }
    Rows rows;
    for (const StoredRecord& record : *stored) {
        if (auto row = mirror_of(record, repository_id)) {
            rows.push_back(std::move(*row));
        }
    }
    co_return ZettaControlResult<Rows>(std::move(rows));
}

liquid::runtime::task<ZettaControlResult<bool>>
put_platform_mirror_record(WorkspaceRecords& records, std::string repository_id,
                           PlatformMirrorRow row, std::int64_t at_us, int attempts) {
    if (!is_platform_mirror_kind(row.kind) || row.key.empty() || !row.value.is_object() ||
        repository_id.empty() || text_of(row.value, "repository") != repository_id) {
        co_return failure<bool>("invalid_input", "platform mirror row is incomplete");
    }
    const auto collection = PlatformMirrorCodec::collection;
    const std::string id = platform_mirror_id(row.kind, row.key);
    const json body{{"kind", row.kind}, {"key", row.key}, {"value", row.value},
                    {"seen_at", row.seen_at}};
    ZettaControlError last{"conflict", "the mirror row kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto current = settled(co_await records.get(collection, id));
        if (!current) {
            co_return ZettaControlResult<bool>(std::unexpected(current.error()));
        }
        RecordCondition condition;
        std::int64_t lake_revision = 1;
        if (*current) {
            const auto existing = mirror_of(**current, repository_id);
            if (!existing) {
                co_return failure<bool>("conflict", "the mirror row's key holds a record that "
                                                    "does not open here");
            }
            if (existing->value == row.value || !platform_row_is_newer(row.value, existing->value)) {
                co_return ZettaControlResult<bool>(false); // it holds this row or a newer one
            }
            condition.if_revision = (*current)->revision;
            lake_revision = (*current)->lake_revision + 1;
        } else {
            condition.if_absent = true;
        }
        auto written = settled(co_await records.put(collection, id,
                                                    platform_record_lookup(repository_id), body,
                                                    condition, lake_revision,
                                                    at_us > 0 ? at_us : now_us()));
        if (written) {
            co_return ZettaControlResult<bool>(true);
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<bool>(std::unexpected(written.error()));
        }
        last = written.error(); // another replica wrote first: reread and compare again
    }
    co_return ZettaControlResult<bool>(std::unexpected(last));
}

} // namespace lake::control
