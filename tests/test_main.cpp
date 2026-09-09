#include "lake/control/store.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

class PostgresDatabase {
public:
    explicit PostgresDatabase(const std::string& base_uri) : admin_(base_uri) {
        name_ = "lake_test_" + lake::control::new_uuid_v4();
        std::erase(name_, '-');
        {
            pqxx::nontransaction tx(admin_);
            tx.exec("CREATE DATABASE " + tx.quote_name(name_) + " TEMPLATE template0");
            created_ = true;
        }
        uri_ = admin_.connection_string() + " dbname=" + name_;
        try {
            load_schema();
        } catch (...) {
            drop();
            throw;
        }
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
    void load_schema() {
        std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
        const std::string sql((std::istreambuf_iterator<char>(file)), {});
        if (sql.empty()) {
            throw std::runtime_error("Cannot read db/schema.sql for PostgreSQL fixture");
        }
        pqxx::connection connection(uri_);
        pqxx::work tx(connection);
        tx.exec("SET client_min_messages TO warning");
        tx.exec(sql);
        tx.commit();
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

class DatabaseCleanup : public testing::EmptyTestEventListener {
public:
    void OnTestStart(const testing::TestInfo&) override {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error("Cannot generate fixture LAKE_INSTANCE_KEK");
        }
        constexpr char digits[] = "0123456789abcdef";
        std::string key;
        for (const auto byte : bytes) {
            key += digits[byte >> 4];
            key += digits[byte & 15];
        }
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
    testing::InitGoogleTest(&argc, argv);
    testing::UnitTest::GetInstance()->listeners().Append(new DatabaseCleanup);
    return RUN_ALL_TESTS();
}
