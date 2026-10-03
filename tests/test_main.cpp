#include "lake/control/store.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>
#include <execinfo.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pqxx/pqxx>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// Every database this runner creates starts with this prefix, and the start-up sweep touches no
// other name. Per-test clones are lake_fixture_<server epoch>_<16 hex>; a template being loaded
// is lake_fixture_build_<epoch>_<16 hex>; a finished template is
// lake_fixture_template_<first 16 hex of SHA-256(db/schema.sql)>.
constexpr std::string_view fixture_prefix = "lake_fixture_";
constexpr std::string_view template_prefix = "lake_fixture_template_";
// A clone or build older than this belongs to a process that died before teardown: CTest's
// default timeout is 1,500 seconds.
constexpr long long stale_clone_seconds = 3600;
// Templates of other schema versions are kept a day, so worktrees on different revisions that
// share a server do not drop each other's templates mid-run.
constexpr long long stale_template_seconds = 86400;
// pg_advisory_lock key ("laketpl") serializing template loads on one administrative database.
constexpr long long template_lock_key = 0x6c616b6574706cLL;

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        throw std::runtime_error("Cannot generate random test fixture bytes");
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : random) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

std::string schema_sql() {
    std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
    std::string sql((std::istreambuf_iterator<char>(file)), {});
    if (sql.empty()) {
        throw std::runtime_error("Cannot read db/schema.sql for PostgreSQL fixture");
    }
    return sql;
}

std::string schema_digest(const std::string& sql) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(sql.data()), sql.size(), digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (std::size_t index = 0; index < 8; ++index) {
        text += digits[digest[index] >> 4];
        text += digits[digest[index] & 15];
    }
    return text;
}

long long server_epoch(pqxx::connection& admin) {
    pqxx::nontransaction tx(admin);
    return tx.exec("SELECT floor(extract(epoch FROM now()))::bigint")[0][0].as<long long>();
}

bool database_exists(pqxx::connection& admin, const std::string& name) {
    pqxx::nontransaction tx(admin);
    return !tx.exec("SELECT 1 FROM pg_database WHERE datname = " + tx.quote(name)).empty();
}

bool is_sqlstate(const pqxx::sql_error& error, std::string_view state) {
    return error.sqlstate() == state;
}

// Retries a statement that needs the named database to have no sessions: autovacuum can hold a
// brief one on a database that still allows connections.
void exec_without_sessions(pqxx::connection& admin, const std::string& sql) {
    for (int attempt = 0;; ++attempt) {
        try {
            pqxx::nontransaction tx(admin);
            tx.exec(sql);
            return;
        } catch (const pqxx::sql_error& error) {
            if (!is_sqlstate(error, "55006") || attempt >= 200) {
                throw;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void drop_database(pqxx::connection& admin, const std::string& name, bool is_template) {
    pqxx::nontransaction tx(admin);
    if (is_template) {
        tx.exec("ALTER DATABASE " + tx.quote_name(name) + " WITH IS_TEMPLATE false");
    }
    tx.exec("DROP DATABASE IF EXISTS " + tx.quote_name(name));
}

// Removes databases with this runner's prefix that a killed process left behind. A database
// that still has a session fails DROP with 55006 and is left alone; nothing outside the prefix is
// ever listed.
void sweep_stale_databases(pqxx::connection& admin, const std::string& current_template) {
    const long long now = server_epoch(admin);
    pqxx::result rows;
    {
        pqxx::nontransaction tx(admin);
        rows = tx.exec(
            "SELECT datname, datistemplate, "
            "coalesce(shobj_description(oid, 'pg_database'), '') FROM pg_database "
            "WHERE datname ~ '^lake_fixture_(build_)?[0-9]+_[0-9a-f]{16}$' "
            "OR datname ~ '^lake_fixture_template_[0-9a-f]{16}$'");
    }
    for (const auto& row : rows) {
        const auto name = row[0].as<std::string>();
        const bool is_template = row[1].as<bool>();
        long long created = 0;
        long long limit = stale_clone_seconds;
        if (name.starts_with(template_prefix)) {
            if (name == current_template) {
                continue;
            }
            limit = stale_template_seconds;
            const auto comment = row[2].as<std::string>();
            const auto at = comment.rfind(' ');
            created = std::atoll(comment.c_str() + (at == std::string::npos ? 0 : at + 1));
        } else {
            const auto digits = name.find_first_of("0123456789", fixture_prefix.size());
            created = std::atoll(name.c_str() + digits);
        }
        if (now - created < limit) {
            continue;
        }
        try {
            drop_database(admin, name, is_template);
        } catch (const pqxx::sql_error& error) {
            // In use (55006) means a live process owns it; anything else is left for the next run.
            std::cerr << "PostgreSQL fixture sweep left " << name << ": " << error.what() << '\n';
        }
    }
}

std::string build_template(pqxx::connection& admin, const std::string& name,
                           const std::string& sql) {
    const std::string build =
        std::string(fixture_prefix) + "build_" + std::to_string(server_epoch(admin)) + "_" +
        random_hex(8);
    {
        pqxx::nontransaction tx(admin);
        tx.exec("CREATE DATABASE " + tx.quote_name(build) + " TEMPLATE template0");
    }
    try {
        {
            pqxx::connection connection(admin.connection_string() + " dbname=" + build);
            pqxx::work tx(connection);
            tx.exec("SET client_min_messages TO warning");
            tx.exec(sql);
            tx.commit();
        }
        {
            const std::string created = std::to_string(server_epoch(admin));
            pqxx::nontransaction tx(admin);
            tx.exec("COMMENT ON DATABASE " + tx.quote_name(build) +
                    " IS 'lake test fixture template, created " + created + "'");
            tx.exec("ALTER DATABASE " + tx.quote_name(build) +
                    " WITH ALLOW_CONNECTIONS false IS_TEMPLATE true");
        }
        try {
            // The rename publishes a fully loaded template under its final name in one step.
            exec_without_sessions(admin, "ALTER DATABASE " + admin.quote_name(build) +
                                             " RENAME TO " + admin.quote_name(name));
        } catch (const pqxx::sql_error& error) {
            if (!is_sqlstate(error, "42P04")) {
                throw;
            }
            // Another administrative database's runner published the same schema first.
            drop_database(admin, build, true);
        }
    } catch (...) {
        try {
            drop_database(admin, build, true);
        } catch (...) {
        }
        throw;
    }
    return name;
}

// Returns the migrated template for the current db/schema.sql, loading it once. Loads are
// serialized by an advisory lock so concurrent test processes wait rather than each loading the
// whole schema.
std::string ensure_template(pqxx::connection& admin, const std::string& name,
                            const std::string& sql) {
    if (database_exists(admin, name)) {
        return name;
    }
    {
        pqxx::nontransaction tx(admin);
        tx.exec("SELECT pg_advisory_lock(" + std::to_string(template_lock_key) + ")");
    }
    try {
        if (!database_exists(admin, name)) {
            build_template(admin, name, sql);
        }
    } catch (...) {
        pqxx::nontransaction tx(admin);
        tx.exec("SELECT pg_advisory_unlock(" + std::to_string(template_lock_key) + ")");
        throw;
    }
    pqxx::nontransaction tx(admin);
    tx.exec("SELECT pg_advisory_unlock(" + std::to_string(template_lock_key) + ")");
    return name;
}

bool swept = false;

class PostgresDatabase {
public:
    explicit PostgresDatabase(const std::string& base_uri) : admin_(base_uri) {
        const std::string sql = schema_sql();
        const std::string template_name = std::string(template_prefix) + schema_digest(sql);
        if (!swept) {
            swept = true;
            sweep_stale_databases(admin_, template_name);
        }
        ensure_template(admin_, template_name, sql);
        name_ = std::string(fixture_prefix) + std::to_string(server_epoch(admin_)) + "_" +
                random_hex(8);
        clone(template_name, sql);
        uri_ = admin_.connection_string() + " dbname=" + name_;
    }

    ~PostgresDatabase() {
        try {
            drop();
        } catch (const std::exception& error) {
            ADD_FAILURE() << "Cannot remove test database " << name_ << ": " << error.what();
        }
    }

    const char* uri() const {
        return uri_.c_str();
    }

private:
    // Copying a template takes a handful of locks; loading db/schema.sql takes one per table,
    // index, sequence and trigger, which concurrent tests multiplied past the server's lock table.
    void clone(const std::string& template_name, const std::string& sql) {
        for (int attempt = 0;; ++attempt) {
            try {
                pqxx::nontransaction tx(admin_);
                tx.exec("CREATE DATABASE " + tx.quote_name(name_) + " TEMPLATE " +
                        tx.quote_name(template_name));
                created_ = true;
                return;
            } catch (const pqxx::sql_error& error) {
                if (attempt >= 200) {
                    throw;
                }
                if (is_sqlstate(error, "3D000")) {
                    // A sweep elsewhere removed an old template; load it again.
                    ensure_template(admin_, template_name, sql);
                    continue;
                }
                // 55006: a session (autovacuum, or a template being published) is on the
                // template, and PostgreSQL refuses to copy it until that session leaves.
                if (!is_sqlstate(error, "55006")) {
                    throw;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25 + (attempt % 8) * 25));
        }
    }

    void drop() {
        if (created_) {
            pqxx::nontransaction tx(admin_);
            tx.exec("DROP DATABASE " + tx.quote_name(name_));
            created_ = false;
        }
    }

    pqxx::connection admin_;
    std::string name_;
    std::string uri_;
    bool created_ = false;
};

std::unique_ptr<PostgresDatabase> database;

// An abort names its exception and prints the stack it was thrown on (lake#510), so a crash on a
// background thread in a parallel run is diagnosable from the test's log alone.
[[noreturn]] void print_terminate() {
    if (const auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "terminate: uncaught exception: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "terminate: uncaught non-standard exception\n");
        }
    } else {
        std::fprintf(stderr, "terminate: called without an exception\n");
    }
    void* frames[128];
    const int count = backtrace(frames, 128);
    backtrace_symbols_fd(frames, count, 2);
    std::abort();
}

class DatabaseCleanup : public testing::EmptyTestEventListener {
public:
    void OnTestStart(const testing::TestInfo&) override {
        const std::string key = random_hex(32);
        if (setenv("LAKE_INSTANCE_KEK", key.c_str(), 1) != 0) {
            throw std::runtime_error("Cannot set fixture LAKE_INSTANCE_KEK");
        }
    }

    void OnTestEnd(const testing::TestInfo&) override {
        database.reset();
    }
};

} // namespace

const char* lake::test::postgres_uri() {
    const char* configured = std::getenv("LAKE_TEST_DB_URI");
    if (configured == nullptr || *configured == '\0') {
        return nullptr;
    }
    if (!database) {
        database = std::make_unique<PostgresDatabase>(configured);
    }
    return database->uri();
}

int main(int argc, char** argv) {
    std::set_terminate(print_terminate);
    testing::InitGoogleTest(&argc, argv);
    testing::UnitTest::GetInstance()->listeners().Append(new DatabaseCleanup);
    return RUN_ALL_TESTS();
}
