#pragma once

// lake's control-plane schema as built into this binary (lake#1119, orama-operator#936), and
// `lake migrate`, which applies it. db/schema.sql is compiled in, so an image carries the schema
// it needs and a release is only the image a Lake names: the operator runs `lake migrate` from
// that image instead of vendoring the file. Migrations never run from the server process.
// docs/control-plane/schema.md#applying-the-schema-lake-migrate.

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

// db/schema.sql, byte for byte (cmake/embed_schema.cmake).
std::string_view embedded_schema_sql();

// What a schema file records, read from its text the way the operator's PostgresSchema Job reads
// it: the migration versions its `INSERT INTO schema_migrations (version) VALUES ('NNNN_name')`
// statements record, and the `*_guard` tables it creates.
struct SchemaFacts {
    std::string sha256;                    // lowercase hex of the file's bytes
    std::string version;                   // "sha256:" + its first 12 hex digits
    std::vector<std::string> migrations;   // unique, in file order
    std::string latest_migration;          // the greatest version
    std::vector<std::string> guard_tables; // unique, in file order
    std::string guard_table;               // the last one created: the newest migration's
};
SchemaFacts schema_facts(std::string_view sql);

// pg_advisory_xact_lock key every `lake migrate` takes ("lakemig"), so runs against one
// database apply one at a time.
inline constexpr std::int64_t schema_migration_lock_key = 0x6c616b656d6967LL;

struct MigrationOptions {
    std::string connection_uri;   // LAKE_CONTROL_DB_URI
    std::chrono::seconds wait{0}; // keep trying to connect this long
};

// What one `lake migrate` did. `applied` false means the database was already current (the
// newest guard table exists and every migration the file records is in schema_migrations), so
// nothing ran; true means the whole file ran in one transaction and committed.
struct MigrationReport {
    bool applied = false;
    std::vector<std::string> migrations_applied;   // rows this run added to schema_migrations
    std::vector<std::string> guard_tables_created; // guard tables this run created
    std::vector<std::string> migrations_missing;   // what was missing before (empty if current)
    std::size_t migrations_recorded = 0;           // schema_migrations rows after the run
    std::int64_t elapsed_ms = 0;
};

struct MigrationError {
    // connect: the database could not be reached within the wait; apply: a statement failed and
    // the transaction rolled back, so nothing changed.
    std::string code;
    std::string message;
};

// Applies `sql` to the database, idempotently: one transaction, under
// pg_advisory_xact_lock(schema_migration_lock_key), skipped when the database is already current.
// Implemented in postgres_store.cpp, the one translation unit that names libpqxx.
std::expected<MigrationReport, MigrationError> apply_control_schema(const MigrationOptions& options,
                                                                    std::string_view sql);

// The JSON `lake migrate` prints on stdout for a report or an error.
nlohmann::json migration_report_json(const SchemaFacts& facts, const MigrationReport& report);
nlohmann::json migration_error_json(const SchemaFacts& facts, const MigrationError& error);

// `lake migrate`'s exit codes (docs/control-plane/schema.md).
inline constexpr int migrate_exit_ok = 0;
inline constexpr int migrate_exit_apply_failed = 1;
inline constexpr int migrate_exit_usage = 2;
inline constexpr int migrate_exit_unreachable = 3;

} // namespace lake::control
