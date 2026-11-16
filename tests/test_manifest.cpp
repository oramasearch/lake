// SPDX-License-Identifier: MIT

// `lake manifest` and the lists it publishes (lake#1119, orama-operator#936): the Stripe webhook
// events are exactly the ones the handler dispatches on, the environment is exactly what main.cpp
// reads, and the schema is the db/schema.sql compiled into the binary.

#include "lake/api/manifest.hpp"
#include "lake/api/stripe.hpp"
#include "lake/control/schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {

std::string read_source(const std::string& relative) {
    std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / relative, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

std::set<std::string> matches(const std::string& text, const std::regex& pattern) {
    std::set<std::string> found;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        found.insert((*it)[1].str());
    }
    return found;
}

std::set<std::string> listed_events() {
    return {lake::api::stripe_webhook_events.begin(), lake::api::stripe_webhook_events.end()};
}

// Runs the lake binary with `arguments`, returning its exit status and stdout.
std::pair<int, std::string> run_lake(const std::string& arguments,
                                     const std::string& environment = "") {
    const std::string command =
        environment + "'" + std::string(LAKE_TEST_SERVER_PATH) + "' " + arguments;
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

TEST(StripeWebhookEvents, AreSortedAndUnique) {
    const auto& events = lake::api::stripe_webhook_events;
    EXPECT_TRUE(std::ranges::is_sorted(events));
    EXPECT_EQ(listed_events().size(), events.size());
}

// The handler compares notice.event_type against literals; the list must be exactly those, so an
// event added to the handler and not the list (or the reverse) fails here, not in production.
TEST(StripeWebhookEvents, AreExactlyTheEventsTheHandlerDispatchesOn) {
    const std::string source = read_source("src/api/stripe.cpp");
    ASSERT_FALSE(source.empty());
    const auto compared = matches(source, std::regex(R"re(event_type\s*==\s*"([a-z_.]+)")re"));
    EXPECT_EQ(compared, listed_events());
    // A prefix the handler matches covers at least one listed event.
    for (const auto& prefix :
         matches(source, std::regex(R"re(event_type\.starts_with\("([a-z_.]+)"\))re"))) {
        EXPECT_TRUE(
            std::ranges::any_of(lake::api::stripe_webhook_events,
                                [&](std::string_view event) { return event.starts_with(prefix); }))
            << prefix;
    }
}

TEST(StripeWebhookEvents, EachListedEventIsRead) {
    for (const auto event : lake::api::stripe_webhook_events) {
        const nlohmann::json body = {{"id", "evt_manifest"},
                                     {"type", event},
                                     {"data", {{"object", nlohmann::json::object()}}}};
        const auto notice = lake::api::parse_stripe_event(body.dump(), "lake.test");
        // An empty object may be refused as incomplete, but never as "not a Stripe event".
        if (!notice) {
            EXPECT_NE(notice.error().message, "the body is not a Stripe event") << event;
        } else {
            EXPECT_EQ(notice->event_type, event);
        }
    }
}

TEST(LakeManifest, PublishesTheHandlersStripeEvents) {
    const auto manifest = lake::api::operator_manifest();
    std::vector<std::string> expected(lake::api::stripe_webhook_events.begin(),
                                      lake::api::stripe_webhook_events.end());
    EXPECT_EQ(manifest["stripe"]["webhook_events"].get<std::vector<std::string>>(), expected);
    std::set<std::string> products;
    for (const auto& product : manifest["stripe"]["products"]) {
        products.insert(product["id"].get<std::string>());
    }
    EXPECT_EQ(products,
              (std::set<std::string>{std::string(lake::api::stripe_usage_product),
                                     std::string(lake::api::stripe_private_warehouse_product),
                                     std::string(lake::api::stripe_dedicated_cluster_product)}));
}

// Every variable main.cpp reads (env_or, env_u64) and every getenv in src/ is declared, and nothing
// declared is no longer read. HOME and glibc's MALLOC_MMAP_THRESHOLD_ are the process's, not
// settings anyone supplies to Lake.
TEST(LakeManifest, DeclaresExactlyTheEnvironmentLakeReads) {
    const std::string main_source = read_source("src/main.cpp");
    ASSERT_FALSE(main_source.empty());
    auto read = matches(main_source, std::regex(R"re(env_(?:or|u64)\(\s*"([A-Z0-9_]+)")re"));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "src")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        const std::string text(std::istreambuf_iterator<char>(file), {});
        for (const auto& name : matches(text, std::regex(R"re(getenv\("([A-Za-z0-9_]+)"\))re"))) {
            read.insert(name);
        }
    }
    read.erase("HOME");
    read.erase("MALLOC_MMAP_THRESHOLD_");
    std::set<std::string> declared;
    std::vector<std::string> order;
    for (const auto& variable : lake::api::environment_variables()) {
        declared.insert(std::string(variable.name));
        order.emplace_back(variable.name);
        EXPECT_FALSE(variable.description.empty()) << variable.name;
    }
    EXPECT_TRUE(std::ranges::is_sorted(order));
    EXPECT_EQ(declared.size(), order.size());
    EXPECT_EQ(declared, read);
}

// What the operator renders today and must keep supplying (orama-operator#936); each is declared,
// and the ones a Lake cannot run without are required.
TEST(LakeManifest, MarksWhatTheOperatorMustSupply) {
    const auto manifest = lake::api::operator_manifest();
    std::set<std::string> required;
    std::set<std::string> secret;
    for (const auto& variable : manifest["environment"]) {
        if (variable["required"] == true) {
            required.insert(variable["name"].get<std::string>());
        }
        if (variable["secret"] == true) {
            secret.insert(variable["name"].get<std::string>());
        }
    }
    for (const char* name : {"LAKE_CONTROL_DB_URI",
                             "LAKE_ADMIN_TOKEN",
                             "LAKE_INSTANCE_KEK",
                             "LAKE_STORE_ROOT",
                             "LAKE_PUBLIC_HOST",
                             "ZETTA_EGRESS_ALLOW"}) {
        EXPECT_TRUE(required.contains(name)) << name;
    }
    for (const char* name : {"LAKE_CONTROL_DB_URI",
                             "LAKE_ADMIN_TOKEN",
                             "LAKE_INSTANCE_KEK",
                             "LAKE_CLERK_SECRET_KEY",
                             "LAKE_CLERK_WEBHOOK_SECRET",
                             "LAKE_STRIPE_SECRET_KEY",
                             "LAKE_STRIPE_WEBHOOK_SECRET",
                             "LAKE_DATA_PLANE_TOKEN",
                             "LAKE_OPERATOR_ZETTA_TOKEN",
                             "LAKE_AGENT_SERVICE_TOKEN"}) {
        EXPECT_TRUE(secret.contains(name)) << name;
    }
}

// orama-operator#1176 runs per-workspace library agents only for an image that lists this
// (lake#871).
TEST(LakeManifest, ListsTheFeaturesTheOperatorSwitchesOn) {
    const auto manifest = lake::api::operator_manifest();
    ASSERT_TRUE(manifest.contains("features"));
    ASSERT_TRUE(manifest["features"].is_array());
    const auto& features = manifest["features"];
    EXPECT_NE(std::find(features.begin(), features.end(),
                        nlohmann::json("agent_bus_workspace_credentials")),
              features.end());
    EXPECT_EQ(manifest["manifest_version"], 1) << "the array is additive";
}

TEST(LakeManifest, CarriesTheSchemaFileByteForByte) {
    const std::string file = read_source("db/schema.sql");
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(lake::control::embedded_schema_sql(), file);
    const auto facts = lake::control::schema_facts(file);
    const auto manifest = lake::api::operator_manifest();
    EXPECT_EQ(manifest["schema"]["sha256"], facts.sha256);
    EXPECT_EQ(manifest["schema"]["version"], "sha256:" + facts.sha256.substr(0, 12));
    EXPECT_EQ(manifest["schema"]["latest_migration"], facts.latest_migration);
    EXPECT_EQ(manifest["schema"]["guard_table"], facts.guard_table);
    EXPECT_EQ(manifest["schema"]["migrations"].size(), facts.migrations.size());
}

// The schema's facts as the operator reads them: every recorded migration, numbered and unique,
// and the guard table is the one the newest migration creates, so "guard present" means the newest
// migration committed.
TEST(LakeManifest, TheGuardTableIsTheNewestMigrations) {
    const std::string file = read_source("db/schema.sql");
    const auto facts = lake::control::schema_facts(file);
    const auto recorded = matches(
        file,
        std::regex(
            R"re(INSERT INTO schema_migrations ?\(version\) VALUES \('([0-9]{4}_[A-Za-z0-9_]+)'\))re"));
    EXPECT_EQ(std::set<std::string>(facts.migrations.begin(), facts.migrations.end()), recorded);
    ASSERT_GE(facts.migrations.size(), 2U);
    EXPECT_EQ(facts.latest_migration, *recorded.rbegin());
    std::set<std::string> prefixes;
    for (const auto& version : facts.migrations) {
        EXPECT_TRUE(prefixes.insert(version.substr(0, 4)).second) << version;
    }
    // The text between the previous migration's row and the newest one's creates the guard.
    const auto previous = *std::prev(recorded.end(), 2);
    const auto from = file.find("'" + previous + "'");
    const auto to = file.find("'" + facts.latest_migration + "'");
    ASSERT_NE(from, std::string::npos);
    ASSERT_NE(to, std::string::npos);
    ASSERT_LT(from, to);
    EXPECT_NE(file.substr(from, to - from).find("CREATE TABLE IF NOT EXISTS " + facts.guard_table),
              std::string::npos)
        << facts.guard_table << " is not created by " << facts.latest_migration;
    EXPECT_NE(std::ranges::find(facts.guard_tables, facts.guard_table), facts.guard_tables.end());
}

TEST(LakeManifest, TheCommandPrintsTheDocument) {
    const auto [status, output] = run_lake("manifest");
    ASSERT_EQ(status, 0) << output;
    const auto printed = nlohmann::json::parse(output, nullptr, false);
    ASSERT_FALSE(printed.is_discarded()) << output;
    EXPECT_EQ(printed, lake::api::operator_manifest());
    EXPECT_EQ(printed["manifest_version"], 1);
    EXPECT_EQ(printed["migrate"]["args"], nlohmann::json::array({"migrate"}));
}

// migrate reads only its own arguments: no URI is a usage error with JSON on stdout, before any of
// the server's settings are read (LAKE_HTTP_WORKERS=many would stop the server before it starts).
TEST(LakeMigrate, WithoutADatabaseIsAUsageError) {
    const auto [status, output] =
        run_lake("migrate 2>/dev/null", "env -u LAKE_CONTROL_DB_URI LAKE_HTTP_WORKERS=many ");
    EXPECT_EQ(status, lake::control::migrate_exit_usage);
    const auto printed = nlohmann::json::parse(output, nullptr, false);
    ASSERT_FALSE(printed.is_discarded()) << output;
    EXPECT_EQ(printed["command"], "migrate");
    EXPECT_EQ(printed["ok"], false);
    EXPECT_EQ(printed["outcome"], "failed");
    EXPECT_EQ(printed["error"]["code"], "usage");
    EXPECT_EQ(printed["schema"]["latest_migration"],
              lake::control::schema_facts(lake::control::embedded_schema_sql()).latest_migration);
    EXPECT_EQ(run_lake("migrate --wait-seconds soon --control-db x 2>/dev/null").first,
              lake::control::migrate_exit_usage);
    EXPECT_EQ(run_lake("migrate --verbose 2>/dev/null").first, lake::control::migrate_exit_usage);
}
