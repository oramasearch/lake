// The PostgreSQL fixture's start-up sweep never fails a test (lake#1254). Under `ctest -j` every
// test process sweeps stale `lake_fixture_*` databases at start, and two processes can reach the
// same one: PostgreSQL marks a database invalid as soon as a DROP starts, and an ALTER DATABASE on
// an invalid database is FATAL. SharedLinkScenario's first four cases, the first PostgreSQL cases
// in ctest's order, failed in SetUp with "Lost connection to the database server" that way.

#include "postgres_fixture.hpp"

#include <gtest/gtest.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

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

const char* admin_uri() {
    const char* configured = std::getenv("LAKE_TEST_DB_URI");
    return configured != nullptr && *configured != '\0' ? configured : nullptr;
}

bool exists(pqxx::connection& admin, const std::string& name) {
    pqxx::nontransaction tx(admin);
    return !tx.exec("SELECT 1 FROM pg_database WHERE datname = " + tx.quote(name)).empty();
}

// A stale template of this test's own, in the state another process's sweep leaves it mid-drop:
// that sweep has run `ALTER ... IS_TEMPLATE false` and started `DROP DATABASE`, which marks the
// database invalid (datconnlimit -2) before removing it. This process listed it a moment earlier
// as a template. Setting that state needs a superuser.
TEST(FixtureSweep, AnInvalidTemplateIsDroppedWithoutLosingTheConnection) {
    const char* uri = admin_uri();
    if (uri == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    pqxx::connection admin(uri);
    {
        pqxx::nontransaction tx(admin);
        const auto role = tx.exec("SELECT rolsuper FROM pg_roles WHERE rolname = current_user");
        if (!role[0][0].as<bool>()) {
            GTEST_SKIP() << "marking a database invalid needs a superuser";
        }
    }
    const std::string name = "lake_fixture_template_" + random_hex(8);
    {
        pqxx::nontransaction tx(admin);
        tx.exec("CREATE DATABASE " + tx.quote_name(name) + " TEMPLATE template0");
        tx.exec("COMMENT ON DATABASE " + tx.quote_name(name) +
                " IS 'lake test fixture template, created 1'");
        tx.exec("ALTER DATABASE " + tx.quote_name(name) +
                " WITH ALLOW_CONNECTIONS false IS_TEMPLATE true");
        tx.exec("UPDATE pg_database SET datistemplate = false, datconnlimit = -2 "
                "WHERE datname = " +
                tx.quote(name));
    }
    ASSERT_TRUE(exists(admin, name));

    // Treated as the template it was listed as, the ALTER is FATAL and ends the session: the old
    // sweep did this on the process's shared connection, and the test in SetUp failed with "Lost
    // connection to the database server". Now it is reported and nothing is thrown.
    EXPECT_NO_THROW(EXPECT_FALSE(lake::test::drop_stale_fixture_database(uri, name, true, false)));
    // The caller's own connection is untouched either way.
    EXPECT_TRUE(exists(admin, name));
    // Known to be invalid, it is dropped.
    EXPECT_TRUE(lake::test::drop_stale_fixture_database(uri, name, true, true));
    EXPECT_FALSE(exists(admin, name));
    // Already gone: nothing to do, and nothing thrown.
    EXPECT_NO_THROW(
        static_cast<void>(lake::test::drop_stale_fixture_database(uri, name, true, true)));
}

} // namespace
