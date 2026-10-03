#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/agent_bus.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

// Lake's records in the workspace Zetta that owns them (docs/architecture/workspace-records.md,
// lake#370). Every collection is a table in the workspace's reserved `lake_records` database,
// reached through the workspace caller with Zetta's `sql` capability. A record is a sealed JSON
// body: lake encrypts it with a key derived for that workspace alone, and finds it again through
// one blind-indexed lookup value, so the workspace holds only ciphertext and HMACs, and a row
// copied into another workspace's table does not open there.
//
// The interim limits of the existing Zetta primitive are deliberate and documented: no compare-and-
// set (append-only collections only), single-predicate lookups, and a GET request line of about
// 4 KiB, which is why a large body is written as several parts and becomes visible only once all
// of its parts are present.

// The reserved database. Lake's workspace surfaces never list it and refuse statements naming it.
inline constexpr std::string_view workspace_records_database = "lake_records";

// Whether `text` names the reserved database (case-insensitive), for those surface guards.
bool names_workspace_records(std::string_view text);

// One collection: its table name (lowercase letters, digits and underscores; the Postgres table it
// replaces keeps its name) and the version of the body schema it writes now.
struct RecordCollection {
    std::string_view name;
    int schema_version = 1;
};

// One record as read back: its id, its creation instant in microseconds since the Unix epoch, the
// schema version it was written with, and its opened body.
struct StoredRecord {
    std::string id;
    std::int64_t at_us = 0;
    int schema_version = 1;
    nlohmann::json body;
};

struct RecordWrite {
    // Empty: a new UUIDv4. A copy keeps the source row's id, which makes a rerun idempotent.
    std::string id;
    // 0: now, strictly increasing within this process. A copy keeps the source row's instant.
    std::int64_t at_us = 0;
};

// Record I/O for one workspace. `Call` is the existing workspace caller, already bound to the
// customer and the workspace (WorkspaceService::call, or the API layer's call_workspace so that
// Server-Timing names each call).
class WorkspaceRecords {
public:
    using Call = std::function<liquid::runtime::task<ZettaControlResult<nlohmann::json>>(
        std::string capability, bool read, nlohmann::json input)>;

    // Derives this workspace's sealing and index keys from LAKE_INSTANCE_KEK; refuses a missing
    // KEK or a workspace id that is not a UUID.
    static ZettaControlResult<WorkspaceRecords>
    open(Call call, std::string workspace_id, std::string_view instance_kek);

    const std::string& workspace_id() const {
        return workspace_id_;
    }

    // Appends one record found by `lookup`. Creates the database and table on first use.
    liquid::runtime::task<ZettaControlResult<StoredRecord>>
    append(RecordCollection collection, std::string lookup, nlohmann::json body,
           RecordWrite write = {});

    // Every complete record found by `lookup`, oldest first (by instant, then id), at most the
    // newest `newest` of them (0: all). A missing table is no records. Rows that do not open with
    // this workspace's key, incomplete multi-part records and duplicate copies are skipped.
    liquid::runtime::task<ZettaControlResult<std::vector<StoredRecord>>>
    list(RecordCollection collection, std::string lookup, std::size_t newest = 0);

    // The blind index of `lookup` in `collection`: what the table's `k` column holds.
    std::string index_of(RecordCollection collection, std::string_view lookup) const;

    // The most base64url characters of sealed body one INSERT carries, chosen so the encoded GET
    // request line stays well under Zetta's 4 KiB start-line limit.
    static constexpr std::size_t part_characters = 2800;

private:
    struct Keys;
    WorkspaceRecords(Call call, std::string workspace_id, std::shared_ptr<const Keys> keys);

    liquid::runtime::task<ZettaControlResult<nlohmann::json>> sql(std::string statement);
    liquid::runtime::task<ZettaControlResult<void>> create_table(RecordCollection collection);

    Call call_;
    std::string workspace_id_;
    std::shared_ptr<const Keys> keys_;
};

// A typed collection over WorkspaceRecords. `Codec` provides:
//   using Value = ...;
//   static constexpr RecordCollection collection;
//   static std::string lookup(const Value&);                 // the value's lookup key
//   static nlohmann::json encode(const Value&);              // its body
//   static std::optional<Value> decode(const StoredRecord&); // nullopt: not a valid body
template <class Codec> class RecordSet {
public:
    using Value = typename Codec::Value;

    explicit RecordSet(WorkspaceRecords& records) : records_(records) {}

    liquid::runtime::task<ZettaControlResult<Value>> append(Value value, RecordWrite write = {}) {
        auto stored = settled(co_await records_.append(
            Codec::collection, Codec::lookup(value), Codec::encode(value), std::move(write)));
        if (!stored) {
            co_return ZettaControlResult<Value>(std::unexpected(stored.error()));
        }
        auto decoded = Codec::decode(*stored);
        if (!decoded) {
            co_return ZettaControlResult<Value>(std::unexpected(
                ZettaControlError{"store_error", "the stored record did not decode"}));
        }
        co_return ZettaControlResult<Value>(std::move(*decoded));
    }

    liquid::runtime::task<ZettaControlResult<std::vector<Value>>> list(std::string lookup,
                                                                       std::size_t newest = 0) {
        auto stored =
            settled(co_await records_.list(Codec::collection, std::move(lookup), newest));
        if (!stored) {
            co_return ZettaControlResult<std::vector<Value>>(std::unexpected(stored.error()));
        }
        std::vector<Value> values;
        values.reserve(stored->size());
        for (const StoredRecord& record : *stored) {
            if (auto decoded = Codec::decode(record)) {
                values.push_back(std::move(*decoded));
            }
        }
        co_return ZettaControlResult<std::vector<Value>>(std::move(values));
    }

private:
    WorkspaceRecords& records_;
};

// "YYYY-MM-DDTHH:MM:SSZ" for an instant in microseconds, as Lake's APIs print instants.
std::string record_instant(std::int64_t at_us);

} // namespace lake::control
