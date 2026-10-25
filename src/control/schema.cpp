#include "lake/control/schema.hpp"

#include <algorithm>
#include <array>
#include <regex>
#include <set>
#include <string>

#include <openssl/sha.h>

#include "lake/version.hpp"

namespace lake::control {

namespace {

std::string sha256_of(std::string_view bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

// Every match of `pattern`'s first group, unique, in the order they first appear.
std::vector<std::string> unique_matches(std::string_view text, const std::regex& pattern) {
    std::vector<std::string> found;
    std::set<std::string> seen;
    for (std::cregex_iterator it(text.data(), text.data() + text.size(), pattern), end; it != end;
         ++it) {
        std::string value = (*it)[1].str();
        if (seen.insert(value).second) {
            found.push_back(std::move(value));
        }
    }
    return found;
}

nlohmann::json schema_json(const SchemaFacts& facts) {
    return {{"sha256", facts.sha256},
            {"version", facts.version},
            {"latest_migration", facts.latest_migration},
            {"guard_table", facts.guard_table},
            {"migrations", facts.migrations.size()}};
}

} // namespace

SchemaFacts schema_facts(std::string_view sql) {
    // The operator's PostgresSchema Job reads the versions with a sed over the same statement; the
    // space before "(version)" is optional here, as PostgreSQL allows (0098 is written without).
    static const std::regex migration(
        R"(INSERT\s+INTO\s+schema_migrations\s*\(\s*version\s*\)\s*VALUES\s*\(\s*'([0-9]{4}_[A-Za-z0-9_]+)'\s*\))",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex guard(
        R"(CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([a-z0-9_]+_guard)\b)",
        std::regex::ECMAScript | std::regex::icase);
    SchemaFacts facts;
    facts.sha256 = sha256_of(sql);
    facts.version = "sha256:" + facts.sha256.substr(0, 12);
    facts.migrations = unique_matches(sql, migration);
    if (!facts.migrations.empty()) {
        facts.latest_migration = *std::ranges::max_element(facts.migrations);
    }
    facts.guard_tables = unique_matches(sql, guard);
    if (!facts.guard_tables.empty()) {
        facts.guard_table = facts.guard_tables.back();
    }
    return facts;
}

nlohmann::json migration_report_json(const SchemaFacts& facts, const MigrationReport& report) {
    return {{"command", "migrate"},
            {"ok", true},
            {"outcome", report.applied ? "applied" : "current"},
            {"version", std::string(version_string())},
            {"revision", std::string(build_revision_string())},
            {"schema", schema_json(facts)},
            {"migrations_missing", report.migrations_missing},
            {"migrations_applied", report.migrations_applied},
            {"guard_tables_created", report.guard_tables_created},
            {"migrations_recorded", report.migrations_recorded},
            {"elapsed_ms", report.elapsed_ms}};
}

nlohmann::json migration_error_json(const SchemaFacts& facts, const MigrationError& error) {
    return {{"command", "migrate"},
            {"ok", false},
            {"outcome", "failed"},
            {"version", std::string(version_string())},
            {"revision", std::string(build_revision_string())},
            {"schema", schema_json(facts)},
            {"error", {{"code", error.code}, {"message", error.message}}}};
}

} // namespace lake::control
