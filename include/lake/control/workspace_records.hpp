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
// lake#370). Every collection is a collection of the workspace's keyed record store (zetta#1231:
// create_collection, put_record, get_record, delete_record, list_records), named `lake_<name>`
// and reached through the workspace caller. A record's value is a sealed envelope: lake encrypts
// the body with a key derived for that workspace alone, and the record carries one blind-indexed
// lookup value (index `lookup`), so the workspace holds only ciphertext and HMACs, and a record
// copied into another workspace, collection, key or lookup does not open there.

// Zetta collections lake owns all start with this prefix; a non-admin service user needs
// `GRANT records:read, records:write, records:manage ON COLLECTION lake_`.
inline constexpr std::string_view workspace_records_prefix = "lake_";

// One collection: its name (lowercase letters, digits and underscores; the Postgres table it
// replaces keeps its name), the version of the body schema it writes now, and its retention
// (0: kept until deleted).
struct RecordCollection {
    std::string_view name;
    int schema_version = 1;
    std::int64_t retention_ms = 0;
};

// One record as read back: its key, its creation instant in microseconds since the Unix epoch,
// the schema version it was written with, lake's revision of it (1 for the first write, one more
// for each update), the engine's revision (pass it as `if_revision`), and its opened body.
struct StoredRecord {
    std::string id;
    std::int64_t at_us = 0;
    int schema_version = 1;
    std::int64_t lake_revision = 1;
    std::int64_t revision = 0;
    nlohmann::json body;
};

struct RecordWrite {
    // Empty: a new UUIDv4. A copy keeps the source row's id, which makes a rerun idempotent.
    std::string id;
    // 0: now, strictly increasing within this process. A copy keeps the source row's instant.
    std::int64_t at_us = 0;
};

// The write precondition of a mutable record, decided atomically by the key's owner in Zetta.
struct RecordCondition {
    bool if_absent = false;
    std::optional<std::int64_t> if_revision; // the engine revision the caller read
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

    // Appends one record found by `lookup`, only if its key is absent: one put_record. Creates the
    // collection on first use. An existing key is `conflict` (a rerun copy counts it as present).
    liquid::runtime::task<ZettaControlResult<StoredRecord>>
    append(RecordCollection collection, std::string lookup, nlohmann::json body,
           RecordWrite write = {});

    // Every record found by `lookup`, oldest first (by instant, then id), at most the newest
    // `newest` of them (0: all, following next_after), and none older than `since_us` (0: no
    // bound; the walk stops at the first older record). A missing collection is no records;
    // records that do not open with this workspace's key are skipped. With `keep`, only the
    // records it accepts are returned and counted toward `newest`, so a filtered read walks on
    // until it has `newest` matches (the workspace audit's filters, lake#317).
    using Keep = std::function<bool(const StoredRecord&)>;
    liquid::runtime::task<ZettaControlResult<std::vector<StoredRecord>>>
    list(RecordCollection collection, std::string lookup, std::size_t newest = 0,
         std::int64_t since_us = 0, Keep keep = {});

    // One record by key; nullopt when it does not exist (or does not open here).
    liquid::runtime::task<ZettaControlResult<std::optional<StoredRecord>>>
    get(RecordCollection collection, std::string id);

    // Writes a mutable record under `condition`; a failed condition is `conflict` with the live
    // engine revision in `details.revision`. `lake_revision` is the previous one plus one.
    liquid::runtime::task<ZettaControlResult<StoredRecord>>
    put(RecordCollection collection, std::string id, std::string lookup, nlohmann::json body,
        RecordCondition condition, std::int64_t lake_revision = 1, std::int64_t at_us = 0);

    // Deletes a record, only at `if_revision` when given; `not_found` when it does not exist.
    liquid::runtime::task<ZettaControlResult<void>>
    remove(RecordCollection collection, std::string id,
           std::optional<std::int64_t> if_revision = std::nullopt);

    // Read-modify-write: reads the record, asks `change` for its next body (nullopt: delete it, or
    // leave it absent), and writes it conditionally on the revision it read, rereading and asking
    // again on a conflict, up to `attempts` times. Answers the record written, or nullopt when
    // `change` deleted or left nothing.
    using Change = std::function<std::optional<nlohmann::json>(const std::optional<StoredRecord>&)>;
    liquid::runtime::task<ZettaControlResult<std::optional<StoredRecord>>>
    update(RecordCollection collection, std::string id, std::string lookup, Change change,
           int attempts = 5);

    // The blind index of `lookup` in `collection`: the record's `lookup` index value.
    std::string index_of(RecordCollection collection, std::string_view lookup) const;

    // `lake_<name>`, the Zetta collection of a lake collection.
    static std::string zetta_collection(RecordCollection collection);

private:
    struct Keys;
    WorkspaceRecords(Call call, std::string workspace_id, std::shared_ptr<const Keys> keys);

    // One record capability call, retried while the key's ownership is moving (503 `busy`).
    liquid::runtime::task<ZettaControlResult<nlohmann::json>> invoke(std::string capability,
                                                                     bool read,
                                                                     nlohmann::json input);
    liquid::runtime::task<ZettaControlResult<void>> create_collection(RecordCollection collection);
    liquid::runtime::task<ZettaControlResult<StoredRecord>>
    write(RecordCollection collection, StoredRecord record, std::string lookup,
          RecordCondition condition);
    std::optional<StoredRecord> opened(RecordCollection collection, const nlohmann::json& row) const;
    std::string associated(RecordCollection collection, std::string_view id, std::string_view key,
                           std::int64_t at_us, int schema_version, std::int64_t lake_revision) const;

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
