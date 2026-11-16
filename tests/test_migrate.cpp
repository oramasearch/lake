// SPDX-License-Identifier: MIT

// `lake migrate` (lake#1119, orama-operator#936) does what the operator's PostgresSchema Job did:
// the same database on an empty server and on one already at the current schema, idempotent, one
// transaction, and runs against one database applying one at a time.

#include "lake/control/schema.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <pqxx/pqxx>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

namespace {

using lake::control::apply_control_schema;
using lake::control::embedded_schema_sql;
using lake::control::MigrationOptions;
using lake::control::schema_facts;

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> random(bytes);
    EXPECT_EQ(RAND_bytes(random.data(), static_cast<int>(random.size())), 1);
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : random) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

// An empty database of its own, named as the fixture's clones are (lake_fixture_<epoch>_<hex>),
// so the fixture's sweep removes it if this process dies; dropped when the test ends.
class EmptyDatabase {
public:
    explicit EmptyDatabase(const char* admin_uri) : admin_(admin_uri) {
        pqxx::nontransaction tx(admin_);
        const auto epoch =
            tx.exec("SELECT floor(extract(epoch FROM now()))::bigint")[0][0].as<long long>();
        name_ = "lake_fixture_" + std::to_string(epoch) + "_" + random_hex(8);
        tx.exec("CREATE DATABASE " + tx.quote_name(name_) + " TEMPLATE template0");
        uri_ = admin_.connection_string() + " dbname=" + name_;
    }
    ~EmptyDatabase() {
        try {
            pqxx::nontransaction tx(admin_);
            tx.exec("DROP DATABASE IF EXISTS " + tx.quote_name(name_) + " WITH (FORCE)");
        } catch (const std::exception& error) {
            ADD_FAILURE() << "Cannot drop " << name_ << ": " << error.what();
        }
    }
    const std::string& uri() const {
        return uri_;
    }
    const std::string& name() const {
        return name_;
    }

private:
    pqxx::connection admin_;
    std::string name_;
    std::string uri_;
};

const char* admin_uri() {
    const char* configured = std::getenv("LAKE_TEST_DB_URI");
    return configured != nullptr && *configured != '\0' ? configured : nullptr;
}

// Everything the schema decides, in a stable order: tables and their columns, constraints, indexes,
// functions, triggers, views, owners, the rows of schema_migrations and the row count of every
// table (seed rows and guard rows included). Never applied_at or other clock values.
std::vector<std::string> fingerprint(const std::string& uri) {
    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    std::vector<std::string> lines;
    const auto add = [&](const std::string& label, const std::string& query) {
        for (const auto& row : tx.exec(query)) {
            std::string line = label;
            for (const auto& field : row) {
                line += "|";
                line += field.is_null() ? "<null>" : field.as<std::string>();
            }
            lines.push_back(std::move(line));
        }
    };
    add("column",
        "SELECT table_name, column_name, data_type, is_nullable, coalesce(column_default, '') "
        "FROM information_schema.columns WHERE table_schema = 'public' "
        "ORDER BY table_name, column_name");
    add("constraint",
        "SELECT c.conrelid::regclass::text, c.conname, c.contype::text, "
        "pg_get_constraintdef(c.oid) "
        "FROM pg_constraint c JOIN pg_namespace n ON n.oid = c.connamespace "
        "WHERE n.nspname = 'public' ORDER BY 1, 2");
    add("index",
        "SELECT tablename, indexname, indexdef FROM pg_indexes "
        "WHERE schemaname = 'public' ORDER BY 1, 2");
    add("function",
        "SELECT p.proname, pg_get_function_identity_arguments(p.oid), md5(p.prosrc) "
        "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname = 'public' ORDER BY 1, 2");
    add("trigger",
        "SELECT tgrelid::regclass::text, tgname, pg_get_triggerdef(oid) FROM pg_trigger "
        "WHERE NOT tgisinternal ORDER BY 1, 2");
    add("view",
        "SELECT viewname, md5(definition) FROM pg_views WHERE schemaname = 'public' "
        "ORDER BY 1");
    add("owner",
        "SELECT c.relname, c.relkind::text, pg_get_userbyid(c.relowner) = current_user "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' ORDER BY 1");
    add("migration", "SELECT version FROM schema_migrations ORDER BY 1");
    add("rows",
        "SELECT table_name, (xpath('/row/c/text()', query_to_xml(format('SELECT count(*) AS c "
        "FROM public.%I', table_name), false, true, '')))[1]::text "
        "FROM information_schema.tables WHERE table_schema = 'public' "
        "AND table_type = 'BASE TABLE' ORDER BY 1");
    return lines;
}

std::string quoted(const std::string& value) {
    std::string text = "'";
    for (const char character : value) {
        text += character == '\'' ? std::string("'\\''") : std::string(1, character);
    }
    return text + "'";
}

// The operator's PostgresSchema Job applies the file with
// `psql "$DATABASE_URL" -v ON_ERROR_STOP=1 --single-transaction -f schema.sql`. This runs exactly
// that when psql is installed.
bool operator_job_apply(const EmptyDatabase& database, std::string& output) {
    const auto file = std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql";
    const std::string command = "psql " + quoted(database.uri()) +
                                " -X -q -v ON_ERROR_STOP=1 --single-transaction -f " +
                                quoted(file.string()) + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    std::array<char, 4096> buffer{};
    while (const auto read = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), read);
    }
    const int status = pclose(pipe);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool psql_installed() {
    return std::system("command -v psql >/dev/null 2>&1") == 0;
}

std::pair<int, std::string> run_lake(const std::string& arguments) {
    const std::string command = "env -u LAKE_CONTROL_DB_URI '" +
                                std::string(LAKE_TEST_SERVER_PATH) + "' " + arguments +
                                " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {-1, ""};
    }
    std::string output;
    std::array<char, 4096> buffer{};
    while (const auto read = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), read);
    }
    const int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

} // namespace

TEST(LakeMigrate, AnEmptyDatabaseGetsTheWholeSchema) {
    if (admin_uri() == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const EmptyDatabase database(admin_uri());
    const auto facts = schema_facts(embedded_schema_sql());
    const auto report =
        apply_control_schema(MigrationOptions{database.uri()}, embedded_schema_sql());
    ASSERT_TRUE(report) << report.error().code << ": " << report.error().message;
    EXPECT_TRUE(report->applied);
    EXPECT_EQ(report->migrations_applied, facts.migrations);
    EXPECT_EQ(report->migrations_missing, facts.migrations);
    EXPECT_EQ(report->guard_tables_created, facts.guard_tables);
    EXPECT_EQ(report->migrations_recorded, facts.migrations.size());
}

// The same database the operator's Job leaves, on an empty server.
TEST(LakeMigrate, AnEmptyDatabaseMatchesTheOperatorsJob) {
    if (admin_uri() == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    if (!psql_installed()) {
        GTEST_SKIP() << "psql is not installed";
    }
    const EmptyDatabase by_lake(admin_uri());
    const EmptyDatabase by_job(admin_uri());
    ASSERT_TRUE(apply_control_schema(MigrationOptions{by_lake.uri()}, embedded_schema_sql()));
    std::string output;
    const bool applied = operator_job_apply(by_job, output);
    ASSERT_TRUE(applied) << output;
    const auto lake = fingerprint(by_lake.uri());
    const auto job = fingerprint(by_job.uri());
    EXPECT_GT(lake.size(), 1000U);
    EXPECT_EQ(lake, job);
}

TEST(LakeMigrate, ADatabaseAtTheCurrentSchemaIsLeftAlone) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const auto before = fingerprint(uri);
    const auto report = apply_control_schema(MigrationOptions{uri}, embedded_schema_sql());
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_FALSE(report->applied);
    EXPECT_TRUE(report->migrations_applied.empty());
    EXPECT_TRUE(report->migrations_missing.empty());
    EXPECT_TRUE(report->guard_tables_created.empty());
    EXPECT_EQ(fingerprint(uri), before);
    // And once more: still nothing to do.
    const auto again = apply_control_schema(MigrationOptions{uri}, embedded_schema_sql());
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->applied);
}

// The guard alone is not enough (orama-operator#271): a migration numbered before the guard's can
// merge after it, and a database missing its row gets the whole file again, which adds only it.
TEST(LakeMigrate, AMissingMigrationRowAppliesTheFileAgain) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const auto before = fingerprint(uri);
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0084_network'");
        tx.commit();
    }
    const auto report = apply_control_schema(MigrationOptions{uri}, embedded_schema_sql());
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_TRUE(report->applied);
    EXPECT_EQ(report->migrations_missing, std::vector<std::string>{"0084_network"});
    EXPECT_EQ(report->migrations_applied, std::vector<std::string>{"0084_network"});
    EXPECT_TRUE(report->guard_tables_created.empty());
    EXPECT_EQ(fingerprint(uri), before);
}

TEST(LakeMigrate, ConcurrentRunsApplyOneAtATime) {
    if (admin_uri() == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const EmptyDatabase database(admin_uri());
    constexpr int runs = 4;
    using Result = std::expected<lake::control::MigrationReport, lake::control::MigrationError>;
    // A plain array: libc++'s expected operator== trips on vector iterators of it.
    std::array<std::optional<Result>, runs> results;
    {
        std::vector<std::jthread> threads;
        for (int index = 0; index < runs; ++index) {
            threads.emplace_back([&, index] {
                results[index] =
                    apply_control_schema(MigrationOptions{database.uri()}, embedded_schema_sql());
            });
        }
    }
    int applied = 0;
    for (int index = 0; index < runs; ++index) {
        const auto& result = results[index];
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(*result) << (*result).error().code << ": " << (*result).error().message;
        applied += (*result)->applied ? 1 : 0;
    }
    EXPECT_EQ(applied, 1);
    const auto facts = schema_facts(embedded_schema_sql());
    pqxx::connection connection(database.uri());
    pqxx::read_transaction tx(connection);
    EXPECT_EQ(tx.exec("SELECT count(*) FROM schema_migrations")[0][0].as<std::size_t>(),
              facts.migrations.size());
}

TEST(LakeMigrate, AFailedRunChangesNothing) {
    if (admin_uri() == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const EmptyDatabase database(admin_uri());
    const std::string broken = std::string(embedded_schema_sql()) + "\nSELECT 1 / 0;\n";
    const auto report = apply_control_schema(MigrationOptions{database.uri()}, broken);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, "apply");
    pqxx::connection connection(database.uri());
    pqxx::read_transaction tx(connection);
    EXPECT_EQ(tx.exec("SELECT count(*) FROM pg_tables WHERE schemaname = 'public'")[0][0].as<int>(),
              0);
}

TEST(LakeMigrate, AnUnreachableDatabaseIsReportedAsSuch) {
    const auto started = std::chrono::steady_clock::now();
    const auto report = apply_control_schema(
        MigrationOptions{"postgresql://lake@127.0.0.1:1/lake?connect_timeout=2",
                         std::chrono::seconds(3)},
        embedded_schema_sql());
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, "connect");
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
}

// The binary: JSON on stdout and the documented exit codes.
TEST(LakeMigrate, TheCommandPrintsWhatItApplied) {
    if (admin_uri() == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    const EmptyDatabase database(admin_uri());
    const auto facts = schema_facts(embedded_schema_sql());
    const auto [status, output] = run_lake("migrate --control-db " + quoted(database.uri()));
    ASSERT_EQ(status, lake::control::migrate_exit_ok) << output;
    const auto first = nlohmann::json::parse(output, nullptr, false);
    ASSERT_FALSE(first.is_discarded()) << output;
    EXPECT_EQ(first["ok"], true);
    EXPECT_EQ(first["outcome"], "applied");
    EXPECT_EQ(first["schema"]["latest_migration"], facts.latest_migration);
    EXPECT_EQ(first["schema"]["guard_table"], facts.guard_table);
    EXPECT_EQ(first["schema"]["sha256"], facts.sha256);
    EXPECT_EQ(first["migrations_applied"].size(), facts.migrations.size());
    EXPECT_EQ(first["guard_tables_created"].size(), facts.guard_tables.size());

    const auto [again_status, again_output] =
        run_lake("migrate --control-db " + quoted(database.uri()));
    ASSERT_EQ(again_status, lake::control::migrate_exit_ok) << again_output;
    const auto second = nlohmann::json::parse(again_output, nullptr, false);
    EXPECT_EQ(second["outcome"], "current");
    EXPECT_TRUE(second["migrations_applied"].empty());

    const auto [down_status, down_output] =
        run_lake("migrate --wait-seconds 0 --control-db "
                 "'postgresql://lake@127.0.0.1:1/lake?connect_timeout=2'");
    EXPECT_EQ(down_status, lake::control::migrate_exit_unreachable);
    const auto down = nlohmann::json::parse(down_output, nullptr, false);
    ASSERT_FALSE(down.is_discarded()) << down_output;
    EXPECT_EQ(down["error"]["code"], "connect");
}
