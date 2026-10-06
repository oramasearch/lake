#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Repository usage in the workspace Zetta of the repository's workspace (docs/api/metering.md,
// lake#348, lake#349). Two sealed collections, both found by the space (`customer:<id>`) and
// both stamped with the start of their UTC hour, so a listing is in time order and a window's
// `from` bounds the walk:
//
// - `lake_metering_usage`: exact transfer counters. Lake keeps one record per workspace and hour
//   under the blind key of `hour:<hour>` and adds to it with a conditional write on the revision
//   it read. A row copied from the retired table keeps its own key (its legacy id), so a copy
//   never overwrites what lake counted after the cutover; a reader adds every record of an hour.
// - `lake_metering_build_records`: one record per repository build result, under the blind key
//   of `result:<id>`, written `if_absent`, so a result is counted once however often it is seen.
//
// Repository storage is not recorded: it is measured live on every read (lake#380 makes stores
// per workspace). Nothing here is priced; pricing stays in the API layer's report.
struct MeteringUsageCodec {
    static constexpr RecordCollection collection{"metering_usage", 1};
};
struct MeteredBuildCodec {
    static constexpr RecordCollection collection{"metering_build_records", 1};
};

// One transfer record as read back: an hour's counters, or a copied legacy row's.
struct MeteredTransfer {
    std::string id;
    std::string customer_id;
    std::string hour;
    std::uint64_t transfer_in_bytes = 0;
    std::uint64_t transfer_out_bytes = 0;
};

// Microseconds since the Unix epoch of an hour "YYYY-MM-DDTHH:00:00Z"; nullopt for anything else.
std::optional<std::int64_t> metering_hour_us(std::string_view hour);

// Adds transfer bytes to the workspace's record of `hour`: read, then write on the revision read
// (or `if_absent` for a new hour), rereading on a conflict up to `attempts` times. Concurrent
// writers from several lake replicas therefore never lose an increment.
liquid::runtime::task<ZettaControlResult<void>>
add_metered_transfer(WorkspaceRecords& records,
                     std::string customer_id,
                     std::string hour,
                     std::uint64_t transfer_in_bytes,
                     std::uint64_t transfer_out_bytes,
                     int attempts = 8);

// Records a build result's compute once: `if_absent` under the blind key of its result id. True
// when written, false when the result was already recorded.
liquid::runtime::task<ZettaControlResult<bool>> record_metered_build(WorkspaceRecords& records,
                                                                     MeteredBuild build);

// The space's records in this workspace, oldest hour first, none before `since_us` (0: all).
// Records that do not open here or name another space are skipped.
liquid::runtime::task<ZettaControlResult<std::vector<MeteredTransfer>>> list_metered_transfer(
    WorkspaceRecords& records, std::string customer_id, std::int64_t since_us = 0);
liquid::runtime::task<ZettaControlResult<std::vector<MeteredBuild>>>
list_metered_builds(WorkspaceRecords& records, std::string customer_id, std::int64_t since_us = 0);

} // namespace lake::control
