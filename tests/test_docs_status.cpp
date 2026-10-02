// docs/design/status.md must agree with what is deployed (lake#448): tools/docs/check-status.py
// compares the page's Deployed cells with the newest committed OpenAPI observation and the
// source registry. These tests run it, and prove it refuses each kind of contradiction.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "temporary_directory.hpp"

extern char** environ;

namespace {

namespace fs = std::filesystem;

int run_check(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(),
                     {LAKE_TEST_PYTHON, LAKE_TEST_SOURCE_DIR "/tools/docs/check-status.py"});
    std::vector<char*> argv;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawn(&pid, argv.front(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

// The newest observation: snapshots are named openapi-<YYYY-MM-DD>.json, so names sort by date.
fs::path newest_snapshot() {
    std::vector<fs::path> snapshots;
    for (const auto& entry :
         fs::directory_iterator(fs::path(LAKE_TEST_SOURCE_DIR) / "docs/deployment/observed")) {
        if (entry.path().filename().string().starts_with("openapi-") &&
            entry.path().extension() == ".json") {
            snapshots.push_back(entry.path());
        }
    }
    std::ranges::sort(snapshots);
    return snapshots.empty() ? fs::path() : snapshots.back();
}

std::string status_page(std::string_view deployed, std::string_view capabilities) {
    return std::string("# Status\n\n## Area status\n\n"
                       "| Area | Deployed | Capabilities |\n|---|---|---|\n| Serving | ") +
           std::string(deployed) + " | " + std::string(capabilities) + " |\n";
}

} // namespace

TEST(DocsStatus, MatchesTheObservedOpenApi) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "0.0.1");
    const fs::path tree = lake::test::fixture_path("status-registry-openapi.json");
    std::ofstream(tree) << lake::api::openapi_document(registry, "0.0.1").dump();

    const fs::path snapshot = newest_snapshot();
    ASSERT_FALSE(snapshot.empty()) << "docs/deployment/observed has no openapi-<date>.json";
    EXPECT_EQ(run_check({"--status",
                         LAKE_TEST_SOURCE_DIR "/docs/design/status.md",
                         "--observed",
                         snapshot.string(),
                         "--registry",
                         tree.string(),
                         "--require-link",
                         snapshot.string()}),
              0)
        << "docs/design/status.md contradicts " << snapshot.filename()
        << "; tools/docs/check-status.py printed why";
}

TEST(DocsStatus, CheckRefusesEachKindOfContradiction) {
    const fs::path observed = lake::test::fixture_path("status-observed.json");
    std::ofstream(observed) << nlohmann::json{{"capabilities", {"server_info"}}}.dump();
    const auto check = [&](std::string_view deployed, std::string_view capabilities) {
        const fs::path page = lake::test::fixture_path("status-page.md");
        std::ofstream(page) << status_page(deployed, capabilities);
        return run_check({"--status", page.string(), "--observed", observed.string()});
    };
    EXPECT_EQ(check("Yes", "`server_info`"), 0);
    EXPECT_NE(check("No", "`server_info`"), 0) << "undeployed, yet the OpenAPI lists it";
    EXPECT_NE(check("Not yet", "`server_info`"), 0) << "'Not yet' is a No";
    EXPECT_NE(check("Yes", "`server_info`, `missing_capability`"), 0)
        << "deployed, yet the OpenAPI lacks one";
    EXPECT_NE(check("Partly", "`server_info`"), 0) << "Deployed must start with Yes or No";
    EXPECT_NE(check("Yes", ""), 0) << "a row names its capabilities or says —";
    EXPECT_EQ(check("Yes", "— (pages)"), 0);

    // A capability the source registry no longer has is a stale row, whatever was observed.
    const fs::path page = lake::test::fixture_path("status-page.md");
    std::ofstream(page) << status_page("Yes", "`server_info`");
    const fs::path registry = lake::test::fixture_path("status-registry.json");
    std::ofstream(registry) << nlohmann::json{{"capabilities", {"create_customer"}}}.dump();
    EXPECT_NE(run_check({"--status", page.string(), "--observed", observed.string(), "--registry",
                         registry.string()}),
              0);
    // The page must link the observation it is held to.
    EXPECT_NE(run_check({"--status", page.string(), "--observed", observed.string(),
                         "--require-link", observed.string()}),
              0);
}
