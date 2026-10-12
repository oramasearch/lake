#include "lake/control/metering.hpp"

#include <array>
#include <chrono>
#include <format>
#include <limits>

#include <openssl/evp.h>

namespace lake::control {
namespace {

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

std::string lookup_of(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::uint64_t unsigned_field(const nlohmann::json& body, const char* name) {
    const auto found = body.find(name);
    if (found == body.end() || !found->is_number_unsigned()) {
        return 0;
    }
    return found->get<std::uint64_t>();
}

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

template <class T> ZettaControlResult<T> failure(std::string code, std::string message) {
    return ZettaControlResult<T>(
        std::unexpected(ZettaControlError{std::move(code), std::move(message)}));
}

// A transfer record that opened here and names this space and a valid hour.
std::optional<MeteredTransfer> transfer_of(const StoredRecord& record,
                                           const std::string& customer_id) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        record.body.value("customer_id", std::string()) != customer_id) {
        return std::nullopt;
    }
    MeteredTransfer transfer{record.id,
                             customer_id,
                             record.body.value("hour", std::string()),
                             unsigned_field(record.body, "transfer_in_bytes"),
                             unsigned_field(record.body, "transfer_out_bytes")};
    if (!metering_hour_us(transfer.hour)) {
        return std::nullopt;
    }
    return transfer;
}

std::optional<MeteredBuild> build_of(const StoredRecord& record, const std::string& customer_id) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        record.body.value("customer_id", std::string()) != customer_id) {
        return std::nullopt;
    }
    MeteredBuild build{customer_id,
                       record.body.value("result_id", std::string()),
                       record.body.value("hour", std::string()),
                       unsigned_field(record.body, "compute_seconds"),
                       record.body.value("repository_id", std::string())};
    if (build.result_id.empty() || !metering_hour_us(build.hour)) {
        return std::nullopt;
    }
    return build;
}

std::optional<MeteredJob> job_of(const StoredRecord& record, const std::string& customer_id) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        record.body.value("customer_id", std::string()) != customer_id) {
        return std::nullopt;
    }
    MeteredJob job{customer_id,
                   record.body.value("kind", std::string()),
                   record.body.value("object_id", std::string()),
                   record.body.value("hour", std::string()),
                   unsigned_field(record.body, "seconds")};
    if (job.kind.empty() || job.object_id.empty() || !metering_hour_us(job.hour)) {
        return std::nullopt;
    }
    return job;
}

std::string job_key(const MeteredJob& job) {
    return job.kind + ":" + job.object_id;
}

std::optional<MeteredAppHour> app_hour_of(const StoredRecord& record,
                                          const std::string& customer_id) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        record.body.value("customer_id", std::string()) != customer_id) {
        return std::nullopt;
    }
    MeteredAppHour hour{customer_id,
                        record.body.value("app_id", std::string()),
                        record.body.value("hour", std::string()),
                        unsigned_field(record.body, "in_bytes"),
                        unsigned_field(record.body, "out_bytes")};
    if (hour.app_id.empty() || !metering_hour_us(hour.hour)) {
        return std::nullopt;
    }
    return hour;
}

std::string app_hour_key(const MeteredAppHour& hour) {
    return hour.app_id + ":" + hour.hour;
}

} // namespace

std::optional<std::int64_t> metering_hour_us(std::string_view hour) {
    if (hour.size() != 20 || hour[4] != '-' || hour[7] != '-' || hour[10] != 'T' ||
        hour.substr(13) != ":00:00Z") {
        return std::nullopt;
    }
    const auto number = [&](std::size_t offset, std::size_t digits) -> std::optional<int> {
        int value = 0;
        for (std::size_t index = offset; index < offset + digits; ++index) {
            if (hour[index] < '0' || hour[index] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (hour[index] - '0');
        }
        return value;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hours = number(11, 2);
    if (!year || !month || !day || !hours || *hours > 23) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(days).count() +
           std::int64_t{*hours} * 3'600'000'000;
}

liquid::runtime::task<ZettaControlResult<void>>
add_metered_transfer(WorkspaceRecords& records,
                     std::string customer_id,
                     std::string hour,
                     std::uint64_t transfer_in_bytes,
                     std::uint64_t transfer_out_bytes,
                     int attempts) {
    const auto at_us = metering_hour_us(hour);
    if (customer_id.empty() || !at_us) {
        co_return failure<void>("invalid_input", "a transfer needs its account and UTC hour");
    }
    const auto collection = MeteringUsageCodec::collection;
    const std::string key = records.index_of(collection, "hour:" + hour);
    if (key.empty()) {
        co_return failure<void>("store_error", "could not index the hour");
    }
    ZettaControlError last{"conflict", "the hour's record kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto current = settled(co_await records.get(collection, key));
        if (!current) {
            co_return ZettaControlResult<void>(std::unexpected(current.error()));
        }
        RecordCondition condition;
        std::int64_t lake_revision = 1;
        std::uint64_t in = transfer_in_bytes;
        std::uint64_t out = transfer_out_bytes;
        if (*current) {
            const auto existing = transfer_of(**current, customer_id);
            if (!existing || existing->hour != hour) {
                // A record under this hour's key that does not belong here is never added to.
                co_return failure<void>("conflict", "the hour's record does not open here");
            }
            in = add_saturated(existing->transfer_in_bytes, in);
            out = add_saturated(existing->transfer_out_bytes, out);
            condition.if_revision = (*current)->revision;
            lake_revision = (*current)->lake_revision + 1;
        } else {
            condition.if_absent = true;
        }
        auto written = settled(co_await records.put(collection,
                                                    key,
                                                    lookup_of(customer_id),
                                                    {{"customer_id", customer_id},
                                                     {"hour", hour},
                                                     {"transfer_in_bytes", in},
                                                     {"transfer_out_bytes", out}},
                                                    condition,
                                                    lake_revision,
                                                    *at_us));
        if (written) {
            co_return ZettaControlResult<void>{};
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<void>(std::unexpected(written.error()));
        }
        last = written.error(); // another replica added first: reread and add again
    }
    co_return ZettaControlResult<void>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<bool>> record_metered_build(WorkspaceRecords& records,
                                                                     MeteredBuild build) {
    const auto at_us = metering_hour_us(build.hour);
    if (build.customer_id.empty() || build.result_id.empty() || !at_us) {
        co_return failure<bool>("invalid_input", "a build record needs its account, result and hour");
    }
    const auto collection = MeteredBuildCodec::collection;
    const std::string key = records.index_of(collection, "result:" + build.result_id);
    if (key.empty()) {
        co_return failure<bool>("store_error", "could not index the result");
    }
    auto written = settled(co_await records.put(collection,
                                                key,
                                                lookup_of(build.customer_id),
                                                {{"customer_id", build.customer_id},
                                                 {"repository_id", build.repository_id},
                                                 {"result_id", build.result_id},
                                                 {"hour", build.hour},
                                                 {"compute_seconds", build.compute_seconds}},
                                                RecordCondition{.if_absent = true},
                                                1,
                                                *at_us));
    if (!written) {
        if (written.error().code == "conflict") {
            co_return ZettaControlResult<bool>(false); // already counted
        }
        co_return ZettaControlResult<bool>(std::unexpected(written.error()));
    }
    co_return ZettaControlResult<bool>(true);
}

liquid::runtime::task<ZettaControlResult<bool>> record_metered_job(WorkspaceRecords& records,
                                                                   MeteredJob job) {
    const auto at_us = metering_hour_us(job.hour);
    if (job.customer_id.empty() || job.kind.empty() || job.object_id.empty() || !at_us) {
        co_return failure<bool>("invalid_input", "a job record needs its account, kind, id and hour");
    }
    const auto collection = MeteredJobCodec::collection;
    const std::string key = records.index_of(collection, job_key(job));
    if (key.empty()) {
        co_return failure<bool>("store_error", "could not index the job");
    }
    auto written = settled(co_await records.put(collection,
                                                key,
                                                lookup_of(job.customer_id),
                                                {{"customer_id", job.customer_id},
                                                 {"kind", job.kind},
                                                 {"object_id", job.object_id},
                                                 {"hour", job.hour},
                                                 {"seconds", job.seconds}},
                                                RecordCondition{.if_absent = true},
                                                1,
                                                *at_us));
    if (!written) {
        if (written.error().code == "conflict") {
            co_return ZettaControlResult<bool>(false); // charged before
        }
        co_return ZettaControlResult<bool>(std::unexpected(written.error()));
    }
    co_return ZettaControlResult<bool>(true);
}

liquid::runtime::task<ZettaControlResult<std::vector<MeteredJob>>>
list_metered_jobs(WorkspaceRecords& records, std::string customer_id, std::int64_t since_us) {
    using Jobs = std::vector<MeteredJob>;
    const auto collection = MeteredJobCodec::collection;
    auto stored = settled(co_await records.list(collection, lookup_of(customer_id), 0, since_us));
    if (!stored) {
        co_return ZettaControlResult<Jobs>(std::unexpected(stored.error()));
    }
    Jobs jobs;
    for (const StoredRecord& record : *stored) {
        auto job = job_of(record, customer_id);
        // The key binds the job: a record moved under another job's key does not count.
        if (job && record.id == records.index_of(collection, job_key(*job))) {
            jobs.push_back(std::move(*job));
        }
    }
    co_return ZettaControlResult<Jobs>(std::move(jobs));
}

liquid::runtime::task<ZettaControlResult<bool>> record_metered_app_hour(WorkspaceRecords& records,
                                                                        MeteredAppHour hour) {
    const auto at_us = metering_hour_us(hour.hour);
    if (hour.customer_id.empty() || hour.app_id.empty() || !at_us) {
        co_return failure<bool>("invalid_input", "an app hour record needs its account, app and hour");
    }
    const auto collection = MeteredAppTransferCodec::collection;
    const std::string key = records.index_of(collection, app_hour_key(hour));
    if (key.empty()) {
        co_return failure<bool>("store_error", "could not index the app hour");
    }
    auto written = settled(co_await records.put(collection,
                                                key,
                                                lookup_of(hour.customer_id),
                                                {{"customer_id", hour.customer_id},
                                                 {"app_id", hour.app_id},
                                                 {"hour", hour.hour},
                                                 {"in_bytes", hour.in_bytes},
                                                 {"out_bytes", hour.out_bytes}},
                                                RecordCondition{.if_absent = true},
                                                1,
                                                *at_us));
    if (!written) {
        if (written.error().code == "conflict") {
            co_return ZettaControlResult<bool>(false); // charged before
        }
        co_return ZettaControlResult<bool>(std::unexpected(written.error()));
    }
    co_return ZettaControlResult<bool>(true);
}

liquid::runtime::task<ZettaControlResult<std::vector<MeteredAppHour>>>
list_metered_app_hours(WorkspaceRecords& records, std::string customer_id, std::int64_t since_us) {
    using Hours = std::vector<MeteredAppHour>;
    const auto collection = MeteredAppTransferCodec::collection;
    auto stored = settled(co_await records.list(collection, lookup_of(customer_id), 0, since_us));
    if (!stored) {
        co_return ZettaControlResult<Hours>(std::unexpected(stored.error()));
    }
    Hours hours;
    for (const StoredRecord& record : *stored) {
        auto hour = app_hour_of(record, customer_id);
        // The key binds the hour: a record moved under another app's or hour's key does not count.
        if (hour && record.id == records.index_of(collection, app_hour_key(*hour))) {
            hours.push_back(std::move(*hour));
        }
    }
    co_return ZettaControlResult<Hours>(std::move(hours));
}

liquid::runtime::task<ZettaControlResult<std::vector<MeteredTransfer>>>
list_metered_transfer(WorkspaceRecords& records, std::string customer_id, std::int64_t since_us) {
    using Transfers = std::vector<MeteredTransfer>;
    auto stored = settled(
        co_await records.list(MeteringUsageCodec::collection, lookup_of(customer_id), 0, since_us));
    if (!stored) {
        co_return ZettaControlResult<Transfers>(std::unexpected(stored.error()));
    }
    Transfers transfers;
    for (const StoredRecord& record : *stored) {
        if (auto transfer = transfer_of(record, customer_id)) {
            transfers.push_back(std::move(*transfer));
        }
    }
    co_return ZettaControlResult<Transfers>(std::move(transfers));
}

liquid::runtime::task<ZettaControlResult<std::vector<MeteredBuild>>>
list_metered_builds(WorkspaceRecords& records, std::string customer_id, std::int64_t since_us) {
    using Builds = std::vector<MeteredBuild>;
    const auto collection = MeteredBuildCodec::collection;
    auto stored = settled(co_await records.list(collection, lookup_of(customer_id), 0, since_us));
    if (!stored) {
        co_return ZettaControlResult<Builds>(std::unexpected(stored.error()));
    }
    Builds builds;
    for (const StoredRecord& record : *stored) {
        auto build = build_of(record, customer_id);
        // The key binds the result: a record moved under another result's key does not count.
        if (build && record.id == records.index_of(collection, "result:" + build->result_id)) {
            builds.push_back(std::move(*build));
        }
    }
    co_return ZettaControlResult<Builds>(std::move(builds));
}

} // namespace lake::control
