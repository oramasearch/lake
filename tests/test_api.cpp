// SPDX-License-Identifier: MIT

#include "../src/api/store_repository.hpp"
#include "audit_test_support.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <openssl/hmac.h>

#include <liquid/liquid.hpp>
#include <liquid/version.hpp>

#include "objects/records.hpp"  // from LAKE_LIQUID_SOURCE_DIR/src (see tests/CMakeLists.txt)

#include "api/repository_people.hpp"
#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/liquid_clients.hpp"
#include "lake/api/markdown.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/server.hpp"
#include "lake/api/stripe.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drive_share_links.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/platform_records.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/workspace_audit.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/telemetry/metrics.hpp"
#include "lake/version.hpp"
#include "repository_test_support.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"
#include "workspace_fixture.hpp"

using nlohmann::json;
using namespace lake::api;

extern char** environ;

namespace {

struct Surface {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext ctx;
    Surface() {
        register_core_capabilities(registry, "0.0.1");
        ctx.store = &store;
        ctx.node_name = "test";
        // Hexadecimal, as LAKE_INSTANCE_KEK is: workspace records derive their keys from it.
        ctx.instance_kek = "6c616b652d666f7267652d7265636f7264732d746573742d696e7374616e6365";
        ctx.authorized = true;
        ctx.acting_identity = "admin";
        ctx.operator_credential = true;
    }
    ApiResult call(const std::string& name, json input = json::object()) {
        ctx.request_id.clear();
        const auto capability = registry.find(name);
        if (capability && capability->async_handler) {
            auto completed = liquid::runtime::block_on(registry.call_async(name, input, ctx));
            return completed ? std::move(*completed)
                             : ApiResult::failure("store_error", completed.error().message);
        }
        return registry.call(name, input, ctx);
    }
    // Workspace Zettas that hold lake's records (forge records, lake#341), created on first use.
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces;
    void ready(const std::string& workspace_id) {
        if (!workspaces) {
            workspaces = std::make_unique<lake::test::RecordWorkspaces>(
                std::shared_ptr<lake::control::Store>(&store, [](lake::control::Store*) {}));
            ctx.workspaces = workspaces->service.get();
        }
        workspaces->ready(workspace_id);
    }
    // The workspace a test repository is created in, with its Zetta Ready.
    std::string workspace(const json& customer_id) {
        const std::string id = lake::test::repository_workspace(store, customer_id);
        ready(id);
        return id;
    }
};

std::vector<std::byte> bytes(const std::string& value);

struct RepositoryFixtureStore final : SyncStore {
    std::map<std::string, RepositoryPortableData> imported;
    std::uint64_t unique_bytes = 123;
    bool restored = false;
    bool shared_folder_moved = false;
    std::optional<std::string> blob_contents;
    std::optional<RepositoryArtifactSource> verified_source;
    std::optional<SyncStoreError> artifact_error;
    std::map<std::string, std::optional<RepositoryArtifactSource>> artifact_sources;
    std::string commit_message;
    std::vector<RepositoryWriteOperation> committed_operations;
    std::optional<std::vector<RepositoryTreeEntry>> write_tree;
    // The tree at a given revision, when a test needs one to differ from main's (lake#923).
    std::map<std::string, std::vector<RepositoryTreeEntry>> trees_at;
    std::string conflict_path = "readme.md";
    std::string resolved_conflict;
    std::string conflict_resolution;
    std::vector<std::byte> conflict_blob;
    SyncStoreResult<std::vector<std::string>>
    find_missing(const std::string&, const std::vector<std::string>&) override {
        return std::vector<std::string>{};
    }

    SyncStoreResult<void>
    write(const std::string&, const std::string&, std::vector<std::byte>, std::uint64_t) override {
        return {};
    }

    SyncStoreResult<std::vector<std::byte>> read(const std::string&, const std::string&) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::uint64_t> unique_stored_bytes(const std::string&) override {
        return unique_bytes;
    }

    SyncStoreResult<std::vector<RepositoryContentRef>>
    repository_refs(const std::string&, const std::string&) override {
        return std::vector<RepositoryContentRef>{{"main", "change-1"}};
    }

    SyncStoreResult<std::vector<RepositoryTreeEntry>>
    repository_tree(const std::string&, const std::string&, const std::string& ref) override {
        if (const auto at = trees_at.find(ref); at != trees_at.end()) {
            return at->second;
        }
        const std::string shared_path = shared_folder_moved ? "renamed" : "shared";
        return std::vector<RepositoryTreeEntry>{
            {"readme.md", "blob", "blob-1", 0644, "11111111111111111111111111111111"},
            {"MODEL_CARD.md", "blob", "blob-card", 0644, "44444444444444444444444444444444"},
            {"weights.bin", "blob", "blob-weights", 0644, "55555555555555555555555555555555"},
            {shared_path, "tree", "", 0755, "22222222222222222222222222222222"},
            {shared_path + "/readme.md",
             "blob",
             "blob-2",
             0644,
             "33333333333333333333333333333333"}};
    }

    SyncStoreResult<RepositoryFile> repository_file(const std::string&,
                                                    const std::string&,
                                                    const std::string& revision,
                                                    const std::string& path) override {
        if (path == "MODEL_CARD.md") {
            return RepositoryFile{path, "blob-card", "# Fixture model", 15};
        }
        if (path == "weights.bin") {
            return RepositoryFile{path, "blob-weights", "fixture weights", 15};
        }
        if (path.ends_with(".cpp")) {
            const std::string source = "int main() {\n    return 0;\n}\n";
            return RepositoryFile{path, "blob-source", source, source.size(), bytes(source)};
        }
        if (revision == "change-1") {
            return RepositoryFile{path, "blob-1", "old\nshared\n", 11, bytes("old\nshared\n")};
        }
        return RepositoryFile{path, "blob-2", "new\nshared\n", 11, bytes("new\nshared\n")};
    }

    SyncStoreResult<RepositoryFile> repository_blob(const std::string&,
                                                    const std::string&,
                                                    const std::string& path,
                                                    const std::string& blob) override {
        const std::string contents =
            blob_contents.value_or(blob == "blob-1" ? "old\nshared\n" : "new\nshared\n");
        return RepositoryFile{path, blob, contents, contents.size(), bytes(contents)};
    }

    SyncStoreResult<std::vector<RepositoryChange>> repository_log(const std::string&,
                                                                  const std::string&) override {
        return std::vector<RepositoryChange>{
            {"change-1", {}, "taap:1:ed25519:author", "initial", "2026-09-04T00:00:00Z"}};
    }

    SyncStoreResult<std::vector<RepositoryDiffEntry>>
    repository_diff(const std::string&, const std::string&, const std::string&) override {
        return std::vector<RepositoryDiffEntry>{
            {.path = "readme.md",
             .kind = "modified",
             .changes = {"modified"},
             .old_mode = 0644,
             .new_mode = 0644,
             .old_blob = "blob-1",
             .new_blob = "blob-2",
             .line_hunks = std::vector<RepositoryLineHunk>{{1, 1, 1, 1, {"old"}, {"hello"}}}}};
    }

    SyncStoreResult<RepositoryArtifactPage> repository_artifacts(
        const std::string&, const std::string&, const RepositoryArtifactFilter&) override {
        return RepositoryArtifactPage{
            .items =
                {{.id = "result-1",
                  .action = "action-1",
                  .outputs = {{"build/app", 0755, "blob-3"}},
                  .timing = {.sandbox_ms = 250, .exec_ms = 1000, .hash_ms = 250, .store_ms = 250},
                  .cache_status = "hit",
                  .worker = "worker-1",
                  .observed_at = "2026-09-04T00:00:00Z"}},
            .done = true};
    }

    SyncStoreResult<RepositoryArtifact> repository_artifact(const std::string&,
                                                            const std::string&,
                                                            const std::string& result_id) override {
        if (artifact_error) {
            return std::unexpected(*artifact_error);
        }
        return RepositoryArtifact{
            .id = result_id,
            .action = "action-1",
            .outputs = {{"build/app", 0755, "blob-3"}},
            .timing = {.queued_ms = 1, .sandbox_ms = 2, .exec_ms = 3, .hash_ms = 4, .store_ms = 5},
            .cache_status = "hit",
            .worker = "worker-1",
            .observed_at = "2026-09-04T00:00:00Z",
            .attestations =
                {{"attestation-1",
                  "slsa_provenance/1",
                  "subject-1",
                  "taap:1:ed25519:builder",
                  true,
                  ""},
                 {"attestation-2", "sbom/1", "subject-1", "taap:1:ed25519:builder", true, ""}},
            .trace = RepositoryArtifactTrace{"trace-1", 12, 2, 1, 1, 1},
            .verified_source = artifact_sources.contains(result_id) ? artifact_sources.at(result_id)
                                                                    : verified_source};
    }

    SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string&, const std::string&) override {
        return std::vector<RepositoryConflict>{
            {.id = "conflict-1",
             .base = "blob-base",
             .left = "blob-1",
             .right = "blob-2",
             .region = {.path = conflict_path, .bytes = RepositoryConflictByteRange{0, 3}},
             .kind = "suspicious-merge",
             .explanation = "Both sides changed the greeting.",
             .proposal = "blob-proposal",
             .reasons = {"The edits choose different words."},
             .version_blobs = {"blob-1", "blob-2"}}};
    }

    SyncStoreResult<RepositoryConflictResolution>
    resolve_repository_conflict(const std::string&,
                                const std::string&,
                                const std::string& conflict_id,
                                const std::string& resolution,
                                std::vector<std::byte> blob,
                                RepositoryWriteAuthorizer authorize) override {
        if (authorize) {
            const auto entries =
                write_tree ? SyncStoreResult<std::vector<RepositoryTreeEntry>>(*write_tree)
                           : repository_tree("", "", "main");
            if (const auto allowed = authorize(*entries); !allowed) {
                return std::unexpected(allowed.error());
            }
        }
        resolved_conflict = conflict_id;
        conflict_resolution = resolution;
        conflict_blob = std::move(blob);
        return RepositoryConflictResolution{conflict_id, "blob-resolved", "change-resolved"};
    }

    SyncStoreResult<std::string>
    commit_repository_files(const std::string&,
                            const std::string&,
                            const std::string&,
                            const std::string& message,
                            std::vector<RepositoryWriteOperation> operations,
                            RepositoryWriteAuthorizer authorize) override {
        if (authorize) {
            const auto entries =
                write_tree ? SyncStoreResult<std::vector<RepositoryTreeEntry>>(*write_tree)
                           : repository_tree("", "", "main");
            if (const auto allowed = authorize(*entries); !allowed) {
                return std::unexpected(allowed.error());
            }
        }
        commit_message = message;
        committed_operations = std::move(operations);
        return "drive-change";
    }

    SyncStoreResult<RepositoryChange>
    restore_repository_file(const std::string&,
                            const std::string&,
                            const std::string& change_id,
                            const std::string& path,
                            RepositoryWriteAuthorizer authorize) override {
        if (authorize) {
            const auto entries =
                write_tree ? SyncStoreResult<std::vector<RepositoryTreeEntry>>(*write_tree)
                           : repository_tree("", "", "main");
            if (const auto allowed = authorize(*entries); !allowed) {
                return std::unexpected(allowed.error());
            }
        }
        restored = true;
        return RepositoryChange{"restored-change",
                                {change_id},
                                "taap:1:ed25519:author",
                                "Restore " + path,
                                "2026-09-04T01:00:00Z"};
    }

    SyncStoreResult<RepositoryPortableData>
    export_portable(const std::string&, const std::string&, bool include_git) override {
        RepositoryPortableData output;
        output.history = {{"refs", {{{"name", "main"}, {"change", "change-1"}}}},
                          {"changes",
                           {{{"id", "change-1"},
                             {"parents", json::array()},
                             {"author", "taap:1:ed25519:author"},
                             {"message", "initial"},
                             {"occurred_at", "2026-09-04T00:00:00Z"}}}}};
        output.artifacts = {
            {{"id", "result-1"},
             {"action", "action-1"},
             {"verification", "verified"},
             {"attestations", {{{"issuer", "taap:1:ed25519:builder"}, {"verified", true}}}}}};
        output.git = {{"requested", include_git}};
        return output;
    }

    SyncStoreResult<void> import_portable(const std::string& customer_id,
                                          const std::string& repository_id,
                                          const RepositoryPortableData& data) override {
        imported[customer_id + "/" + repository_id] = data;
        return {};
    }

    SyncStoreResult<RepositoryGitImport>
    import_git(const std::string&, const std::string&, const std::string&) override {
        return RepositoryGitImport{
            .commits = 1, .blobs = 1, .commit_changes = {{"deadbeef", "change-1"}}};
    }

    SyncStoreResult<std::vector<std::pair<std::string, std::string>>>
    refs(const std::string&, const std::string&) override {
        return std::vector<std::pair<std::string, std::string>>{{"main", "change-1"}};
    }

    SyncStoreResult<std::vector<std::byte>>
    find_missing(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<std::byte>> push(const std::string&,
                                                 const std::string&,
                                                 std::span<const std::byte>,
                                                 SyncPushAuthorizer authorize,
                                                 std::uint64_t) override {
        const auto authorized =
            authorize({.kind = SyncPushItemKind::Change, .change_id = "change-refused"}, {});
        if (!authorized) {
            return std::unexpected(authorized.error());
        }
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<std::byte>>
    pull(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::uint64_t> upload_chunks(const std::string&,
                                                 const std::string&,
                                                 std::span<const std::byte>,
                                                 std::uint64_t) override {
        return 0;
    }

    SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string&,
                                                       const std::string&,
                                                       const std::string&,
                                                       std::uint64_t,
                                                       std::uint64_t) override {
        return std::vector<std::byte>{};
    }
};

TEST(Metering, CapabilityReportsExactStorageAndIdempotentBuildCompute) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    surface.ctx.metering = {
        .currency = "USD", .storage_quota_bytes = 100, .compute_second_micros = 5};
    const ApiResult user =
        surface.call("create_user", {{"email", "meter@example.com"}, {"display_name", "Meter"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value.at("id")}, {"name", "meter"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value.at("id")},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value.at("id"))},
         {"name", "usage"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;

    const json input{{"customer_id", customer.value.at("id")}};
    const ApiResult first = surface.call("metering_usage", input);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_FALSE(first.value.at("items").empty());
    // Storage has no hourly history (lake#348); the build result counts from the repository.
    EXPECT_FALSE(first.value.at("items").back().contains("stored_bytes"));
    EXPECT_EQ(first.value.at("summary").at("storage").at("repositories_bytes"), 123);
    EXPECT_EQ(first.value.at("items").front().at("compute_seconds"), 2);
    EXPECT_EQ(first.value.at("cost_micros"), 10);
    EXPECT_TRUE(first.value.at("over_quota"));

    const ApiResult second = surface.call("metering_usage", input);
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(second.value.at("items").front().at("compute_seconds"), 2);

    // Without a registry or workspaces the report names what it could not measure rather than
    // presenting a partial total as complete.
    const json& summary = second.value.at("summary");
    EXPECT_EQ(summary.at("storage").at("repositories_bytes"), 123);
    EXPECT_TRUE(summary.at("storage").at("images_bytes").is_null());
    EXPECT_EQ(summary.at("storage").at("total_bytes"), 123);
    EXPECT_EQ(summary.at("compute").at("repository_build_seconds"), 2);
    EXPECT_TRUE(summary.at("priced"));
    const std::string not_measured = summary.at("not_measured").dump();
    EXPECT_NE(not_measured.find("Container images"), std::string::npos);
    EXPECT_NE(not_measured.find("no workspaces"), std::string::npos);
    EXPECT_NE(not_measured.find("Repository storage over time"), std::string::npos);
}

namespace {

// A workspace Zetta answering list_datasets and list_runs with fixed records.
struct UsageWorkspaceCaller final : lake::control::WorkspaceCaller {
    std::mutex mutex; // concurrent workspace reads may complete on different threads
    std::vector<std::string> capabilities;
    liquid::runtime::task<lake::control::ZettaControlResult<json>>
    call(std::string, std::string, std::string capability, bool, json) override {
        {
            std::lock_guard lock(mutex);
            capabilities.push_back(capability);
        }
        if (capability == "list_datasets") {
            co_return json{{"datasets",
                            json::array({{{"name", "accounts"}, {"size_bytes", 12'000}},
                                         {{"name", "events"}, {"size_bytes", 3'000}}})}};
        }
        if (capability == "list_runs") {
            co_return json{
                {"runs",
                 json::array({// A training run of 90.5 seconds, rounded up to 91.
                              {{"id", "run-1"},
                               {"status", "succeeded"},
                               {"created", "2026-09-04T00:00:00.000Z"},
                               {"updated", "2026-09-04T00:01:30.500Z"}},
                              {{"id", "run-2"},
                               {"status", "failed"},
                               {"created", "2026-09-05T10:00:00.000Z"},
                               {"updated", "2026-09-05T10:00:09.000Z"}},
                              // No usable timestamps: counted as nothing, never as a guess.
                              {{"id", "run-3"}, {"status", "pending"}, {"created", ""}}})}};
        }
        co_return lake::control::ZettaControlResult<json>(
            std::unexpected(lake::control::ZettaControlError{"not_found", capability}));
    }
};

} // namespace

TEST(Metering, UsageIncludesWorkspaceDataImagesAndComputeOutsideRepositories) {
    const std::filesystem::path directory =
        lake::test::fixture_path("metering-workspaces-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(directory);
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    auto store = std::shared_ptr<lake::control::Store>(&surface.store, [](auto*) {});
    auto caller = std::make_shared<UsageWorkspaceCaller>();
    lake::control::WorkspaceService workspaces(
        lake::control::WorkspaceConfig{directory.string(), 5}, store, caller);
    surface.ctx.workspaces = &workspaces;

    const ApiResult user =
        surface.call("create_user", {{"email", "usage@example.com"}, {"display_name", "Usage"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value.at("id")}, {"name", "usage"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string customer_id = customer.value.at("id");
    const auto requested = liquid::runtime::block_on(workspaces.request(customer_id));
    ASSERT_TRUE(requested && *requested);
    ASSERT_TRUE(surface.store.set_service_status(
        (*requested)->service_id, "Ready", "", "http://usage-workspace.test:4800"));
    const auto service = surface.store.get_service((*requested)->service_id);
    ASSERT_TRUE(service);
    std::ofstream(directory / (service->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", "usage-token"}}.dump();
    // A second workspace that is not running is reported as unmeasured, not as zero.
    const auto stopped = surface.store.create_workspace({customer_id, "stopped", "dev", ""});
    ASSERT_TRUE(stopped);

    const auto repository = surface.store.create_repository({customer_id, "app", "", "private",
                                                             lake::test::repository_workspace(
                                                                 surface.store, customer_id)});
    ASSERT_TRUE(repository);
    const auto build = surface.store.create_build({.customer_id = customer_id,
                                                   .workspace_id = (*requested)->workspace_id,
                                                   .repository = repository->id,
                                                   .revision = "main",
                                                   .target = "registry/usage/app:v1"});
    ASSERT_TRUE(build);
    ASSERT_TRUE(surface.store.set_build_status(build->id, {.phase = "Succeeded"}));
    const auto queued = surface.store.create_build({.customer_id = customer_id,
                                                    .workspace_id = (*requested)->workspace_id,
                                                    .repository = repository->id,
                                                    .revision = "main",
                                                    .target = "registry/usage/app:v2"});
    ASSERT_TRUE(queued);

    const ApiResult usage = surface.call("metering_usage", {{"customer_id", customer_id}});
    ASSERT_TRUE(usage.ok) << usage.error.message;
    const json& summary = usage.value.at("summary");
    EXPECT_EQ(summary.at("storage").at("repositories_bytes"), 123);
    // Images are counted from the tags of each routed workspace (lake#383); this space has none
    // (RegistryProtocols.UsageCountsEachImageDigestOnceForItsSpace counts some).
    EXPECT_EQ(summary.at("storage").at("images_bytes"), 0);
    EXPECT_EQ(summary.at("storage").at("workspace_data_bytes"), 15'000);
    EXPECT_EQ(summary.at("storage").at("total_bytes"), 123 + 15'000);
    EXPECT_EQ(summary.at("compute").at("workspace_run_seconds"), 91 + 9);
    EXPECT_EQ(summary.at("compute").at("workspace_runs"), 2);
    EXPECT_EQ(summary.at("compute").at("image_builds"), 1) << "only finished builds count";
    EXPECT_FALSE(summary.at("priced"));
    ASSERT_EQ(summary.at("workspaces").size(), 2U);
    std::size_t measured = 0;
    for (const json& workspace : summary.at("workspaces")) {
        if (workspace.at("measured")) {
            ++measured;
            EXPECT_EQ(workspace.at("datasets"), 2);
        }
    }
    EXPECT_EQ(measured, 1U);
    const std::string not_measured = summary.at("not_measured").dump();
    EXPECT_NE(not_measured.find("Workspace stopped"), std::string::npos) << not_measured;
    EXPECT_NE(not_measured.find("Model files"), std::string::npos);
    EXPECT_EQ(std::ranges::count(caller->capabilities, "list_datasets"), 1);
    EXPECT_EQ(std::ranges::count(caller->capabilities, "list_runs"), 1);

    // The window applies to when a run finished.
    const ApiResult windowed = surface.call("metering_usage",
                                            {{"customer_id", customer_id},
                                             {"from", "2026-09-05T00:00:00Z"},
                                             {"to", "2026-09-05T10:00:00Z"}});
    ASSERT_TRUE(windowed.ok) << windowed.error.message;
    EXPECT_EQ(windowed.value.at("summary").at("compute").at("workspace_run_seconds"), 9);
    std::filesystem::remove_all(directory);
}

TEST(Metering, CustomerQuotaCapabilityOverridesTheProcessDefault) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    surface.ctx.metering.storage_quota_bytes = 100;
    const ApiResult user =
        surface.call("create_user", {{"email", "quota@example.com"}, {"display_name", "Quota"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value.at("id")}, {"name", "quota"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string customer_id = customer.value.at("id");
    const ApiResult configured =
        surface.call("set_storage_quota", {{"customer_id", customer_id}, {"quota_bytes", 10}});
    ASSERT_TRUE(configured.ok) << configured.error.message;
    EXPECT_EQ(configured.value.at("quota_bytes"), 10);
    const ApiResult usage = surface.call("metering_usage", {{"customer_id", customer_id}});
    ASSERT_TRUE(usage.ok) << usage.error.message;
    EXPECT_EQ(usage.value.at("quota_bytes"), 10);
    EXPECT_TRUE(usage.value.at("over_quota"));
}

// create_membership, accept_invitation and create_grant sign their own attestation and keep its
// digest as the evidence; a digest that is not lake's is refused at its field and writes nothing
// (lake#602).
TEST(Organizations, GrantsAndMembershipsKeepTheDigestOfAStatementLakeSigned) {
    Surface surface;
    const ApiResult user = surface.call("create_user",
                                        {{"taap_id", "taap:1:ed25519:attested"},
                                         {"email", "attested@example.com"},
                                         {"display_name", "Attested"}});
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Attested"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const auto recomputed = [](const json& attestation) {
        return "sha256:" + lake::control::drive_sha256_hex(json{
                               {"statement", attestation["statement"]},
                               {"signer", attestation["signer"]},
                               {"signature",
                                attestation["signature"]}}.dump());
    };

    // A membership: a made-up digest is refused and nothing is written; without one, lake signs.
    const json membership_input{{"organization_id", organization.value["id"]},
                                {"principal_type", "user"},
                                {"principal_id", user.value["id"]},
                                {"principal_taap_id", user.value["taap_id"]},
                                {"role", "member"}};
    json made_up = membership_input;
    made_up["attestation_digest"] = "sha512-256:membership";
    const ApiResult refused = surface.call("create_membership", made_up);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/attestation_digest");
    EXPECT_NE(refused.error.message.find("leave attestation_digest out"), std::string::npos);
    const ApiResult none =
        surface.call("list_memberships", {{"organization_id", organization.value["id"]}});
    ASSERT_TRUE(none.ok) << none.error.message;
    EXPECT_TRUE(none.value["items"].empty()) << "a refused digest writes nothing";
    const ApiResult membership = surface.call("create_membership", membership_input);
    ASSERT_TRUE(membership.ok) << membership.error.message;
    const json& signed_membership = membership.value["attestation"];
    const std::string membership_digest = signed_membership["digest"];
    EXPECT_TRUE(membership_digest.starts_with("sha256:")) << membership_digest;
    EXPECT_EQ(membership.value["attestation_digest"], membership_digest);
    EXPECT_EQ(recomputed(signed_membership), membership_digest);
    EXPECT_EQ(signed_membership["statement"]["type"], "lake.org_membership/1");
    EXPECT_EQ(signed_membership["statement"]["role"], "member");
    EXPECT_EQ(signed_membership["statement"]["principal_taap_id"], "taap:1:ed25519:attested");
    EXPECT_EQ(signed_membership["statement"]["granted_by"], "admin");
    EXPECT_EQ(signed_membership["signer"], signed_membership["statement"]["issuer"]);
    EXPECT_FALSE(signed_membership["signature"].get<std::string>().empty());
    const ApiResult read = surface.call("get_membership", {{"id", membership.value["id"]}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["attestation_digest"], membership_digest);

    // A grant on the organization: the same rule, and lake's own digest is accepted.
    const auto object = surface.store.find_policy_object("organization", organization.value["id"]);
    ASSERT_TRUE(object) << object.error().message;
    const json grant_input{{"principal_taap_id", "taap:1:ed25519:reader"},
                           {"object_id", object->id},
                           {"role", "read"}};
    json made_up_grant = grant_input;
    made_up_grant["attestation_digest"] = "sha512-256:grant";
    const ApiResult refused_grant = surface.call("create_grant", made_up_grant);
    EXPECT_EQ(refused_grant.error.code, "invalid_input");
    EXPECT_EQ(refused_grant.error.path, "/attestation_digest");
    const ApiResult grant = surface.call("create_grant", grant_input);
    ASSERT_TRUE(grant.ok) << grant.error.message;
    const json& signed_grant = grant.value["attestation"];
    EXPECT_EQ(grant.value["attestation_digest"], signed_grant["digest"]);
    EXPECT_EQ(recomputed(signed_grant), signed_grant["digest"].get<std::string>());
    EXPECT_EQ(signed_grant["statement"]["type"], "lake.grant/1");
    EXPECT_EQ(signed_grant["statement"]["object_id"], object->id);
    // The digest lake answered names this grant only: for another role it is refused; for the same
    // grant it passes the check and the store answers that the grant exists.
    json other_role = grant_input;
    other_role["role"] = "write";
    other_role["attestation_digest"] = signed_grant["digest"];
    EXPECT_EQ(surface.call("create_grant", other_role).error.path, "/attestation_digest");
    json repeated = grant_input;
    repeated["attestation_digest"] = signed_grant["digest"];
    EXPECT_EQ(surface.call("create_grant", repeated).error.code, "conflict");

    // An invitation accepted on someone's behalf: the statement names the invitation and its role.
    const ApiResult guest = surface.call("create_user",
                                         {{"taap_id", "taap:1:ed25519:guest"},
                                          {"email", "guest@example.com"},
                                          {"display_name", "Guest"}});
    ASSERT_TRUE(guest.ok) << guest.error.message;
    const ApiResult invitation = surface.call("create_invitation",
                                              {{"organization_id", organization.value["id"]},
                                               {"invitee_type", "email"},
                                               {"invitee", "guest@example.com"},
                                               {"role", "admin"},
                                               {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(invitation.ok) << invitation.error.message;
    const json accept_input{{"invitation_id", invitation.value["id"]},
                            {"principal_type", "user"},
                            {"principal_id", guest.value["id"]},
                            {"principal_taap_id", guest.value["taap_id"]}};
    json made_up_accept = accept_input;
    made_up_accept["attestation_digest"] = "sha512-256:accepted";
    EXPECT_EQ(surface.call("accept_invitation", made_up_accept).error.path, "/attestation_digest");
    const ApiResult accepted = surface.call("accept_invitation", accept_input);
    ASSERT_TRUE(accepted.ok) << accepted.error.message;
    const json& signed_accept = accepted.value["attestation"];
    EXPECT_EQ(accepted.value["attestation_digest"], signed_accept["digest"]);
    EXPECT_EQ(recomputed(signed_accept), signed_accept["digest"].get<std::string>());
    EXPECT_EQ(signed_accept["statement"]["invitation_id"], invitation.value["id"]);
    EXPECT_EQ(signed_accept["statement"]["role"], "admin");
}

TEST(Organizations, MemberMutationCapabilitiesUseTheRegistry) {
    Surface surface;
    const ApiResult first = surface.call("create_user",
                                         {{"taap_id", "taap:1:ed25519:first-owner"},
                                          {"email", "first-owner@example.com"},
                                          {"display_name", "First owner"}});
    const ApiResult second = surface.call("create_user",
                                          {{"taap_id", "taap:1:ed25519:second-owner"},
                                           {"email", "second-owner@example.com"},
                                           {"display_name", "Second owner"}});
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Members"}});
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    ASSERT_TRUE(organization.ok);
    const auto create = [&](const ApiResult& user, const std::string& role) {
        return surface.call("create_membership",
                            {{"organization_id", organization.value.at("id")},
                             {"principal_type", "user"},
                             {"principal_id", user.value.at("id")},
                             {"principal_taap_id", user.value.at("taap_id")},
                             {"role", role}});
    };
    const ApiResult first_membership = create(first, "owner");
    const ApiResult second_membership = create(second, "owner");
    ASSERT_TRUE(first_membership.ok) << first_membership.error.message;
    ASSERT_TRUE(second_membership.ok) << second_membership.error.message;
    const ApiResult changed = surface.call(
        "set_member_role", {{"id", first_membership.value.at("id")}, {"role", "admin"}});
    ASSERT_TRUE(changed.ok) << changed.error.message;
    EXPECT_EQ(changed.value.at("role"), "admin");
    const ApiResult removed =
        surface.call("remove_member", {{"id", first_membership.value.at("id")}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_TRUE(removed.value.at("removed"));
}

class TemporaryRepositoryRoot {
public:
    TemporaryRepositoryRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("web-ui-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryRepositoryRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class TemporarySyncRoot {
public:
    TemporarySyncRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("sync-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporarySyncRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(const std::string& value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::string base64_bytes(std::span<const std::byte> input) {
    if (input.empty()) {
        return {};
    }
    std::string output(4 * ((input.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    output.resize(static_cast<std::size_t>(size));
    return output;
}

liquid::attestation sign_attestation(liquid::attestation value) {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    value.issuer = signer->identity();
    auto body = liquid::objects::encode_attestation_body(value);
    EXPECT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    value.signature = std::move(*signature);
    return value;
}

constexpr std::string_view drive_kek =
    "6472697665207368617265206c696e6b7320696e20776f726b73706163657321";

// Drive links live in workspace Zettas (lake#350): the surface's store with Ready workspaces.
struct DriveWorkspaces : lake::test::RecordWorkspaces {
    explicit DriveWorkspaces(Surface& surface)
        : RecordWorkspaces(std::shared_ptr<lake::control::Store>(&surface.store,
                                                                  [](lake::control::Store*) {})) {
        surface.ctx.workspaces = service.get();
        surface.ctx.instance_kek = std::string(drive_kek);
    }
};

std::string share_token_hash(std::string_view token) {
    static constexpr std::string_view key = "lake-drive-share-link/1";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(token.data()),
         token.size(),
         digest,
         &size);
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string output;
    output.reserve(size * 2);
    for (unsigned index = 0; index < size; ++index) {
        output += alphabet[digest[index] >> 4U];
        output += alphabet[digest[index] & 0x0fU];
    }
    return output;
}

std::vector<std::byte> sync_bytes(std::string_view value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::uint32_t test_u32_le(const std::array<std::byte, 49>& header, std::size_t offset) {
    return std::to_integer<std::uint32_t>(header[offset]) |
           std::to_integer<std::uint32_t>(header[offset + 1]) << 8U |
           std::to_integer<std::uint32_t>(header[offset + 2]) << 16U |
           std::to_integer<std::uint32_t>(header[offset + 3]) << 24U;
}

std::uint64_t unique_pack_raw_bytes(const std::filesystem::path& packs) {
    std::set<std::string> digests;
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(packs)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pack") {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        input.seekg(16);
        for (;;) {
            std::array<std::byte, 49> header{};
            input.read(reinterpret_cast<char*>(header.data()), header.size());
            if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
                std::string_view(reinterpret_cast<const char*>(header.data()), 6) != "LQFRAM") {
                break;
            }
            const std::string digest(reinterpret_cast<const char*>(header.data() + 16), 33);
            if (digests.insert(digest).second) {
                total += test_u32_le(header, 8);
            }
            input.seekg(test_u32_le(header, 12), std::ios::cur);
        }
    }
    return total;
}

std::vector<std::string> tree_fingerprint(const liquid::tree& tree) {
    std::vector<std::string> output;
    output.reserve(tree.entries.size());
    for (const auto& entry : tree.entries) {
        output.push_back(entry.name + ":" + liquid::to_string(entry.ref));
    }
    std::ranges::sort(output);
    return output;
}

const liquid::sync_ref_outcome* reference_outcome(const liquid::sync_report& report,
                                                  std::string_view name) {
    const auto outcome =
        std::ranges::find(report.references, name, &liquid::sync_ref_outcome::name);
    return outcome == report.references.end() ? nullptr : &*outcome;
}

bool send_socket_bytes(int socket_fd, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t count = ::send(socket_fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::size_t content_length_of(std::string_view header) {
    constexpr std::string_view name = "Content-Length:";
    const std::size_t start = header.find(name);
    if (start == std::string_view::npos) {
        return 0;
    }
    const std::size_t value_start = start + name.size();
    const std::size_t value_end = header.find("\r\n", value_start);
    return static_cast<std::size_t>(
        std::stoull(std::string(header.substr(value_start, value_end - value_start))));
}

std::string read_http_request(int socket_fd) {
    std::string request;
    char buffer[4096];
    std::size_t expected = 0;
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return request;
        }
        request.append(buffer, static_cast<std::size_t>(count));
        const std::size_t header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos && expected == 0) {
            expected = header_end + 4 + content_length_of(request.substr(0, header_end + 4));
        }
        if (expected != 0 && request.size() >= expected) {
            return request;
        }
    }
}

class RecordingProxy {
public:
    explicit RecordingProxy(unsigned short upstream_port) : upstream_port_(upstream_port) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ < 0 ||
            ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 16) != 0) {
            return;
        }
        socklen_t size = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~RecordingProxy() {
        stop();
    }

    [[nodiscard]] unsigned short port() const {
        return port_;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (wake >= 0) {
            static_cast<void>(
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            ::close(wake);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ >= 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }

    [[nodiscard]] std::vector<std::byte> push_body() const {
        std::lock_guard lock(mutex_);
        return push_body_;
    }

private:
    void run() {
        while (!stopped_.load()) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            if (stopped_.load()) {
                ::close(client);
                break;
            }
            const std::string request = read_http_request(client);
            const std::size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos &&
                request.substr(0, header_end).find("/sync/push ") != std::string::npos) {
                const auto* first =
                    reinterpret_cast<const std::byte*>(request.data() + header_end + 4);
                std::lock_guard lock(mutex_);
                push_body_.assign(first, first + request.size() - header_end - 4);
            }
            const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(upstream_port_);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (upstream >= 0 &&
                ::connect(upstream, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                send_socket_bytes(upstream, request)) {
                std::string response;
                char buffer[4096];
                for (;;) {
                    const ssize_t count = ::recv(upstream, buffer, sizeof(buffer), 0);
                    if (count <= 0) {
                        break;
                    }
                    response.append(buffer, static_cast<std::size_t>(count));
                }
                static_cast<void>(send_socket_bytes(client, response));
            }
            if (upstream >= 0) {
                ::close(upstream);
            }
            ::close(client);
        }
    }

    unsigned short upstream_port_{};
    int listener_{-1};
    unsigned short port_{};
    std::atomic<bool> stopped_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::byte> push_body_;
};

liquid::platform host_platform() {
#if defined(__APPLE__)
    return {"macos", "arm64"};
#elif defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
}

std::string
http_get(unsigned short port, std::string_view path, std::string_view authorization = {}) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }

    std::string request =
        "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (!authorization.empty())
        request += "Authorization: " + std::string(authorization) + "\r\n";
    request += "\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string http_request(
    unsigned short port,
    std::string_view method,
    std::string_view path,
    std::string_view body,
    std::string_view content_type,
    std::string_view cookie = {},
    std::string_view authorization = {},
    std::string_view host = "localhost",
    std::initializer_list<std::pair<std::string_view, std::string_view>> extra_headers = {},
    std::optional<std::size_t> declared_body_bytes = std::nullopt) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    if (declared_body_bytes) {
        timeval timeout{1, 0};
        static_cast<void>(
            ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    }
    std::string request = std::string(method) + " " + std::string(path) +
                          " HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n";
    request += "Content-Type: " + std::string(content_type) + "\r\n";
    request +=
        "Content-Length: " + std::to_string(declared_body_bytes.value_or(body.size())) + "\r\n";
    if (!cookie.empty())
        request += "Cookie: " + std::string(cookie) + "\r\n";
    if (!authorization.empty())
        request += "Authorization: " + std::string(authorization) + "\r\n";
    for (const auto& [name, value] : extra_headers) {
        request += std::string(name) + ": " + std::string(value) + "\r\n";
    }
    request += "\r\n";
    if (!send_socket_bytes(socket_fd, request) || !send_socket_bytes(socket_fd, body)) {
        ::close(socket_fd);
        return {};
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string response_header(std::string_view response, std::string_view name) {
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const std::size_t start = response.find(prefix);
    if (start == std::string_view::npos)
        return {};
    const std::size_t value_start = start + prefix.size();
    const std::size_t end = response.find("\r\n", value_start);
    return std::string(response.substr(value_start, end - value_start));
}

json response_json(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return json();
    }
    return json::parse(response.substr(separator + 4), nullptr, false);
}

// The `refs` capability's items for one repository, read over REST. Liquid's repository is the
// only record of refs; lake keeps no copy (lake#319).
json served_refs(unsigned short port,
                 const std::string& repository_id,
                 std::string_view authorization) {
    const json body =
        response_json(http_get(port, "/v1/refs?repository_id=" + repository_id, authorization));
    if (!body.is_object() || !body.contains("value")) {
        return body;
    }
    return body.at("value").at("items");
}

std::string response_body(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return {};
    }
    return std::string(response.substr(separator + 4));
}

std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

unsigned short unused_loopback_port() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return 0;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        ::close(socket_fd);
        return 0;
    }
    ::close(socket_fd);
    return ntohs(address.sin_port);
}

class LakeProcess {
public:
    LakeProcess(unsigned short port,
                const std::string& node,
                const std::string& database_uri,
                const std::filesystem::path& store_root,
                const std::vector<std::string>& environment = {}) {
        std::vector<std::string> arguments{
            LAKE_TEST_SERVER_PATH,
            "--listen",
            "127.0.0.1:" + std::to_string(port),
            "--node",
            node,
            "--control-db",
            database_uri,
            "--store-root",
            store_root.string(),
            "--admin-token",
            "replication-admin",
            "--instance-kek",
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
        };
        std::vector<char*> raw;
        raw.reserve(arguments.size() + 1);
        for (std::string& argument : arguments) {
            raw.push_back(argument.data());
        }
        raw.push_back(nullptr);
        // The parent's environment, then `environment` (NAME=value), which wins.
        std::vector<std::string> variables = environment;
        for (char** entry = environ; *entry != nullptr; ++entry) {
            const std::string_view variable(*entry);
            const std::string_view name = variable.substr(0, variable.find('='));
            if (std::ranges::none_of(environment, [name](const std::string& given) {
                    return given.starts_with(std::string(name) + "=");
                })) {
                variables.emplace_back(variable);
            }
        }
        std::vector<char*> raw_environment;
        raw_environment.reserve(variables.size() + 1);
        for (std::string& variable : variables) {
            raw_environment.push_back(variable.data());
        }
        raw_environment.push_back(nullptr);
        if (::posix_spawn(
                &pid_, raw.front(), nullptr, nullptr, raw.data(), raw_environment.data()) != 0) {
            pid_ = -1;
            return;
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            const std::string ready = http_request(port, "GET", "/readyz", "", "application/json");
            if (ready.find(" 200 ") != std::string::npos) {
                ready_ = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ~LakeProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            static_cast<void>(::waitpid(pid_, nullptr, 0));
        }
    }

    [[nodiscard]] bool ready() const {
        return ready_;
    }

private:
    pid_t pid_{-1};
    bool ready_ = false;
};

} // namespace

TEST(RegistryProtocols, OciClientPushesAndPullsTwoLayersByteIdentical) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "oci-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer registry-admin";

    const std::string ping = http_request(server.port(), "GET", "/v2/", "", "application/json");
    ASSERT_NE(ping.find(" 200 "), std::string::npos) << ping;
    const std::string unauthorized = http_request(
        server.port(), "POST", "/v2/lake-fixture/blobs/uploads/", "", "application/octet-stream");
    EXPECT_NE(unauthorized.find(" 401 "), std::string::npos) << unauthorized;

    const auto push_blob = [&](const std::string& contents) {
        const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
        const std::string started = http_request(server.port(),
                                                 "POST",
                                                 "/v2/lake-fixture/blobs/uploads/",
                                                 "",
                                                 "application/octet-stream",
                                                 {},
                                                 authorization);
        EXPECT_NE(started.find(" 202 "), std::string::npos) << started;
        const std::string location = response_header(started, "Location");
        EXPECT_FALSE(location.empty()) << started;
        const std::string completed = http_request(server.port(),
                                                   "PUT",
                                                   location + "?digest=" + digest,
                                                   contents,
                                                   "application/octet-stream",
                                                   {},
                                                   authorization);
        EXPECT_NE(completed.find(" 201 "), std::string::npos) << completed;
        EXPECT_EQ(response_header(completed, "Docker-Content-Digest"), digest);
        return digest;
    };

    // LSR-192 (lake#998): an upload started in one repository cannot be continued in another.
    {
        const std::string started = http_request(server.port(),
                                                 "POST",
                                                 "/v2/lake-fixture/blobs/uploads/",
                                                 "",
                                                 "application/octet-stream",
                                                 {},
                                                 authorization);
        const std::string location = response_header(started, "Location");
        const std::string upload = location.substr(location.rfind('/') + 1);
        const std::string contents = "elsewhere";
        const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
        const std::string crossed = http_request(server.port(),
                                                 "PUT",
                                                 "/v2/other-fixture/blobs/uploads/" + upload +
                                                     "?digest=" + digest,
                                                 contents,
                                                 "application/octet-stream",
                                                 {},
                                                 authorization);
        EXPECT_NE(crossed.find(" 404 "), std::string::npos) << crossed;
        EXPECT_NE(crossed.find("BLOB_UPLOAD_UNKNOWN"), std::string::npos) << crossed;
    }

    const std::string config = R"({"architecture":"arm64","os":"darwin"})";
    const std::string first_layer("lake layer one\0with binary", 26);
    const std::string second_layer = "lake layer two has different bytes\n";
    const std::string config_digest = push_blob(config);
    const std::string first_digest = push_blob(first_layer);
    const std::string second_digest = push_blob(second_layer);
    const json manifest{{"schemaVersion", 2},
                        {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                        {"config",
                         {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                          {"digest", config_digest},
                          {"size", config.size()}}},
                        {"layers",
                         {{{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                           {"digest", first_digest},
                           {"size", first_layer.size()}},
                          {{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                           {"digest", second_digest},
                           {"size", second_layer.size()}}}}};
    const std::string manifest_text = manifest.dump();
    const std::string published = http_request(server.port(),
                                               "PUT",
                                               "/v2/lake-fixture/manifests/latest",
                                               manifest_text,
                                               "application/vnd.oci.image.manifest.v1+json",
                                               {},
                                               authorization);
    ASSERT_NE(published.find(" 201 "), std::string::npos) << published;

    const std::string pulled_manifest = http_request(
        server.port(), "GET", "/v2/lake-fixture/manifests/latest", "", "application/json");
    EXPECT_EQ(response_body(pulled_manifest), manifest_text);
    for (const auto& [digest, expected] : std::array<std::pair<std::string, std::string>, 2>{
             std::pair{first_digest, first_layer}, std::pair{second_digest, second_layer}}) {
        const std::string pulled = http_request(server.port(),
                                                "GET",
                                                "/v2/lake-fixture/blobs/" + digest,
                                                "",
                                                "application/octet-stream");
        EXPECT_EQ(response_body(pulled), expected);
    }
    const std::string cross_repository = http_request(server.port(),
                                                      "GET",
                                                      "/v2/public-alias/blobs/" + first_digest,
                                                      "",
                                                      "application/octet-stream");
    EXPECT_NE(cross_repository.find(" 404 "), std::string::npos) << cross_repository;
    server.stop();
}

TEST(RegistryProtocols, OciUploadStreamsBodiesLargerThanTheGeneralRequestLimit) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "large-oci-upload-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer registry-admin";
    const std::string contents(64 * 1024 * 1024 + 4096, 'L');
    const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);

    const std::string started = http_request(server.port(),
                                             "POST",
                                             "/v2/large-layer/blobs/uploads/",
                                             "",
                                             "application/octet-stream",
                                             {},
                                             authorization);
    ASSERT_NE(started.find(" 202 "), std::string::npos) << started;
    const std::string location = response_header(started, "Location");
    ASSERT_FALSE(location.empty()) << started;
    const std::string completed = http_request(server.port(),
                                               "PUT",
                                               location + "?digest=" + digest,
                                               contents,
                                               "application/octet-stream",
                                               {},
                                               authorization);
    EXPECT_NE(completed.find(" 201 "), std::string::npos) << completed;
    EXPECT_EQ(response_header(completed, "Docker-Content-Digest"), digest);
    server.stop();
}

TEST(RegistryProtocols, OciUploadHonoursChunkRangesBeforeAnEmptyFinalPut) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "chunked-oci-upload-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer registry-admin";
    const std::string contents = "chunk-onechunk-two";
    const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
    const std::string started = http_request(server.port(),
                                             "POST",
                                             "/v2/chunked-layer/blobs/uploads/",
                                             "",
                                             "application/octet-stream",
                                             {},
                                             authorization);
    ASSERT_NE(started.find(" 202 "), std::string::npos) << started;
    const std::string location = response_header(started, "Location");
    ASSERT_FALSE(location.empty()) << started;

    const std::string first = http_request(server.port(),
                                           "PATCH",
                                           location,
                                           "chunk-one",
                                           "application/octet-stream",
                                           {},
                                           authorization,
                                           "localhost",
                                           {{"Content-Range", "0-8"}});
    EXPECT_NE(first.find(" 202 "), std::string::npos) << first;
    EXPECT_EQ(response_header(first, "Range"), "0-8");
    const std::string second = http_request(server.port(),
                                            "PATCH",
                                            location,
                                            "chunk-two",
                                            "application/octet-stream",
                                            {},
                                            authorization,
                                            "localhost",
                                            {{"Content-Range", "9-17"}});
    EXPECT_NE(second.find(" 202 "), std::string::npos) << second;
    EXPECT_EQ(response_header(second, "Range"), "0-17");
    const std::string completed = http_request(server.port(),
                                               "PUT",
                                               location + "?digest=" + digest,
                                               "",
                                               "application/octet-stream",
                                               {},
                                               authorization);
    EXPECT_NE(completed.find(" 201 "), std::string::npos) << completed;
    EXPECT_EQ(response_header(completed, "Docker-Content-Digest"), digest);
    server.stop();
}

TEST(RegistryProtocols, OciUploadLimitReturnsARegistrySizeError) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "bounded-oci-upload-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    options.max_registry_blob_bytes = 8;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer registry-admin";
    const std::string started = http_request(server.port(),
                                             "POST",
                                             "/v2/bounded-layer/blobs/uploads/",
                                             "",
                                             "application/octet-stream",
                                             {},
                                             authorization);
    ASSERT_NE(started.find(" 202 "), std::string::npos) << started;
    const std::string location = response_header(started, "Location");
    ASSERT_FALSE(location.empty()) << started;
    const std::string refused = http_request(server.port(),
                                             "PUT",
                                             location + "?digest=sha256:" + std::string(64, '0'),
                                             "nine-byte",
                                             "application/octet-stream",
                                             {},
                                             authorization);
    EXPECT_NE(refused.find(" 413 "), std::string::npos) << refused;
    EXPECT_EQ(response_json(refused)["errors"][0]["code"], "BLOB_UPLOAD_INVALID");
    EXPECT_NE(response_json(refused)["errors"][0]["message"].get<std::string>().find("8 bytes"),
              std::string::npos);
    server.stop();
}

TEST(HttpServer, NonRegistryJsonBodyLargerThan64MiBIsRejected) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "bounded-json-test";
    options.admin_token = "admin";
    options.instance_kek = "bounded-json-instance-kek";
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    // Reject a declared oversized body before the caller uploads it. An ordinary capability
    // now uses the 1 MiB default, and the server may close as soon as it sends its refusal.
    const std::string refused = http_request(server.port(),
                                             "POST",
                                             "/v1/create_customer",
                                             {},
                                             "application/json",
                                             {},
                                             "Bearer admin",
                                             "localhost",
                                             {},
                                             64 * 1024 * 1024 + 14);
    EXPECT_NE(refused.find(" 413 "), std::string::npos) << refused;
    EXPECT_EQ(response_json(refused)["error"]["code"], "size_invalid");
    EXPECT_NE(response_json(refused)["error"]["message"].get<std::string>().find("1048576"),
              std::string::npos);
    server.stop();
}

TEST(RegistryProtocols, BuildPushCredentialIsLimitedToItsCustomerPrefix) {
    TemporarySyncRoot temporary;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user(
        {"taap:1:ed25519:registry-owner", "registry-owner@example.test", "Registry owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "tenant-one"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = store->create_repository({customer->id, "source", "", "private",
                                                      lake::test::repository_workspace(
                                                          *store, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    lake::test::RecordWorkspaces workspaces(store);
    const std::string build_workspace = workspaces.create(customer->id, "builds");
    const auto build = store->create_build({.customer_id = customer->id,
                                            .workspace_id = build_workspace,
                                            .repository = repository->id,
                                            .revision = "main",
                                            .target = "registry/tenant-one/service:v1"});
    ASSERT_TRUE(build) << build.error().message;
    ASSERT_TRUE(store->set_build_status(
        build->id,
        {.phase = "Running",
         .token_hashes = {{"push", lake::control::identity_crypto::sha256_hex("scoped-push")}}}));
    const auto pull = store->create_token(
        {user->id, "lake", "registry pull", "operator provisioned", {"registry_pull"}, 300, false});
    ASSERT_TRUE(pull) << pull.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "scoped-oci-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    options.store = store;
    options.workspaces = workspaces.service;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string basic = "Basic bGFrZTpzY29wZWQtcHVzaA==";

    const std::string allowed = http_request(server.port(),
                                             "POST",
                                             "/v2/tenant-one/service/blobs/uploads/",
                                             "",
                                             "application/octet-stream",
                                             {},
                                             basic);
    EXPECT_NE(allowed.find(" 202 "), std::string::npos) << allowed;
    // The first push routed the repository to the build's workspace (lake#383).
    const auto routed = store->get_oci_repository_route(customer->id, "service");
    ASSERT_TRUE(routed) << routed.error().message;
    EXPECT_EQ(routed->workspace_id, build_workspace);
    const std::string refused = http_request(server.port(),
                                             "POST",
                                             "/v2/tenant-two/service/blobs/uploads/",
                                             "",
                                             "application/octet-stream",
                                             {},
                                             basic);
    EXPECT_NE(refused.find(" 403 "), std::string::npos) << refused;
    const std::string private_anonymous = http_request(
        server.port(), "GET", "/v2/tenant-one/service/manifests/latest", "", "application/json");
    EXPECT_NE(private_anonymous.find(" 401 "), std::string::npos) << private_anonymous;
    const std::string private_authorized = http_request(server.port(),
                                                        "GET",
                                                        "/v2/tenant-one/service/manifests/latest",
                                                        "",
                                                        "application/json",
                                                        {},
                                                        basic);
    EXPECT_NE(private_authorized.find(" 404 "), std::string::npos) << private_authorized;
    const std::string customer_pull = http_request(server.port(),
                                                   "GET",
                                                   "/v2/tenant-one/service/manifests/latest",
                                                   "",
                                                   "application/json",
                                                   {},
                                                   "Bearer " + pull->secret);
    EXPECT_NE(customer_pull.find(" 404 "), std::string::npos) << customer_pull;
    const std::string foreign_pull = http_request(server.port(),
                                                  "GET",
                                                  "/v2/tenant-two/service/manifests/latest",
                                                  "",
                                                  "application/json",
                                                  {},
                                                  "Bearer " + pull->secret);
    EXPECT_NE(foreign_pull.find(" 403 "), std::string::npos) << foreign_pull;
    server.stop();
}

TEST(RegistryProtocols, UsageCountsEachImageDigestOnceForItsSpace) {
    TemporarySyncRoot temporary;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user(
        {"taap:1:ed25519:registry-usage", "registry-usage@example.test", "Registry usage"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "tenant-one"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto other_user = store->create_user(
        {"taap:1:ed25519:registry-usage-two", "registry-usage-two@example.test", "Usage two"});
    ASSERT_TRUE(other_user) << other_user.error().message;
    const auto other = store->create_customer({"individual", other_user->id, "", "tenant-two"});
    ASSERT_TRUE(other) << other.error().message;
    // Tenant images live in their routed workspace (lake#383): two of the space's, and another
    // space's.
    lake::test::RecordWorkspaces workspaces(store);
    const std::string images = workspaces.create(customer->id, "images");
    const std::string cache = workspaces.create(customer->id, "cache");
    const std::string elsewhere = workspaces.create(other->id, "home");

    ServerOptions options;
    options.port = 0;
    options.node_name = "usage-oci-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    options.store = store;
    options.workspaces = workspaces.service;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer registry-admin";
    for (const auto& [space, workspace, name] :
         {std::tuple{customer->id, images, std::string("service")},
          std::tuple{customer->id, cache, std::string("cache/service")},
          std::tuple{other->id, elsewhere, std::string("other")}}) {
        const std::string created = http_request(
            server.port(), "POST", "/v1/create_image_repository",
            json{{"customer_id", space}, {"workspace_id", workspace}, {"name", name}}.dump(),
            "application/json", {}, authorization);
        ASSERT_NE(created.find(" 200 "), std::string::npos) << created;
    }

    const auto push_blob = [&](const std::string& repository, const std::string& contents) {
        const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
        const std::string started = http_request(server.port(),
                                                 "POST",
                                                 "/v2/" + repository + "/blobs/uploads/",
                                                 "",
                                                 "application/octet-stream",
                                                 {},
                                                 authorization);
        EXPECT_NE(started.find(" 202 "), std::string::npos) << started;
        const std::string completed =
            http_request(server.port(),
                         "PUT",
                         response_header(started, "Location") + "?digest=" + digest,
                         contents,
                         "application/octet-stream",
                         {},
                         authorization);
        EXPECT_NE(completed.find(" 201 "), std::string::npos) << completed;
        return digest;
    };
    const auto publish = [&](const std::string& repository,
                             const std::string& tag,
                             const std::string& config,
                             const std::vector<std::string>& layers) {
        json manifest{{"schemaVersion", 2},
                      {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                      {"config",
                       {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                        {"digest", push_blob(repository, config)},
                        {"size", config.size()}}},
                      {"layers", json::array()}};
        for (const std::string& layer : layers) {
            manifest["layers"].push_back({{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                                          {"digest", push_blob(repository, layer)},
                                          {"size", layer.size()}});
        }
        const std::string text = manifest.dump();
        const std::string published = http_request(server.port(),
                                                   "PUT",
                                                   "/v2/" + repository + "/manifests/" + tag,
                                                   text,
                                                   "application/vnd.oci.image.manifest.v1+json",
                                                   {},
                                                   authorization);
        EXPECT_NE(published.find(" 201 "), std::string::npos) << published;
        return text.size();
    };

    const std::string config = R"({"architecture":"amd64","os":"linux"})";
    const std::string base_layer(1000, 'b');
    const std::string app_layer(300, 'a');
    // Two tags of one image and a cache image sharing its base layer: every digest once.
    const std::size_t manifest_size =
        publish("tenant-one/service", "v1", config, {base_layer, app_layer});
    EXPECT_EQ(publish("tenant-one/service", "v2", config, {base_layer, app_layer}), manifest_size);
    const std::string cache_config = R"({"cache":true})";
    const std::size_t cache_manifest =
        publish("tenant-one/cache/service", "buildcache", cache_config, {base_layer});
    // Another space's image never counts toward this one.
    publish("tenant-two/other", "v1", R"({"other":true})", {std::string(5000, 'x')});

    const std::string response = http_request(server.port(),
                                              "GET",
                                              "/v1/metering_usage?customer_id=" + customer->id,
                                              "",
                                              "application/json",
                                              {},
                                              authorization);
    const json body = json::parse(response_body(response));
    ASSERT_TRUE(body.at("ok")) << body;
    EXPECT_EQ(body.at("value").at("summary").at("storage").at("images_bytes"),
              manifest_size + config.size() + base_layer.size() + app_layer.size() +
                  cache_manifest + cache_config.size());
    server.stop();
}

// Deleting a workspace removes its tenant images (lake#514, lake#383): the routes of the app's
// image and its build cache go with the cascade, so neither answers pulls, while another
// workspace's image of the same space, and the blobs it uses, stay.
TEST(RegistryProtocols, DeletingAWorkspaceMakesItsImagesUnpullable) {
    TemporarySyncRoot temporary;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user(
        {"taap:1:ed25519:image-owner", "image-owner@example.test", "Image owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "tenant-one"});
    ASSERT_TRUE(customer) << customer.error().message;
    lake::test::RecordWorkspaces workspaces(store);
    const auto doomed = store->get_workspace(workspaces.create(customer->id, "doomed"));
    const auto kept = store->get_workspace(workspaces.create(customer->id, "kept"));
    ASSERT_TRUE(doomed && kept);
    for (const auto& [workspace, name] :
         {std::pair{doomed->id, std::string("reports")}, std::pair{kept->id, std::string("other")}}) {
        ASSERT_TRUE(store->create_web_app({.customer_id = customer->id,
                                           .workspace_id = workspace,
                                           .name = name,
                                           .mode = "image",
                                           .image = "lake.example.test/tenant-one/" + name + ":v1",
                                           .framework = "container",
                                           .port = 8080,
                                           .host = name + ".apps.example.test"}));
    }
    ServerOptions options;
    options.port = 0;
    options.node_name = "workspace-deletion-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "6c616b652d666f7267652d7265636f7264732d746573742d696e7374616e6365";
    options.store_root = temporary.path().string();
    options.store = store;
    options.workspaces = workspaces.service;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string admin = "Bearer registry-admin";
    // Each repository is routed to its workspace before its first push (lake#383).
    for (const auto& [workspace, name] : {std::pair{doomed->id, std::string("reports")},
                                          std::pair{doomed->id, std::string("cache/reports")},
                                          std::pair{kept->id, std::string("other")}}) {
        const std::string created = http_request(
            server.port(), "POST", "/v1/create_image_repository",
            json{{"customer_id", customer->id}, {"workspace_id", workspace}, {"name", name}}.dump(),
            "application/json", {}, admin);
        ASSERT_NE(created.find(" 200 "), std::string::npos) << created;
    }
    const std::string config = R"({"architecture":"amd64","os":"linux"})";
    const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(config);
    const json manifest{{"schemaVersion", 2},
                        {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                        {"config",
                         {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                          {"digest", digest},
                          {"size", config.size()}}},
                        {"layers", json::array()}};
    const auto publish = [&](const std::string& repository, const std::string& tag) {
        const std::string started = http_request(server.port(), "POST",
                                                 "/v2/" + repository + "/blobs/uploads/", "",
                                                 "application/octet-stream", {}, admin);
        ASSERT_NE(started.find(" 202 "), std::string::npos) << started;
        const std::string completed = http_request(
            server.port(), "PUT", response_header(started, "Location") + "?digest=" + digest,
            config, "application/octet-stream", {}, admin);
        ASSERT_NE(completed.find(" 201 "), std::string::npos) << completed;
        const std::string published = http_request(
            server.port(), "PUT", "/v2/" + repository + "/manifests/" + tag, manifest.dump(),
            "application/vnd.oci.image.manifest.v1+json", {}, admin);
        ASSERT_NE(published.find(" 201 "), std::string::npos) << published;
    };
    publish("tenant-one/reports", "latest");
    publish("tenant-one/cache/reports", "buildcache");
    publish("tenant-one/other", "latest");
    const auto pull = [&](const std::string& repository, const std::string& tag) {
        return http_request(server.port(), "GET", "/v2/" + repository + "/manifests/" + tag, "",
                            "application/json", {}, admin);
    };
    ASSERT_NE(pull("tenant-one/reports", "latest").find(" 200 "), std::string::npos);

    const std::string planned = http_request(
        server.port(), "POST", "/v1/delete_workspace",
        json{{"customer_id", customer->id}, {"workspace_id", doomed->id}, {"dry_run", true}}.dump(),
        "application/json", {}, admin);
    const json plan = json::parse(response_body(planned));
    ASSERT_TRUE(plan.at("ok")) << plan;
    EXPECT_EQ(plan["value"]["counts"]["images"], 2) << plan;
    EXPECT_EQ(plan["value"]["counts"]["apps"], 1) << plan;

    const std::string deleted = http_request(
        server.port(), "POST", "/v1/delete_workspace",
        json{{"customer_id", customer->id}, {"workspace_id", doomed->id}, {"confirm", "doomed"}}
            .dump(),
        "application/json", {}, admin);
    const json body = json::parse(response_body(deleted));
    ASSERT_TRUE(body.at("ok")) << body;
    EXPECT_TRUE(body["value"]["deletion"]["completed"]) << body;

    // The routes went with the cascade, and the records with the workspace's Zetta.
    EXPECT_NE(pull("tenant-one/reports", "latest").find(" 404 "), std::string::npos);
    EXPECT_NE(pull("tenant-one/cache/reports", "buildcache").find(" 404 "), std::string::npos);
    EXPECT_FALSE(store->get_oci_repository_route(customer->id, "reports"));
    EXPECT_FALSE(store->get_oci_repository_route(customer->id, "cache/reports"));
    EXPECT_NE(pull("tenant-one/other", "latest").find(" 200 "), std::string::npos);
    const std::string blob = http_request(server.port(), "GET",
                                          "/v2/tenant-one/other/blobs/" + digest, "",
                                          "application/octet-stream", {}, admin);
    EXPECT_EQ(response_body(blob), config);
    server.stop();
}

TEST(RegistryProtocols, AppPullCredentialReadsOnlyItsOwnImage) {
    TemporarySyncRoot temporary;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user(
        {"taap:1:ed25519:app-pull-owner", "app-pull-owner@example.test", "App pull owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "tenant-one"});
    ASSERT_TRUE(customer) << customer.error().message;
    lake::test::RecordWorkspaces workspaces(store);
    const auto workspace = store->get_workspace(workspaces.create(customer->id, "apps"));
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto app = store->create_web_app({.customer_id = customer->id,
                                            .workspace_id = workspace->id,
                                            .name = "reports",
                                            .mode = "image",
                                            .image = "lake.example.test/tenant-one/reports:v1",
                                            .framework = "container",
                                            .port = 8080,
                                            .host = "reports.apps.example.test"});
    ASSERT_TRUE(app) << app.error().message;
    const auto other_app = store->create_web_app({.customer_id = customer->id,
                                                  .workspace_id = workspace->id,
                                                  .name = "other",
                                                  .mode = "image",
                                                  .image = "lake.example.test/tenant-one/other:v1",
                                                  .framework = "container",
                                                  .port = 8080,
                                                  .host = "other.apps.example.test"});
    ASSERT_TRUE(other_app) << other_app.error().message;
    ASSERT_TRUE(store->set_web_app_token_hashes(
        app->id, {{"pull", lake::control::identity_crypto::sha256_hex("app-pull-token")}}));

    ServerOptions options;
    options.port = 0;
    options.node_name = "app-pull-oci-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = temporary.path().string();
    options.store = store;
    options.workspaces = workspaces.service;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string admin = "Bearer registry-admin";
    ASSERT_TRUE(store->create_oci_repository_route(customer->id, "reports", workspace->id));
    const std::string config = R"({"architecture":"arm64","os":"darwin"})";
    const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(config);
    const std::string started = http_request(server.port(),
                                             "POST",
                                             "/v2/tenant-one/reports/blobs/uploads/",
                                             "",
                                             "application/octet-stream",
                                             {},
                                             admin);
    ASSERT_NE(started.find(" 202 "), std::string::npos) << started;
    const std::string completed =
        http_request(server.port(),
                     "PUT",
                     response_header(started, "Location") + "?digest=" + digest,
                     config,
                     "application/octet-stream",
                     {},
                     admin);
    ASSERT_NE(completed.find(" 201 "), std::string::npos) << completed;
    const json manifest{{"schemaVersion", 2},
                        {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                        {"config",
                         {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                          {"digest", digest},
                          {"size", config.size()}}},
                        {"layers", json::array()}};
    const std::string published = http_request(server.port(),
                                               "PUT",
                                               "/v2/tenant-one/reports/manifests/latest",
                                               manifest.dump(),
                                               "application/vnd.oci.image.manifest.v1+json",
                                               {},
                                               admin);
    ASSERT_NE(published.find(" 201 "), std::string::npos) << published;

    const std::string basic = "Basic YXBwOmFwcC1wdWxsLXRva2Vu";
    const std::string own_manifest = http_request(server.port(),
                                                  "GET",
                                                  "/v2/tenant-one/reports/manifests/latest",
                                                  "",
                                                  "application/json",
                                                  {},
                                                  basic);
    EXPECT_NE(own_manifest.find(" 200 "), std::string::npos) << own_manifest;
    const std::string own_head = http_request(server.port(),
                                              "HEAD",
                                              "/v2/tenant-one/reports/manifests/latest",
                                              "",
                                              "application/json",
                                              {},
                                              basic);
    EXPECT_NE(own_head.find(" 200 "), std::string::npos) << own_head;
    const std::string own_blob = http_request(server.port(),
                                              "GET",
                                              "/v2/tenant-one/reports/blobs/" + digest,
                                              "",
                                              "application/octet-stream",
                                              {},
                                              basic);
    EXPECT_EQ(response_body(own_blob), config);
    const std::string other = http_request(server.port(),
                                           "GET",
                                           "/v2/tenant-one/other/manifests/latest",
                                           "",
                                           "application/json",
                                           {},
                                           basic);
    EXPECT_NE(other.find(" 403 "), std::string::npos) << other;
    const std::string cache = http_request(server.port(),
                                           "GET",
                                           "/v2/tenant-one/cache/reports/manifests/latest",
                                           "",
                                           "application/json",
                                           {},
                                           basic);
    EXPECT_NE(cache.find(" 403 "), std::string::npos) << cache;
    const std::string push = http_request(server.port(),
                                          "POST",
                                          "/v2/tenant-one/reports/blobs/uploads/",
                                          "",
                                          "application/octet-stream",
                                          {},
                                          basic);
    EXPECT_NE(push.find(" 403 "), std::string::npos) << push;
    const std::string wrong_username = http_request(server.port(),
                                                    "GET",
                                                    "/v2/tenant-one/reports/manifests/latest",
                                                    "",
                                                    "application/json",
                                                    {},
                                                    "Basic bGFrZTphcHAtcHVsbC10b2tlbg==");
    EXPECT_NE(wrong_username.find(" 403 "), std::string::npos) << wrong_username;
    const std::string bearer = http_request(server.port(),
                                            "GET",
                                            "/v2/tenant-one/reports/manifests/latest",
                                            "",
                                            "application/json",
                                            {},
                                            "Bearer app-pull-token");
    EXPECT_NE(bearer.find(" 403 "), std::string::npos) << bearer;
    server.stop();
}

namespace {

// A space and its workspaces for the tenant image tests (lake#383), on the memory store or, with
// LAKE_TEST_DB_URI, on PostgreSQL.
class TenantImages : public testing::TestWithParam<bool> {
protected:
    static constexpr const char* kek =
        "74656e616e742d696d616765732d696e7374616e63652d6b656b2d7465737421";

    void SetUp() override {
        if (GetParam()) {
            const char* uri = lake::test::postgres_uri();
            if (uri == nullptr || *uri == '\0') {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            database_uri = uri;
            store = lake::control::make_postgres_store(uri, kek);
        } else {
            store = std::make_shared<lake::control::InMemoryStore>();
        }
        workspaces = std::make_unique<lake::test::RecordWorkspaces>(store, database_uri);
        suffix = lake::control::new_uuid_v4().substr(0, 8);
    }

    void start(const std::filesystem::path& root) {
        ServerOptions options;
        options.port = 0;
        options.node_name = "tenant-images-" + suffix;
        options.admin_token = "registry-admin";
        options.instance_kek = kek;
        options.store_root = root.string();
        options.store = store;
        options.workspaces = workspaces->service;
        server = std::make_unique<ApiServer>(options);
        ASSERT_TRUE(server->start()) << server->reason();
    }

    // A space named `<name>-<suffix>`: its id and name.
    std::pair<std::string, std::string> space(const std::string& name) {
        const std::string handle = name + "-" + suffix;
        const auto user = store->create_user(
            {"taap:1:ed25519:" + handle, handle + "@example.test", handle});
        EXPECT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", handle});
        EXPECT_TRUE(customer) << customer.error().message;
        return {customer->id, handle};
    }

    json call(const std::string& capability, const json& input,
              std::string_view authorization = "Bearer registry-admin") {
        const std::string response = http_request(server->port(), "POST", "/v1/" + capability,
                                                  input.dump(), "application/json", {},
                                                  authorization);
        return response_json(response);
    }

    std::string push_blob(const std::string& repository, const std::string& contents,
                          std::string_view authorization = "Bearer registry-admin") {
        const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
        const std::string started =
            http_request(server->port(), "POST", "/v2/" + repository + "/blobs/uploads/", "",
                         "application/octet-stream", {}, authorization);
        if (started.find(" 202 ") == std::string::npos) {
            return started;
        }
        const std::string completed = http_request(
            server->port(), "PUT", response_header(started, "Location") + "?digest=" + digest,
            contents, "application/octet-stream", {}, authorization);
        EXPECT_NE(completed.find(" 201 "), std::string::npos) << completed;
        return digest;
    }

    // An image of one config and one layer pushed to `repository:tag`; answers the manifest's
    // text, or the refusal.
    std::string push_image(const std::string& repository, const std::string& tag,
                           const std::string& config, const std::string& layer,
                           std::string_view authorization = "Bearer registry-admin") {
        const std::string config_digest = push_blob(repository, config, authorization);
        if (!config_digest.starts_with("sha256:")) {
            return config_digest;
        }
        const std::string layer_digest = push_blob(repository, layer, authorization);
        const json manifest{{"schemaVersion", 2},
                            {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                            {"config",
                             {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                              {"digest", config_digest},
                              {"size", config.size()}}},
                            {"layers",
                             {{{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                               {"digest", layer_digest},
                               {"size", layer.size()}}}}};
        const std::string text = manifest.dump();
        const std::string published = http_request(
            server->port(), "PUT", "/v2/" + repository + "/manifests/" + tag, text,
            "application/vnd.oci.image.manifest.v1+json", {}, authorization);
        return published.find(" 201 ") == std::string::npos ? published : text;
    }

    std::string pull(const std::string& path, std::string_view authorization = "Bearer registry-admin") {
        return http_request(server->port(), "GET", path, "", "application/json", {},
                            authorization);
    }

    std::shared_ptr<lake::control::Store> store;
    std::string database_uri;
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces;
    std::unique_ptr<ApiServer> server;
    std::string suffix;
};

std::string oci_code(const std::string& response) {
    const json body = response_json(response);
    return body.contains("errors") ? body["errors"][0].value("code", std::string()) : std::string();
}

} // namespace

// Two spaces with two workspaces each (lake#383 phase A): a tenant image repository is routed to
// one workspace by an explicit workspace_id, never by default; its manifests and tags are sealed
// records only in that workspace's Zetta, pulled back through the route byte for byte; the route
// never moves; another space may use the same name; builds push only into their own workspace and
// route on their first push; a workspace that is not ready refuses rather than falling back;
// npm and one-component OCI stay with the instance; deleting a workspace removes its routes and
// its records go with its Zetta.
TEST_P(TenantImages, LiveInTheirRoutedWorkspaceAndKeepTheirAddresses) {
    TemporarySyncRoot temporary;
    const auto [alpha, alpha_name] = space("alpha");
    const auto [beta, beta_name] = space("beta");
    const std::string a1 = workspaces->create(alpha, "a1");
    const std::string a2 = workspaces->create(alpha, "a2");
    const std::string b1 = workspaces->create(beta, "b1");
    const std::string b2 = workspaces->create(beta, "b2");
    start(temporary.path());

    // Missing, unknown, foreign and deleted workspaces, and a bad name: invalid_input naming the
    // field. There is no default workspace.
    json refused = call("create_image_repository", {{"customer_id", alpha}, {"name", "web"}});
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    EXPECT_EQ(refused["error"]["path"], "/workspace_id") << refused;
    refused = call("create_image_repository", {{"customer_id", alpha},
                                               {"workspace_id", lake::control::new_uuid_v4()},
                                               {"name", "web"}});
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    EXPECT_EQ(refused["error"]["path"], "/workspace_id") << refused;
    refused = call("create_image_repository",
                   {{"customer_id", alpha}, {"workspace_id", "not-a-uuid"}, {"name", "web"}});
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    EXPECT_EQ(refused["error"]["path"], "/workspace_id") << refused;
    refused = call("create_image_repository",
                   {{"customer_id", alpha}, {"workspace_id", b1}, {"name", "web"}});
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    refused = call("create_image_repository",
                   {{"customer_id", alpha}, {"workspace_id", a1}, {"name", "Web/../x"}});
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    EXPECT_EQ(refused["error"]["path"], "/name") << refused;
    const std::string doomed = workspaces->create(alpha, "doomed");
    ASSERT_TRUE(store->update_workspace(doomed, {.desired = "deleted"}));
    refused = call("create_image_repository",
                   {{"customer_id", alpha}, {"workspace_id", doomed}, {"name", "web"}});
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    EXPECT_EQ(refused["error"]["path"], "/workspace_id") << refused;
    // Nothing was routed by any refusal.
    EXPECT_TRUE(store->list_oci_repository_routes(alpha, {})->empty());

    // An unrouted tenant repository takes no push from the administrator and answers no pull.
    const std::string unrouted = push_blob(alpha_name + "/web", "orphan");
    EXPECT_NE(unrouted.find(" 404 "), std::string::npos) << unrouted;
    EXPECT_EQ(oci_code(unrouted), "NAME_UNKNOWN");

    // Routed explicitly; repeating it answers the route; another workspace cannot take it.
    json routed = call("create_image_repository",
                       {{"customer_id", alpha}, {"workspace_id", a1}, {"name", "web"}});
    ASSERT_TRUE(routed.value("ok", false)) << routed;
    EXPECT_EQ(routed["value"]["repository"], alpha_name + "/web");
    EXPECT_EQ(routed["value"]["workspace_id"], a1);
    EXPECT_TRUE(call("create_image_repository",
                     {{"customer_id", alpha}, {"workspace_id", a1}, {"name", "web"}})
                    .value("ok", false));
    refused = call("create_image_repository",
                   {{"customer_id", alpha}, {"workspace_id", a2}, {"name", "web"}});
    EXPECT_EQ(refused["error"]["code"], "conflict") << refused;
    // The other space may use the same name in its own workspace.
    ASSERT_TRUE(call("create_image_repository",
                     {{"customer_id", beta}, {"workspace_id", b2}, {"name", "web"}})
                    .value("ok", false));
    if (!database_uri.empty()) {
        // The route never changes, whoever writes the table.
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE oci_repository_routes SET workspace_id = $2::uuid "
                                    "WHERE customer_id = $1::uuid AND name = 'web'",
                                    alpha, a2),
                     pqxx::check_violation);
    }
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, a1);

    // Push and pull through the route: the bytes come back, and the records sit only in a1.
    const std::string alpha_manifest =
        push_image(alpha_name + "/web", "v1", R"({"space":"alpha"})", "alpha layer");
    ASSERT_TRUE(alpha_manifest.starts_with("{")) << alpha_manifest;
    const std::string beta_manifest =
        push_image(beta_name + "/web", "v1", R"({"space":"beta"})", "beta layer");
    ASSERT_TRUE(beta_manifest.starts_with("{")) << beta_manifest;
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/web/manifests/v1")), alpha_manifest);
    EXPECT_EQ(response_body(pull("/v2/" + beta_name + "/web/manifests/v1")), beta_manifest);
    const std::string alpha_digest =
        "sha256:" + lake::control::identity_crypto::sha256_hex(alpha_manifest);
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/web/manifests/" + alpha_digest)),
              alpha_manifest);
    const std::string layer_digest =
        "sha256:" + lake::control::identity_crypto::sha256_hex(std::string("alpha layer"));
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/web/blobs/" + layer_digest)),
              "alpha layer");
    // Another space's repository never serves this space's digests.
    EXPECT_NE(pull("/v2/" + beta_name + "/web/blobs/" + layer_digest).find(" 404 "),
              std::string::npos);
    EXPECT_NE(pull("/v2/" + beta_name + "/web/manifests/" + alpha_digest).find(" 404 "),
              std::string::npos);
    EXPECT_EQ(workspaces->records(a1).rows("oci_tags").size(), 1U);
    EXPECT_EQ(workspaces->records(a1).rows("oci_manifests").size(), 1U);
    EXPECT_TRUE(workspaces->records(a2).rows("oci_tags").empty());
    EXPECT_TRUE(workspaces->records(b1).rows("oci_tags").empty());
    EXPECT_EQ(workspaces->records(b2).rows("oci_tags").size(), 1U);
    // Sealed: the workspace holds neither the tag nor the manifest in the clear.
    for (const auto& [key, record] : workspaces->records(a1).rows("oci_manifests")) {
        EXPECT_EQ(record.dump().find("alpha"), std::string::npos) << record;
    }
    for (const auto& [key, record] : workspaces->records(a1).rows("oci_tags")) {
        EXPECT_EQ(record.dump().find("\"v1\""), std::string::npos) << record;
    }
    // The route holds no manifest, digest or tag.
    if (!database_uri.empty()) {
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        const auto columns = tx.exec(
            "SELECT string_agg(column_name, ',' ORDER BY column_name) FROM "
            "information_schema.columns WHERE table_name = 'oci_repository_routes'");
        EXPECT_EQ(columns[0][0].as<std::string>(), "created_at,customer_id,name,updated_at,workspace_id");
    }

    // A tag moves at the revision read: the next push of v1 is what v1 pulls.
    const std::string moved =
        push_image(alpha_name + "/web", "v1", R"({"space":"alpha","build":2})", "alpha layer two");
    ASSERT_TRUE(moved.starts_with("{")) << moved;
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/web/manifests/v1")), moved);
    EXPECT_EQ(workspaces->records(a1).rows("oci_tags").size(), 1U);
    EXPECT_EQ(workspaces->records(a1).rows("oci_manifests").size(), 2U);
    const json listed = response_json(http_request(
        server->port(), "GET", "/v1/list_image_repositories?customer_id=" + alpha, "",
        "application/json", {}, "Bearer registry-admin"));
    ASSERT_TRUE(listed.value("ok", false)) << listed;
    ASSERT_EQ(listed["value"]["items"].size(), 1U) << listed;
    EXPECT_EQ(listed["value"]["items"][0]["tags"][0]["tag"], "v1");

    // A build pushes only into its own workspace, and routes a new repository on its first push.
    const auto source = store->create_repository({alpha, "source-" + suffix, "", "private", a2});
    ASSERT_TRUE(source) << source.error().message;
    const auto build = store->create_build({.customer_id = alpha,
                                            .workspace_id = a2,
                                            .repository = source->id,
                                            .revision = "main",
                                            .target = "registry/" + alpha_name + "/api:v1"});
    ASSERT_TRUE(build) << build.error().message;
    const json hashes{{"push", lake::control::identity_crypto::sha256_hex("a2-push")}};
    if (database_uri.empty()) {
        ASSERT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_build_status(
            build->id, {.phase = "Running", .token_hashes = hashes}));
    } else {
        // What the operator writes for a running build.
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE builds SET phase = 'Running', token_hashes_json = $2::jsonb "
                       "WHERE id = $1::uuid",
                       build->id, hashes.dump());
        tx.commit();
    }
    const std::string basic = "Basic bGFrZTphMi1wdXNo"; // lake:a2-push
    const std::string foreign = push_blob(alpha_name + "/web", "into a1", basic);
    EXPECT_NE(foreign.find(" 403 "), std::string::npos) << foreign;
    const std::string built = push_image(alpha_name + "/api", "v1", R"({"api":1})", "api", basic);
    ASSERT_TRUE(built.starts_with("{")) << built;
    EXPECT_EQ(store->get_oci_repository_route(alpha, "api")->workspace_id, a2);
    EXPECT_EQ(workspaces->records(a2).rows("oci_tags").size(), 1U);

    // npm and one-component OCI stay with the instance: no route, no workspace record.
    const json npm_metadata{{"name", "instance-package-" + suffix},
                            {"dist-tags", {{"latest", "1.0.0"}}},
                            {"versions",
                             {{"1.0.0",
                               {{"name", "instance-package-" + suffix},
                                {"version", "1.0.0"},
                                {"dist", {{"tarball", "placeholder"}}}}}}},
                            {"_attachments",
                             {{"package-1.0.0.tgz",
                               {{"content_type", "application/octet-stream"},
                                {"data", base64_bytes(bytes("npm bytes"))},
                                {"length", 9}}}}}};
    const std::string published =
        http_request(server->port(), "PUT", "/instance-package-" + suffix, npm_metadata.dump(),
                     "application/json", {}, "Bearer registry-admin");
    ASSERT_NE(published.find(" 201 "), std::string::npos) << published;
    EXPECT_EQ(response_body(http_request(server->port(), "GET",
                                         "/instance-package-" + suffix + "/-/package-1.0.0.tgz", "",
                                         "application/octet-stream")),
              "npm bytes");
    const std::string legacy = push_image("instance-image", "v1", R"({"instance":1})", "public");
    ASSERT_TRUE(legacy.starts_with("{")) << legacy;
    EXPECT_EQ(response_body(http_request(server->port(), "GET", "/v2/instance-image/manifests/v1",
                                         "", "application/json")),
              legacy);
    EXPECT_EQ(store->list_oci_repository_routes(alpha, {})->size(), 2U);
    EXPECT_EQ(store->list_oci_repository_routes(beta, {})->size(), 1U);

    // A workspace that is not ready is refused, never served from elsewhere.
    workspaces->status(a1, "Pending");
    const std::string pending = pull("/v2/" + alpha_name + "/web/manifests/v1");
    EXPECT_NE(pending.find(" 503 "), std::string::npos) << pending;
    workspaces->ready(a1);
    EXPECT_NE(pull("/v2/" + alpha_name + "/web/manifests/v1").find(" 200 "), std::string::npos);

    // Deleting a1 deletes its route in the cascade; a2's repository and the other space stay.
    const json deleted = call("delete_workspace",
                              {{"customer_id", alpha}, {"workspace_id", a1}, {"confirm", "a1"}});
    ASSERT_TRUE(deleted.value("ok", false)) << deleted;
    EXPECT_TRUE(deleted["value"]["deletion"]["completed"]) << deleted;
    ASSERT_EQ(deleted["value"]["deletion"]["images"].size(), 1U) << deleted;
    EXPECT_EQ(deleted["value"]["deletion"]["images"][0]["name"], alpha_name + "/web");
    EXPECT_EQ(deleted["value"]["deletion"]["images"][0]["state"], "routed");
    EXPECT_FALSE(store->get_oci_repository_route(alpha, "web"));
    EXPECT_NE(pull("/v2/" + alpha_name + "/web/manifests/v1").find(" 404 "), std::string::npos);
    EXPECT_NE(pull("/v2/" + alpha_name + "/api/manifests/v1").find(" 200 "), std::string::npos);
    EXPECT_NE(pull("/v2/" + beta_name + "/web/manifests/v1").find(" 200 "), std::string::npos);
    server->stop();
}

// The one-shot copy of the instance registry's tenant tags (lake#383, copy_workspace_records,
// collection oci_tags): a repository whose image one workspace's app uses is routed there with its
// build cache, a space's only workspace takes its space's repositories, an ambiguous one waits for
// create_image_repository and blocks completion, an unknown space is reported without blocking;
// copy writes each manifest and tag once, a rerun writes nothing, verify reads them back and finds
// tampering, the copied image pulls through its route, and only the operator copies.
TEST_P(TenantImages, TheOneShotCopyRoutesAndMovesLegacyTags) {
    TemporarySyncRoot temporary;
    const auto [alpha, alpha_name] = space("alpha");
    const auto [gamma, gamma_name] = space("gamma");
    const std::string a1 = workspaces->create(alpha, "a1");
    const std::string a2 = workspaces->create(alpha, "a2");
    const std::string g1 = workspaces->create(gamma, "g1");
    ASSERT_TRUE(store->create_web_app({.customer_id = alpha,
                                       .workspace_id = a1,
                                       .name = "web",
                                       .mode = "image",
                                       .image = "lake.example.test/" + alpha_name + "/web:v1",
                                       .framework = "container",
                                       .port = 8080,
                                       .host = "web-" + suffix + ".apps.example.test"}));

    // What lake wrote before lake#383: blobs and tag pointers in the instance registry.
    const std::filesystem::path registry =
        temporary.path() / ("tenant-images-" + suffix) / "registry";
    const auto blob = [&](const std::string& contents) {
        const std::string hex = lake::control::identity_crypto::sha256_hex(contents);
        std::filesystem::create_directories(registry / "blobs");
        std::ofstream(registry / "blobs" / hex, std::ios::binary) << contents;
        return "sha256:" + hex;
    };
    std::map<std::string, std::string> documents;
    const auto legacy = [&](const std::string& repository, const std::string& tag) {
        const std::string config = R"({"legacy":")" + repository + R"("})";
        const std::string layer = "layer of " + repository;
        const json manifest{{"schemaVersion", 2},
                            {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                            {"config", {{"digest", blob(config)}, {"size", config.size()}}},
                            {"layers", {{{"digest", blob(layer)}, {"size", layer.size()}}}}};
        const std::string text = manifest.dump();
        const std::string digest = blob(text);
        std::filesystem::create_directories(registry / "manifests" / repository);
        std::ofstream(registry / "manifests" / repository / (tag + ".json"))
            << json{{"digest", digest},
                    {"content_type", "application/vnd.oci.image.manifest.v1+json"}}
                   .dump();
        documents[repository + ":" + tag] = text;
    };
    legacy(alpha_name + "/web", "v1");
    legacy(alpha_name + "/cache/web", "buildcache");
    legacy(alpha_name + "/orphan", "v1");
    legacy(gamma_name + "/solo", "v1");
    legacy(gamma_name + "/broken", "v1");
    legacy(alpha_name + "/early", "v1");
    legacy("nobody-" + suffix + "/lost", "v1");
    legacy("public-image", "v1"); // the instance's own: never part of the copy
    start(temporary.path());
    const auto copy = [&](const std::string& mode) {
        const json result = call("copy_workspace_records", {{"collection", "oci_tags"},
                                                            {"mode", mode},
                                                            {"limit", 1000}});
        EXPECT_TRUE(result.value("ok", false)) << result;
        return result.value("value", json::object());
    };
    const auto outcomes = [](const json& page) {
        std::map<std::string, std::string> found;
        for (const json& item : page.value("unresolved", json::array())) {
            found[item.value("id", std::string())] = item.value("outcome", std::string());
        }
        return found;
    };

    // A tag pushed through its route after the cutover and before the copy: newer than the
    // registry's pointer, kept by the copy and accepted by verify.
    ASSERT_TRUE(call("create_image_repository",
                     {{"customer_id", alpha}, {"workspace_id", a1}, {"name", "early"}})
                    .value("ok", false));
    const std::string early = push_image(alpha_name + "/early", "v1", R"({"early":2})", "early 2");
    ASSERT_TRUE(early.starts_with("{")) << early;
    // A tag whose layer the registry lost is reported, never copied with a wrong size.
    const std::string lost_layer =
        lake::control::identity_crypto::sha256_hex("layer of " + gamma_name + "/broken");
    std::filesystem::rename(registry / "blobs" / lost_layer, registry / "lost-layer");

    // Plan and verify change nothing: no route is written before the copy.
    json plan = copy("plan");
    EXPECT_EQ(plan["rows"], 7) << plan;
    EXPECT_EQ(plan["assigned"], 4) << plan;
    EXPECT_EQ(outcomes(plan), (std::map<std::string, std::string>{
                                  {alpha_name + "/orphan:v1", "needs_selection"},
                                  {gamma_name + "/broken:v1", "blob_missing"},
                                  {"nobody-" + suffix + "/lost:v1", "unknown_space"}}));
    EXPECT_FALSE(copy("verify")["complete"]);
    for (const auto& [space_id, name] : {std::pair{alpha, std::string("web")},
                                         std::pair{alpha, std::string("cache/web")},
                                         std::pair{gamma, std::string("solo")},
                                         std::pair{alpha, std::string("orphan")}}) {
        EXPECT_FALSE(store->get_oci_repository_route(space_id, name)) << name;
    }
    EXPECT_NE(pull("/v2/" + alpha_name + "/web/manifests/v1").find(" 404 "), std::string::npos);
    std::filesystem::rename(registry / "lost-layer", registry / "blobs" / lost_layer);

    // Copy routes what is unambiguous and writes each tag once.
    json copied = copy("copy");
    EXPECT_EQ(copied["copied"], 4) << copied;
    EXPECT_EQ(copied["already_present"], 1) << copied;
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, a1);
    EXPECT_EQ(store->get_oci_repository_route(alpha, "cache/web")->workspace_id, a1);
    EXPECT_EQ(store->get_oci_repository_route(gamma, "solo")->workspace_id, g1);
    EXPECT_FALSE(store->get_oci_repository_route(alpha, "orphan"));
    copied = copy("copy");
    EXPECT_EQ(copied["copied"], 0) << copied;
    EXPECT_EQ(copied["already_present"], 5) << copied;
    json verified = copy("verify");
    EXPECT_EQ(verified["verified"], 5) << verified;
    EXPECT_EQ(verified["mismatched"], json::array()) << verified;
    EXPECT_FALSE(verified["complete"]) << "the ambiguous repository blocks completion";
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/early/manifests/v1")), early);

    // An operator routes the ambiguous repository; the next run copies it and completes.
    ASSERT_TRUE(call("create_image_repository",
                     {{"customer_id", alpha}, {"workspace_id", a2}, {"name", "orphan"}})
                    .value("ok", false));
    EXPECT_EQ(copy("copy")["copied"], 1);
    verified = copy("verify");
    EXPECT_EQ(verified["verified"], 6) << verified;
    EXPECT_TRUE(verified["complete"]) << verified;
    EXPECT_EQ(workspaces->records(a1).rows("oci_tags").size(), 3U);
    EXPECT_EQ(workspaces->records(a2).rows("oci_tags").size(), 1U);
    EXPECT_EQ(workspaces->records(g1).rows("oci_tags").size(), 2U);

    // The copied image pulls through its route with its old address and bytes.
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/web/manifests/v1")),
              documents[alpha_name + "/web:v1"]);
    const std::string layer_digest =
        "sha256:" + lake::control::identity_crypto::sha256_hex("layer of " + alpha_name + "/web");
    EXPECT_EQ(response_body(pull("/v2/" + alpha_name + "/web/blobs/" + layer_digest)),
              "layer of " + alpha_name + "/web");
    // Image bytes are counted from the copied tags.
    const json usage = response_json(http_request(
        server->port(), "GET", "/v1/metering_usage?customer_id=" + gamma, "", "application/json",
        {}, "Bearer registry-admin"));
    ASSERT_TRUE(usage.value("ok", false)) << usage;
    std::size_t legacy_bytes = 0;
    for (const std::string repository : {gamma_name + "/solo", gamma_name + "/broken"}) {
        legacy_bytes += documents[repository + ":v1"].size() +
                        std::string(R"({"legacy":")" + repository + R"("})").size() +
                        std::string("layer of " + repository).size();
    }
    EXPECT_EQ(usage["value"]["summary"]["storage"]["images_bytes"], legacy_bytes);

    // A later push of a copied tag is accepted by verify; a changed legacy pointer is not.
    const std::string pushed = push_image(gamma_name + "/solo", "v1", R"({"new":1})", "new layer");
    ASSERT_TRUE(pushed.starts_with("{")) << pushed;
    const std::string other = blob(R"({"schemaVersion":2,"layers":[]})");
    std::ofstream(registry / "manifests" / alpha_name / "web" / "v1.json")
        << json{{"digest", other}, {"content_type", "application/vnd.oci.image.manifest.v1+json"}}
               .dump();
    verified = copy("verify");
    EXPECT_EQ(verified["mismatched"], json::array({alpha_name + "/web:v1"})) << verified;
    EXPECT_FALSE(verified["complete"]);

    // Only the operator copies.
    const auto user = store->create_user({"taap:1:ed25519:copy-" + suffix,
                                          "copy-" + suffix + "@example.test", "Copy"});
    ASSERT_TRUE(user);
    const auto token = store->create_token(
        {user->id, "lake", "copy", "test", {"*"}, 300, false});
    ASSERT_TRUE(token) << token.error().message;
    const json denied = call("copy_workspace_records", {{"collection", "oci_tags"}, {"mode", "plan"}},
                             "Bearer " + token->secret);
    EXPECT_FALSE(denied.value("ok", true)) << denied;
    server->stop();
}

// Who may claim a tenant image name (lake#383): a build's first push routes only its own target
// and its cache, never another name of the space; an app recreated in another workspace cannot
// pull or push through the old workspace's route until the operator removes it with
// unroute_image_repository, after which the new workspace's build routes it on its push and its
// app pulls; the old workspace's records stay where they were; only the operator unroutes; an app
// without a workspace pulls nothing; a layer another space or workspace pushed cannot be published
// by its digest; and list_image_repositories refuses a workspace that is not the space's.
TEST_P(TenantImages, RoutesAreClaimedOnlyByTheirOwnerAndUnroutedOnlyByTheOperator) {
    TemporarySyncRoot temporary;
    const auto [alpha, alpha_name] = space("alpha");
    const auto [beta, beta_name] = space("beta");
    const std::string w1 = workspaces->create(alpha, "w1");
    const std::string w2 = workspaces->create(alpha, "w2");
    const std::string b1 = workspaces->create(beta, "b1");
    start(temporary.path());
    const auto sql = [&](const std::string& statement, const std::string& id, const std::string& value) {
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params(statement, id, value);
        tx.commit();
    };
    const auto app = [&](const std::string& workspace, const std::string& name,
                         const std::string& pull_token) {
        const auto created = store->create_web_app(
            {.customer_id = alpha,
             .workspace_id = workspace,
             .name = name,
             .mode = "image",
             .image = "lake.example.test/" + alpha_name + "/" + name + ":v1",
             .framework = "container",
             .port = 8080,
             .host = name + "-" + lake::control::new_uuid_v4().substr(0, 8) + ".apps.example.test"});
        EXPECT_TRUE(created) << created.error().message;
        const json hashes{{"pull", lake::control::identity_crypto::sha256_hex(pull_token)}};
        if (database_uri.empty()) {
            EXPECT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_web_app_token_hashes(
                created->id, hashes));
        } else {
            sql("UPDATE apps SET token_hashes_json = $2::jsonb WHERE id = $1::uuid", created->id,
                hashes.dump());
        }
        return created->id;
    };
    const auto build = [&](const std::string& workspace, const std::string& target,
                           const std::string& push_token) {
        const auto source =
            store->create_repository({alpha, "source-" + lake::control::new_uuid_v4().substr(0, 8),
                                      "", "private", workspace});
        EXPECT_TRUE(source) << source.error().message;
        const auto created = store->create_build({.customer_id = alpha,
                                                  .workspace_id = workspace,
                                                  .repository = source->id,
                                                  .revision = "main",
                                                  .target = "registry/" + alpha_name + "/" + target});
        EXPECT_TRUE(created) << created.error().message;
        const json hashes{{"push", lake::control::identity_crypto::sha256_hex(push_token)}};
        if (database_uri.empty()) {
            EXPECT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_build_status(
                created->id, {.phase = "Running", .token_hashes = hashes}));
        } else {
            sql("UPDATE builds SET phase = 'Running', token_hashes_json = $2::jsonb "
                "WHERE id = $1::uuid",
                created->id, hashes.dump());
        }
        return "Basic " + base64_bytes(bytes("lake:" + push_token));
    };
    const auto app_basic = [](const std::string& token) {
        return "Basic " + base64_bytes(bytes("app:" + token));
    };

    // A build claims only its own target and its cache.
    const std::string web_app = app(w1, "web", "w1-pull");
    const std::string first = build(w1, "web:v1", "w1-push");
    const std::string elsewhere = push_blob(alpha_name + "/other", "claim", first);
    EXPECT_NE(elsewhere.find(" 403 "), std::string::npos) << elsewhere;
    EXPECT_FALSE(store->get_oci_repository_route(alpha, "other"));
    const std::string nested = push_blob(alpha_name + "/cache/other", "claim", first);
    EXPECT_NE(nested.find(" 403 "), std::string::npos) << nested;
    const std::string w1_image = push_image(alpha_name + "/web", "v1", R"({"w":1})", "w1", first);
    ASSERT_TRUE(w1_image.starts_with("{")) << w1_image;
    ASSERT_TRUE(push_blob(alpha_name + "/cache/web", "cache", first).starts_with("sha256:"));
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, w1);
    EXPECT_EQ(store->get_oci_repository_route(alpha, "cache/web")->workspace_id, w1);
    EXPECT_NE(pull("/v2/" + alpha_name + "/web/manifests/v1", app_basic("w1-pull")).find(" 200 "),
              std::string::npos);
    // Only `<image>` and `cache/<image>` are names at all.
    json refused = call("create_image_repository",
                        {{"customer_id", alpha}, {"workspace_id", w1}, {"name", "team/web"}});
    EXPECT_EQ(refused["error"]["path"], "/name") << refused;

    // The app moves to w2: the old route still answers for w1 only.
    ASSERT_TRUE(store->update_web_app(web_app, {.desired = "deleted"}));
    app(w2, "web", "w2-pull");
    const std::string second = build(w2, "web:v2", "w2-push");
    const std::string blocked = push_image(alpha_name + "/web", "v2", R"({"w":2})", "w2", second);
    EXPECT_NE(blocked.find(" 403 "), std::string::npos) << blocked;
    EXPECT_NE(pull("/v2/" + alpha_name + "/web/manifests/v1", app_basic("w2-pull")).find(" 403 "),
              std::string::npos);

    // Only the operator unroutes; the old workspace keeps its records.
    const auto user = store->create_user({"taap:1:ed25519:unroute-" + suffix,
                                          "unroute-" + suffix + "@example.test", "Unroute"});
    ASSERT_TRUE(user);
    const auto token = store->create_token({user->id, "lake", "unroute", "test", {"*"}, 300, false});
    ASSERT_TRUE(token) << token.error().message;
    EXPECT_FALSE(call("unroute_image_repository", {{"customer_id", alpha}, {"name", "web"}},
                      "Bearer " + token->secret)
                     .value("ok", true));
    EXPECT_TRUE(store->get_oci_repository_route(alpha, "web"));
    const json unrouted = call("unroute_image_repository", {{"customer_id", alpha}, {"name", "web"}});
    ASSERT_TRUE(unrouted.value("ok", false)) << unrouted;
    ASSERT_EQ(unrouted["value"]["routes"].size(), 2U) << unrouted;
    EXPECT_EQ(unrouted["value"]["routes"][0]["name"], "web");
    EXPECT_EQ(unrouted["value"]["routes"][0]["workspace_id"], w1);
    EXPECT_EQ(unrouted["value"]["routes"][1]["name"], "cache/web");
    EXPECT_FALSE(store->get_oci_repository_route(alpha, "web"));
    EXPECT_FALSE(store->get_oci_repository_route(alpha, "cache/web")) << "the cache goes with it";
    EXPECT_EQ(workspaces->records(w1).rows("oci_tags").size(), 1U);
    EXPECT_EQ(call("unroute_image_repository", {{"customer_id", alpha}, {"name", "web"}})["error"]["code"],
              "not_found");
    const auto audit = lake::test::audit_events(*store, {.limit = 200});
    ASSERT_TRUE(audit) << audit.error().message;
    EXPECT_TRUE(std::ranges::any_of(*audit, [](const auto& event) {
        return event.draft.kind == "image.route.remove";
    })) << "the removal is in the central log";

    // w2's build routes it on its push, and w2's app pulls it.
    const std::string w2_image = push_image(alpha_name + "/web", "v2", R"({"w":2})", "w2", second);
    ASSERT_TRUE(w2_image.starts_with("{")) << w2_image;
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, w2);
    const std::string moved = pull("/v2/" + alpha_name + "/web/manifests/v2", app_basic("w2-pull"));
    EXPECT_EQ(response_body(moved), w2_image) << moved;

    // A layer pushed into another space or workspace cannot be published by its digest.
    const std::string w1_layer =
        "sha256:" + lake::control::identity_crypto::sha256_hex(std::string("w1"));
    const std::string beta_config = R"({"beta":1})";
    ASSERT_TRUE(call("create_image_repository",
                     {{"customer_id", beta}, {"workspace_id", b1}, {"name", "thief"}})
                    .value("ok", false));
    ASSERT_TRUE(call("create_image_repository",
                     {{"customer_id", alpha}, {"workspace_id", w2}, {"name", "borrow"}})
                    .value("ok", false));
    for (const std::string repository : {beta_name + "/thief", alpha_name + "/borrow"}) {
        const std::string config_digest = push_blob(repository, beta_config);
        const json manifest{{"schemaVersion", 2},
                            {"config", {{"digest", config_digest}, {"size", beta_config.size()}}},
                            {"layers", {{{"digest", w1_layer}, {"size", 2}}}}};
        const std::string published = http_request(
            server->port(), "PUT", "/v2/" + repository + "/manifests/v1", manifest.dump(),
            "application/vnd.oci.image.manifest.v1+json", {}, "Bearer registry-admin");
        EXPECT_NE(published.find(" 400 "), std::string::npos) << repository << published;
        EXPECT_EQ(oci_code(published), "MANIFEST_BLOB_UNKNOWN") << published;
        EXPECT_NE(pull("/v2/" + repository + "/blobs/" + w1_layer).find(" 404 "),
                  std::string::npos);
    }

    // list_image_repositories refuses a workspace that is not the space's.
    for (const std::string workspace : {std::string("not-a-uuid"), lake::control::new_uuid_v4(), b1}) {
        const json listed = response_json(http_request(
            server->port(), "GET",
            "/v1/list_image_repositories?customer_id=" + alpha + "&workspace_id=" + workspace, "",
            "application/json", {}, "Bearer registry-admin"));
        EXPECT_EQ(listed["error"]["code"], "invalid_input") << workspace << listed;
    }

    // An app left without a workspace (a row from before workspaces) pulls nothing.
    if (!database_uri.empty()) {
        const std::string legacy_app = app(w1, "legacy", "legacy-pull");
        ASSERT_TRUE(store->create_oci_repository_route(alpha, "legacy", w1));
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE apps DISABLE TRIGGER app_workspace_routing");
        tx.exec_params("UPDATE apps SET workspace_id = NULL WHERE id = $1::uuid", legacy_app);
        tx.exec("ALTER TABLE apps ENABLE TRIGGER app_workspace_routing");
        tx.commit();
        const std::string orphan =
            pull("/v2/" + alpha_name + "/legacy/manifests/v1", app_basic("legacy-pull"));
        EXPECT_NE(orphan.find(" 403 "), std::string::npos) << orphan;
    }
    server->stop();

    // Routes that cannot be read are 503, never "unknown": a lake whose route table is broken
    // (here, its new connections see a table of another shape first on their search path; the
    // test's database is its own clone) refuses retryably.
    if (!database_uri.empty()) {
        const std::string broken = "broken_routes_" + suffix;
        {
            pqxx::connection connection(database_uri);
            pqxx::work tx(connection);
            tx.exec("CREATE SCHEMA " + broken);
            tx.exec("CREATE TABLE " + broken + ".oci_repository_routes (unrelated int)");
            tx.commit();
            pqxx::nontransaction alter(connection);
            alter.exec("DO $$ BEGIN EXECUTE format('ALTER DATABASE %I SET search_path = " + broken +
                       ", public', current_database()); END $$");
        }
        store = lake::control::make_postgres_store(database_uri, kek);
        start(temporary.path());
        const std::string unreadable = pull("/v2/" + alpha_name + "/web/manifests/v2");
        EXPECT_NE(unreadable.find(" 503 "), std::string::npos) << unreadable;
        EXPECT_EQ(oci_code(unreadable), "UNAVAILABLE") << unreadable;
        const std::string unwritable = push_blob(alpha_name + "/web", "retry", second);
        EXPECT_NE(unwritable.find(" 503 "), std::string::npos) << unwritable;
        server->stop();
        pqxx::connection connection(database_uri);
        pqxx::nontransaction tx(connection);
        tx.exec("DO $$ BEGIN EXECUTE format('ALTER DATABASE %I RESET search_path', "
                "current_database()); END $$");
        tx.exec("DROP SCHEMA " + broken + " CASCADE");
    }
}

// Moving a route (lake#383): unroute_image_repository with workspace_id deletes and rewrites the
// route of `<image>` and of `cache/<image>` in one transaction, so a build of the old workspace
// pushing all the while never claims the name back, and the image ends routed to the new
// workspace; unrouting `cache/<image>` alone leaves `<image>`; a missing, unknown, foreign or
// deleted target is invalid_input and moves nothing.
TEST_P(TenantImages, AMoveReroutesAnImageAndItsCacheInOneStep) {
    TemporarySyncRoot temporary;
    const auto [alpha, alpha_name] = space("alpha");
    const auto [beta, beta_name] = space("beta");
    const std::string w1 = workspaces->create(alpha, "w1");
    const std::string w2 = workspaces->create(alpha, "w2");
    const std::string b1 = workspaces->create(beta, "b1");
    start(temporary.path());
    const auto source = store->create_repository({alpha, "source-" + suffix, "", "private", w1});
    ASSERT_TRUE(source) << source.error().message;
    const auto build = store->create_build({.customer_id = alpha,
                                            .workspace_id = w1,
                                            .repository = source->id,
                                            .revision = "main",
                                            .target = "registry/" + alpha_name + "/web:v1"});
    ASSERT_TRUE(build) << build.error().message;
    const json hashes{{"push", lake::control::identity_crypto::sha256_hex("mover-push")}};
    if (database_uri.empty()) {
        ASSERT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_build_status(
            build->id, {.phase = "Running", .token_hashes = hashes}));
    } else {
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE builds SET phase = 'Running', token_hashes_json = $2::jsonb "
                       "WHERE id = $1::uuid",
                       build->id, hashes.dump());
        tx.commit();
    }
    const std::string basic = "Basic " + base64_bytes(bytes("lake:mover-push"));
    ASSERT_TRUE(push_image(alpha_name + "/web", "v1", R"({"w":1})", "w1", basic).starts_with("{"));
    ASSERT_TRUE(push_blob(alpha_name + "/cache/web", "cache", basic).starts_with("sha256:"));

    // Refused targets move nothing.
    for (const json& bad : {json{{"customer_id", alpha}, {"name", "web"}, {"workspace_id", ""}},
                            json{{"customer_id", alpha}, {"name", "web"},
                                 {"workspace_id", lake::control::new_uuid_v4()}},
                            json{{"customer_id", alpha}, {"name", "web"}, {"workspace_id", b1}}}) {
        const json refused = call("unroute_image_repository", bad);
        EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused;
    }
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, w1);
    EXPECT_EQ(store->get_oci_repository_route(alpha, "cache/web")->workspace_id, w1);

    // The old workspace's build keeps pushing while the operator moves the image.
    std::atomic<bool> moving{true};
    std::atomic<int> attempts{0};
    std::thread pusher([&] {
        // Either outcome of each request is fine here (accepted before the move, refused after
        // it); only where the route ends matters.
        const auto attempt = [&](const std::string& repository, const std::string& contents) {
            const std::string started =
                http_request(server->port(), "POST", "/v2/" + repository + "/blobs/uploads/", "",
                             "application/octet-stream", {}, basic);
            const std::string location = response_header(started, "Location");
            if (!location.empty()) {
                http_request(server->port(), "PUT",
                             location + "?digest=sha256:" +
                                 lake::control::identity_crypto::sha256_hex(contents),
                             contents, "application/octet-stream", {}, basic);
            }
        };
        while (moving.load() || attempts.load() < 20) {
            attempt(alpha_name + "/web", "racing-" + std::to_string(attempts.load()));
            attempt(alpha_name + "/cache/web", "racing-cache");
            ++attempts;
        }
    });
    while (attempts.load() < 5) {
        std::this_thread::yield();
    }
    const json moved = call("unroute_image_repository",
                            {{"customer_id", alpha}, {"name", "web"}, {"workspace_id", w2}});
    moving = false;
    pusher.join();
    ASSERT_TRUE(moved.value("ok", false)) << moved;
    ASSERT_EQ(moved["value"]["routes"].size(), 2U) << moved;
    EXPECT_EQ(moved["value"]["routes"][1]["name"], "cache/web");
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, w2);
    EXPECT_EQ(store->get_oci_repository_route(alpha, "cache/web")->workspace_id, w2);
    const std::string late = push_blob(alpha_name + "/web", "late", basic);
    EXPECT_NE(late.find(" 403 "), std::string::npos) << late;
    EXPECT_EQ(workspaces->records(w1).rows("oci_tags").size(), 1U) << "w1 keeps its records";

    // The cache alone may be unrouted; its image stays.
    const json cache_only = call("unroute_image_repository", {{"customer_id", alpha}, {"name", "cache/web"}});
    ASSERT_TRUE(cache_only.value("ok", false)) << cache_only;
    EXPECT_EQ(cache_only["value"]["routes"].size(), 1U);
    EXPECT_FALSE(store->get_oci_repository_route(alpha, "cache/web"));
    EXPECT_EQ(store->get_oci_repository_route(alpha, "web")->workspace_id, w2);
    server->stop();
}

INSTANTIATE_TEST_SUITE_P(Stores, TenantImages, testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

TEST(RegistryProtocols, NpmPublishesAndInstallsWithTheRealClient) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "npm-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = (temporary.path() / "store").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    if (std::system("command -v npm >/dev/null 2>&1") != 0) {
        const std::string tarball = "protocol-level npm fixture";
        const json metadata{{"name", "lake-issue-3-fixture"},
                            {"dist-tags", {{"latest", "1.0.0"}}},
                            {"versions",
                             {{"1.0.0",
                               {{"name", "lake-issue-3-fixture"},
                                {"version", "1.0.0"},
                                {"dist", {{"tarball", "placeholder"}}}}}}},
                            {"_attachments",
                             {{"lake-issue-3-fixture-1.0.0.tgz",
                               {{"content_type", "application/octet-stream"},
                                {"data", base64_bytes(bytes(tarball))},
                                {"length", tarball.size()}}}}}};
        const std::string published = http_request(server.port(),
                                                   "PUT",
                                                   "/lake-issue-3-fixture",
                                                   metadata.dump(),
                                                   "application/json",
                                                   {},
                                                   "Bearer registry-admin");
        ASSERT_NE(published.find(" 201 "), std::string::npos) << published;
        const std::string pulled = http_request(server.port(),
                                                "GET",
                                                "/lake-issue-3-fixture/-/"
                                                "lake-issue-3-fixture-1.0.0.tgz",
                                                "",
                                                "application/octet-stream");
        EXPECT_EQ(response_body(pulled), tarball);
        server.stop();
        return;
    }

    const std::filesystem::path package = temporary.path() / "package";
    const std::filesystem::path installed = temporary.path() / "installed";
    const std::filesystem::path cache = temporary.path() / "npm-cache";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(installed);
    {
        std::ofstream output(package / "package.json");
        output << R"({"name":"lake-issue-3-fixture","version":"1.0.0","main":"index.js"})";
    }
    {
        std::ofstream output(package / "index.js");
        output << "module.exports = 'published through lake';\n";
    }
    const std::string registry = "http://127.0.0.1:" + std::to_string(server.port());
    const std::filesystem::path user_config = temporary.path() / "npmrc";
    {
        std::ofstream output(user_config);
        output << "registry=" << registry << "/\n";
        output << "//127.0.0.1:" << server.port() << "/:_authToken=registry-admin\n";
    }
    const std::filesystem::path publish_log = temporary.path() / "npm-publish.log";
    const std::string publish_command = "npm publish '" + package.string() + "' --userconfig='" +
                                        user_config.string() + "' --cache='" + cache.string() +
                                        "' --ignore-scripts --loglevel=error >'" +
                                        publish_log.string() + "' 2>&1";
    ASSERT_EQ(std::system(publish_command.c_str()), 0) << read_binary_file(publish_log);
    const std::filesystem::path install_log = temporary.path() / "npm-install.log";
    const std::string install_command = "cd '" + installed.string() +
                                        "' && npm install lake-issue-3-fixture@1.0.0"
                                        " --userconfig='" +
                                        user_config.string() + "' --cache='" + cache.string() +
                                        "' --ignore-scripts --no-audit --no-fund --loglevel=error"
                                        " >'" +
                                        install_log.string() + "' 2>&1";
    ASSERT_EQ(std::system(install_command.c_str()), 0) << read_binary_file(install_log);
    EXPECT_EQ(read_binary_file(installed / "node_modules/lake-issue-3-fixture/index.js"),
              "module.exports = 'published through lake';\n");
    server.stop();
}

TEST(RegistryProtocols, PythonPackagePublishesAndInstallsWithPip) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "python-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = (temporary.path() / "store").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::filesystem::path wheel =
        temporary.path() / "lake_issue_3_fixture-1.0.0-py3-none-any.whl";
    const std::filesystem::path script = temporary.path() / "make_wheel.py";
    {
        std::ofstream output(script);
        output << R"py(import pathlib
import sys
import zipfile

wheel = pathlib.Path(sys.argv[1])
files = {
    "lake_issue_3_fixture/__init__.py": "value = 'published through lake'\n",
    "lake_issue_3_fixture-1.0.0.dist-info/METADATA": (
        "Metadata-Version: 2.1\nName: lake-issue-3-fixture\nVersion: 1.0.0\n"
    ),
    "lake_issue_3_fixture-1.0.0.dist-info/WHEEL": (
        "Wheel-Version: 1.0\nGenerator: lake-test\nRoot-Is-Purelib: true\nTag: py3-none-any\n"
    ),
    "lake_issue_3_fixture-1.0.0.dist-info/RECORD": "",
}
with zipfile.ZipFile(wheel, "w", zipfile.ZIP_DEFLATED) as archive:
    for name, contents in files.items():
        archive.writestr(name, contents)
)py";
    }
    const std::string wheel_command = "python3 '" + script.string() + "' '" + wheel.string() + "'";
    ASSERT_EQ(std::system(wheel_command.c_str()), 0);
    const std::string wheel_bytes = read_binary_file(wheel);
    const std::string boundary = "lake-issue-3-boundary";
    std::string publication;
    const auto add_field = [&](std::string_view name, std::string_view value) {
        publication += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" +
                       std::string(name) + "\"\r\n\r\n" + std::string(value) + "\r\n";
    };
    add_field("name", "lake-issue-3-fixture");
    add_field("version", "1.0.0");
    publication +=
        "--" + boundary + "\r\nContent-Disposition: form-data; name=\"content\"; filename=\"" +
        wheel.filename().string() + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" +
        wheel_bytes + "\r\n--" + boundary + "--\r\n";
    const std::string published = http_request(server.port(),
                                               "POST",
                                               "/legacy/",
                                               publication,
                                               "multipart/form-data; boundary=" + boundary,
                                               {},
                                               "Bearer registry-admin");
    ASSERT_NE(published.find(" 201 "), std::string::npos) << published;

    const std::string simple =
        http_request(server.port(), "GET", "/simple/lake-issue-3-fixture/", "", "text/html");
    ASSERT_NE(simple.find(wheel.filename().string()), std::string::npos) << simple;
    if (std::system("command -v pip3 >/dev/null 2>&1") == 0) {
        const std::filesystem::path target = temporary.path() / "python-installed";
        const std::filesystem::path install_log = temporary.path() / "pip-install.log";
        const std::string registry =
            "http://127.0.0.1:" + std::to_string(server.port()) + "/simple";
        const std::string install_command =
            "pip3 install --index-url '" + registry +
            "' --trusted-host 127.0.0.1"
            " --target '" +
            target.string() +
            "' --no-deps --disable-pip-version-check lake-issue-3-fixture==1.0.0 >'" +
            install_log.string() + "' 2>&1";
        ASSERT_EQ(std::system(install_command.c_str()), 0) << read_binary_file(install_log);
        EXPECT_EQ(read_binary_file(target / "lake_issue_3_fixture/__init__.py"),
                  "value = 'published through lake'\n");
    } else {
        const std::string downloaded = http_request(server.port(),
                                                    "GET",
                                                    "/packages/" + wheel.filename().string(),
                                                    "",
                                                    "application/octet-stream");
        EXPECT_EQ(response_body(downloaded), wheel_bytes);
    }
    server.stop();
}

TEST(RegistryProtocols, DebuginfodServesExecutableDebuginfoAndSource) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "debug-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "72656769737472792d746573742d696e7374616e63652d6b656b2d6865782121";
    options.store_root = (temporary.path() / "store").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string executable = read_binary_file(LAKE_TEST_DEBUG_FIXTURE_PATH);
    ASSERT_FALSE(executable.empty());
    const std::string build_id =
        lake::control::identity_crypto::sha256_hex(executable).substr(0, 40);
    const std::string debug_info = executable;
    const std::string source =
        read_binary_file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "tests/debug_fixture.cpp");
    const auto publish =
        [&](std::string_view kind, std::string_view query, std::string_view contents) {
            return http_request(server.port(),
                                "PUT",
                                "/_lake/debug/" + build_id + "/" + std::string(kind) +
                                    std::string(query),
                                contents,
                                "application/octet-stream",
                                {},
                                "Bearer registry-admin");
        };
    EXPECT_NE(publish("executable", "", executable).find(" 201 "), std::string::npos);
    EXPECT_NE(publish("debuginfo", "", debug_info).find(" 201 "), std::string::npos);
    EXPECT_NE(publish("source", "?source=debug_fixture.cpp", source).find(" 201 "),
              std::string::npos);
    const auto fetch = [&](std::string_view suffix) {
        return response_body(http_request(server.port(),
                                          "GET",
                                          "/buildid/" + build_id + "/" + std::string(suffix),
                                          "",
                                          "application/octet-stream"));
    };
    EXPECT_EQ(fetch("executable"), executable);
    EXPECT_EQ(fetch("debuginfo"), debug_info);
    EXPECT_EQ(fetch("source/debug_fixture.cpp"), source);
    server.stop();
}

TEST(Registry, ServerInfoNeedsNoAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("server_info");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value["name"], "lake");
    EXPECT_EQ(r.value["store"], "memory");
    EXPECT_EQ(r.value["revision"], lake::build_revision_string());
    // lake#1119: the Stripe events this version handles, as `lake manifest` publishes them.
    nlohmann::json events = nlohmann::json::array();
    for (const auto event : lake::api::stripe_webhook_events) {
        events.push_back(std::string(event));
    }
    EXPECT_EQ(r.value["stripe_webhook_events"], events);
    // lake#1324: what a liquid client checks, read from the code, never repeated here.
    EXPECT_EQ(r.value["liquid"], lake::api::liquid_clients_json());
    EXPECT_EQ(r.value["liquid"]["sync_protocol"], lake::api::liquid_sync_protocol_version);
    EXPECT_EQ(r.value["liquid"]["forge_api"], lake::api::liquid_forge_api_version);
    EXPECT_EQ(r.value["liquid"]["oldest_release"], lake::api::oldest_supported_liquid);
    EXPECT_EQ(r.value["liquid"]["newest_release"], liquid::version_string());
}

// lake#1324: the supported range is [oldest_supported_liquid, the linked libliquid].
TEST(LiquidClients, TheRangeRunsFromTheFirstReleaseToTheLinkedLiquid) {
    using lake::api::LiquidRelease;
    using lake::api::parse_liquid_release;
    const auto oldest = parse_liquid_release(lake::api::oldest_supported_liquid);
    ASSERT_TRUE(oldest);
    const LiquidRelease newest = lake::api::newest_supported_liquid();
    EXPECT_EQ(lake::api::to_string(newest), liquid::version_string());
    EXPECT_LE(*oldest, newest);
    // The content type the sync routes speak is the one published.
    EXPECT_EQ(lake::api::liquid_sync_media_type,
              "application/vnd.liquid.sync-v" +
                  std::to_string(lake::api::liquid_sync_protocol_version));

    EXPECT_EQ(lake::api::liquid_client_refusal(lake::api::to_string(*oldest)), "");
    EXPECT_EQ(lake::api::liquid_client_refusal(lake::api::to_string(newest)), "");
    EXPECT_EQ(lake::api::liquid_client_refusal("v" + lake::api::to_string(newest)), "");
    EXPECT_EQ(lake::api::liquid_client_refusal(lake::api::to_string(newest) + "-rc.1"), "");

    const LiquidRelease too_new{newest.major, newest.minor, newest.patch + 1};
    const std::string newer = lake::api::liquid_client_refusal(lake::api::to_string(too_new));
    EXPECT_NE(newer.find("newer than this Lake supports"), std::string::npos) << newer;
    EXPECT_NE(newer.find(lake::api::to_string(newest)), std::string::npos) << newer;
    if (oldest->patch > 0 || oldest->minor > 0 || oldest->major > 0) {
        const LiquidRelease too_old{0, 0, 0};
        const std::string older = lake::api::liquid_client_refusal(lake::api::to_string(too_old));
        EXPECT_NE(older.find("older than this Lake supports"), std::string::npos) << older;
        EXPECT_NE(older.find("upgrade liquid"), std::string::npos) << older;
    }
    for (const std::string_view bad : {"", "1", "1.2", "1.2.x", "01.2.3", "1.2.3-", "1.2.3.4"}) {
        const std::string refused = lake::api::liquid_client_refusal(bad);
        EXPECT_NE(refused.find("is not a liquid release"), std::string::npos) << bad;
    }
    EXPECT_TRUE(lake::api::liquid_version_exempt("/v1/server_info"));
    EXPECT_FALSE(lake::api::liquid_version_exempt("/v1/viewer"));
    EXPECT_NE(lake::api::find_error_code("unsupported_client_version"), nullptr);
}

TEST(Telemetry, PrometheusCounterAndHistogram) {
    lake::telemetry::Metrics metrics;
    metrics.increment_counter("test_requests_total", {{"result", "ok"}});
    metrics.increment_counter("test_requests_total", {{"result", "ok"}}, 2.0);
    metrics.observe_histogram("test_duration_seconds", {{"operation", "read"}}, 0.012);

    const std::string text = metrics.prometheus_text();
    EXPECT_NE(text.find("# TYPE test_requests_total counter\n"), std::string::npos);
    EXPECT_NE(text.find("test_requests_total{result=\"ok\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE test_duration_seconds histogram\n"), std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"0.01\"} 0\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"0.025\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"+Inf\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_count{operation=\"read\"} 1\n"), std::string::npos);
}

TEST(Telemetry, StorePingIsMeasured) {
    lake::telemetry::Metrics metrics;
    auto store = lake::telemetry::instrument_store(std::make_shared<lake::control::InMemoryStore>(),
                                                   metrics);
    ASSERT_TRUE(store->ping());

    const std::string text = metrics.prometheus_text();
    EXPECT_NE(text.find("lake_store_calls_total{store=\"memory\",operation="
                        "\"ping\",result=\"ok\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("lake_store_call_duration_seconds_count{store=\"memory\","
                        "operation=\"ping\",result=\"ok\"} 1\n"),
              std::string::npos);
}

TEST(TelemetryHttp, HealthAndMemoryReadinessNeedNoAuth) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string health = http_get(server.port(), "/healthz");
    EXPECT_NE(health.find(" 200 "), std::string::npos);
    EXPECT_TRUE(health.ends_with("{\"ok\":true}"));

    const std::string ready = http_get(server.port(), "/readyz");
    EXPECT_NE(ready.find(" 200 "), std::string::npos);
    EXPECT_TRUE(ready.ends_with("{\"ok\":true,\"store\":\"memory\"}"));
    server.stop();
}

TEST(TelemetryHttp, MetricsRequireExistingAuthAndUsePrometheusContentType) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.admin_token = "test-token";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string denied = http_get(server.port(), "/metrics");
    EXPECT_NE(denied.find(" 403 "), std::string::npos);
    EXPECT_NE(denied.find("\"code\":\"unauthorized\""), std::string::npos);

    const std::string served = http_get(server.port(), "/metrics", "Bearer test-token");
    EXPECT_NE(served.find(" 200 "), std::string::npos);
    EXPECT_NE(served.find("Content-Type: text/plain; version=0.0.4"), std::string::npos);
    EXPECT_NE(served.find("lake_build_info{version=\"" + std::string(lake::version_string()) +
                          "\",node=\"test-node\"} 1\n"),
              std::string::npos);
    EXPECT_NE(served.find("lake_up 1\n"), std::string::npos);
    EXPECT_NE(served.find("lake_http_requests_total{method=\"GET\",route=\"/"
                          "metrics\",status=\"403\"} 1\n"),
              std::string::npos);
    server.stop();
}

// The browser session is the API's: password sign-up through /v1 sets the HttpOnly lake_session
// cookie, which then reads sessions and enrolls MFA. The former server pages are the web app's, so
// a request that is not a browser's gets the 404 (lake#561).
TEST(IdentityHttp, ApiSignUpIssuesAndResolvesHttpOnlySessions) {
    ServerOptions options;
    options.port = 0;
    options.public_host = "lake.example";
    options.admin_token = "operator-page-token";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    for (const std::string_view path : {"/sign-up", "/sign-in", "/mfa", "/sessions"}) {
        const std::string retired =
            http_request(server.port(), "GET", path, "", "text/plain", {}, {}, "lake.example");
        EXPECT_NE(retired.find(" 404 "), std::string::npos) << path << "\n" << retired;
    }
    const std::string form =
        http_request(server.port(),
                     "POST",
                     "/identity/sign-up",
                     "email=page%40example.test&display_name=Page+User&password=correct+horse+"
                     "battery+staple&device_name=browser&identity_document=%7B%7D",
                     "application/x-www-form-urlencoded",
                     {},
                     {},
                     "lake.example");
    EXPECT_NE(form.find(" 404 "), std::string::npos) << "the sign-up form is retired: " << form;

    const std::string created = http_request(
        server.port(),
        "POST",
        "/v1/sign_up_password",
        R"({"email":"page@example.test","password":"correct horse battery staple","display_name":"Page User","device_name":"browser","identity_document":"{}"})",
        "application/json",
        {},
        {},
        "lake.example");
    EXPECT_NE(created.find(" 200 "), std::string::npos) << created;
    const std::string set_cookie = response_header(created, "Set-Cookie");
    EXPECT_NE(set_cookie.find("HttpOnly"), std::string::npos);
    EXPECT_NE(set_cookie.find("SameSite=Lax"), std::string::npos);
    const std::string cookie = set_cookie.substr(0, set_cookie.find(';'));

    const std::string sessions = http_request(
        server.port(), "GET", "/v1/list_sessions", "", "text/plain", cookie, {}, "lake.example");
    EXPECT_NE(sessions.find(" 200 "), std::string::npos) << sessions;
    EXPECT_NE(sessions.find("browser"), std::string::npos);

    const std::string enrolled = http_request(server.port(),
                                              "POST",
                                              "/v1/enroll_mfa",
                                              R"({"password":"correct horse battery staple"})",
                                              "application/json",
                                              cookie,
                                              {},
                                              "lake.example");
    EXPECT_NE(enrolled.find(" 200 "), std::string::npos) << enrolled;
    EXPECT_NE(enrolled.find("otpauth://totp/lake:"), std::string::npos);
    EXPECT_NE(enrolled.find("recovery_codes"), std::string::npos);
    server.stop();
}

// lake#912: a session of an identity application's pool carries no Lake grants, even
// when its user's TAAP identity is a Lake account's. `pool_session` mints a session in `pool_id`
// for a user with `taap_id` (the Lake account `user_id` carries it) the way each store can.
void expect_application_sessions_carry_no_lake_grants(
    const std::shared_ptr<lake::control::Store>& store,
    const std::function<std::string(const std::string& pool_id,
                                    const std::string& taap_id,
                                    const std::string& user_id)>& pool_session) {
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    // Any signed-up account may register an application, and so own a pool.
    const auto owner = store->create_user({"taap:1:ed25519:pool-owner-" + suffix,
                                           "pool-owner-" + suffix + "@example.test",
                                           "Pool owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto owner_space =
        store->create_customer({"individual", owner->id, "", "pool-owner-" + suffix});
    ASSERT_TRUE(owner_space) << owner_space.error().message;
    const std::string application_id = lake::control::new_uuid_v4();
    const std::string pool_id = "app:" + application_id;
    const auto application = store->create_application(
        {.id = application_id,
         .customer_id = owner_space->id,
         .pool_id = pool_id,
         .workspace_id = lake::test::repository_workspace(*store, owner_space->id)});
    ASSERT_TRUE(application) << application.error().message;
    // The Lake account whose identity the application's user carries administers its space.
    const std::string taap_id = "taap:1:ed25519:pool-victim-" + suffix;
    const auto victim =
        store->create_user({taap_id, "pool-victim-" + suffix + "@example.test", "Victim"});
    ASSERT_TRUE(victim) << victim.error().message;
    const auto space =
        store->create_customer({"individual", victim->id, "", "pool-victim-" + suffix});
    ASSERT_TRUE(space) << space.error().message;
    const auto space_object = store->find_policy_object("customer", space->id);
    ASSERT_TRUE(space_object) << space_object.error().message;
    ASSERT_TRUE(store->create_grant({taap_id,
                                     space_object->id,
                                     "admin",
                                     lake::control::identity_crypto::sha256_hex(taap_id),
                                     "admin",
                                     lake::control::new_uuid_v4()}));
    const auto lake_token =
        store->create_token({victim->id, "lake", "lake account", "", {"*"}, 3600, false});
    ASSERT_TRUE(lake_token) << lake_token.error().message;
    const std::string pool_secret = pool_session(pool_id, taap_id, victim->id);
    ASSERT_FALSE(pool_secret.empty());
    const auto resolved = store->context_for(pool_secret);
    ASSERT_TRUE(resolved) << resolved.error().message;
    ASSERT_EQ(resolved->pool_id, pool_id);
    ASSERT_EQ(resolved->taap_id, taap_id);

    ServerOptions options;
    options.port = 0;
    options.store = store;
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string read = "/v1/get_customer?id=" + space->id;
    const std::string bearer = "Bearer " + pool_secret;

    // The Lake account reads its space.
    const std::string own = http_request(
        server.port(), "GET", read, "", "text/plain", {}, "Bearer " + lake_token->secret);
    EXPECT_NE(own.find(" 200 "), std::string::npos) << own;

    // The application's session does not: as a bearer, as the session cookie, or through MCP.
    const std::string as_bearer =
        http_request(server.port(), "GET", read, "", "text/plain", {}, bearer);
    EXPECT_NE(as_bearer.find(" 403 "), std::string::npos) << as_bearer;
    EXPECT_EQ(as_bearer.find("pool-victim-" + suffix), std::string::npos) << as_bearer;
    const std::string as_cookie =
        http_request(server.port(), "GET", read, "", "text/plain", "lake_session=" + pool_secret);
    EXPECT_EQ(as_cookie.find(" 200 "), std::string::npos) << as_cookie;
    EXPECT_EQ(as_cookie.find("pool-victim-" + suffix), std::string::npos) << as_cookie;
    const std::string listed =
        http_request(server.port(), "GET", "/v1/list_customers", "", "text/plain", {}, bearer);
    EXPECT_NE(listed.find(" 403 "), std::string::npos) << listed;
    const json call = {{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "tools/call"},
                       {"params", {{"name", "get_customer"}, {"arguments", {{"id", space->id}}}}}};
    const std::string through_mcp =
        http_request(server.port(), "POST", "/mcp", call.dump(), "application/json", {}, bearer);
    EXPECT_NE(through_mcp.find("\"isError\":true"), std::string::npos) << through_mcp;
    EXPECT_EQ(through_mcp.find("pool-victim-" + suffix), std::string::npos) << through_mcp;
    // The viewer sees a visitor, with none of the account's spaces.
    const json viewer = response_json(
        http_request(server.port(), "GET", "/v1/viewer", "", "text/plain", {}, bearer));
    EXPECT_FALSE(viewer.value("value", json::object()).value("signed_in", true)) << viewer.dump();
    EXPECT_EQ(viewer.dump().find(space->id), std::string::npos) << viewer.dump();
    const std::string metrics =
        http_request(server.port(), "GET", "/metrics", "", "text/plain", {}, bearer);
    EXPECT_EQ(metrics.find(" 200 "), std::string::npos) << metrics;

    // Its own sign-in in its pool still answers it.
    const std::string sessions =
        http_request(server.port(), "GET", "/v1/list_sessions", "", "text/plain", {}, bearer);
    EXPECT_NE(sessions.find(" 200 "), std::string::npos) << sessions;
    const json session_items = response_json(sessions).value("value", json::object());
    ASSERT_TRUE(session_items.contains("items")) << sessions;
    for (const json& item : session_items.at("items")) {
        EXPECT_EQ(item.value("pool_id", ""), pool_id) << item.dump();
    }
    server.stop();
}

TEST(ApplicationPoolSessions, MemoryStore) {
    // The in-memory store keeps one row per person and scopes credentials by pool, so the
    // application's user is the session in the application's pool for the account's own row.
    auto store = std::make_shared<lake::control::InMemoryStore>();
    expect_application_sessions_carry_no_lake_grants(
        store, [&](const std::string& pool_id, const std::string&, const std::string& user_id) {
            const auto issued =
                store->create_token({user_id, pool_id, "pool session", "", {"*"}, 3600, false});
            EXPECT_TRUE(issued) << issued.error().message;
            return issued ? issued->secret : std::string();
        });
}

TEST(ApplicationPoolSessions, PostgresStore) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    std::shared_ptr<lake::control::Store> store = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping());
    // PostgreSQL keeps users per (pool_id, taap_id): the application's user is its own row in the
    // pool, with the Lake account's identity, as an enrollment with that identity makes it.
    expect_application_sessions_carry_no_lake_grants(
        store, [&](const std::string& pool_id, const std::string& taap_id, const std::string&) {
            pqxx::connection connection(database_uri);
            pqxx::work tx(connection);
            const std::string user_id =
                tx.exec_params("INSERT INTO users (taap_id, email, display_name, pool_id) "
                               "VALUES ($1, $2, 'Pool user', $3) RETURNING id::text",
                               taap_id,
                               "pool-user-" + lake::control::new_uuid_v4() + "@example.test",
                               pool_id)[0][0]
                    .as<std::string>();
            tx.commit();
            const auto issued =
                store->create_token({user_id, pool_id, "pool session", "", {"*"}, 3600, false});
            EXPECT_TRUE(issued) << issued.error().message;
            return issued ? issued->secret : std::string();
        });
}

TEST(ApplicationHttp, HostedAndCustomDomainPagesUseTheApplicationPool) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = store->create_user(
        {"taap:1:ed25519:http-app-owner", "http-app-owner@example.test", "HTTP owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        store->create_customer({"individual", owner->id, "", "http-application-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string application_id = lake::control::new_uuid_v4();
    // Sign-ups record the application's events in its workspace Zetta (lake#356).
    const auto zetta = std::make_shared<lake::test::StreamCaller>();
    const auto service_dir =
        lake::test::fixture_path("hosted-application-" + lake::control::new_uuid_v4());
    const std::string workspace_id =
        lake::test::ready_workspace(*store, service_dir, customer->id, "hosted");
    const auto application =
        store->create_application({.id = application_id,
                                   .customer_id = customer->id,
                                   .pool_id = "app:" + application_id,
                                   .custom_sign_in_domain = "login.product.example.test",
                                   .workspace_id = workspace_id});
    ASSERT_TRUE(application) << application.error().message;

    ServerOptions options;
    options.port = 0;
    options.public_host = "lake.example.test";
    options.store = store;
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    options.workspaces = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{service_dir.string(), 0}, store, zetta);
    // Its signing key lives in its workspace Zetta (lake#352).
    lake::test::seed_application_signing_key(*store, options.workspaces, customer->id,
                                              workspace_id, application_id,
                                              options.instance_kek);
    // Its settings too (lake#351): the hosted pages render them from there.
    lake::test::seed_application_settings(
        options.workspaces, workspace_id,
        {.application_id = application_id,
         .customer_id = customer->id,
         .name = "Hosted product",
         .allowed_origins = {"https://product.example.test"},
         .redirect_uris = {"https://product.example.test/callback"},
         .theme = {{"accent_color", "#123456"}}},
        options.instance_kek);
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string hosted = http_request(
        server.port(), "GET", "/apps/" + application_id + "/sign-up", "", "text/plain");
    EXPECT_NE(hosted.find(" 200 "), std::string::npos) << hosted;
    EXPECT_NE(hosted.find("Hosted product"), std::string::npos);
    EXPECT_NE(hosted.find("--accent:#123456"), std::string::npos);
    EXPECT_NE(hosted.find("id=\"theme-preference\""), std::string::npos);
    EXPECT_NE(hosted.find("id=\"theme-toggle-script\""), std::string::npos);

    const std::string custom = http_request(
        server.port(), "GET", "/sign-in", "", "text/plain", "", "", "login.product.example.test");
    EXPECT_NE(custom.find(" 200 "), std::string::npos) << custom;
    EXPECT_NE(custom.find("Hosted product"), std::string::npos);
    EXPECT_NE(custom.find("/apps/" + application_id + "/identity/sign-in"), std::string::npos);

    const std::string custom_mfa = http_request(
        server.port(), "GET", "/mfa", "", "text/plain", "", "", "login.product.example.test");
    EXPECT_NE(custom_mfa.find(" 200 "), std::string::npos) << custom_mfa;
    EXPECT_NE(custom_mfa.find("Hosted product"), std::string::npos);
    EXPECT_NE(custom_mfa.find("/apps/" + application_id + "/identity/mfa/verify"),
              std::string::npos);

    const std::string custom_passkey = http_request(server.port(),
                                                    "GET",
                                                    "/passkey-sign-in",
                                                    "",
                                                    "text/plain",
                                                    "",
                                                    "",
                                                    "login.product.example.test");
    EXPECT_NE(custom_passkey.find(" 200 "), std::string::npos) << custom_passkey;
    EXPECT_NE(custom_passkey.find("Hosted product"), std::string::npos);
    EXPECT_NE(custom_passkey.find("navigator.credentials.get"), std::string::npos);

    const std::string created = http_request(
        server.port(),
        "POST",
        "/apps/" + application_id + "/identity/sign-up",
        "email=hosted%40example.test&display_name=Hosted+User&password=correct+horse+battery+"
        "staple&device_name=browser&identity_document=%7B%7D",
        "application/x-www-form-urlencoded");
    EXPECT_NE(created.find(" 302 "), std::string::npos) << created;
    EXPECT_TRUE(response_header(created, "Location")
                    .starts_with("https://product.example.test/callback#access_token="));
    EXPECT_NE(response_header(created, "Location").find("&token_type=Bearer&expires_in=900"),
              std::string::npos);
    EXPECT_NE(response_header(created, "Set-Cookie").find("HttpOnly"), std::string::npos);

    const std::string signed_in = http_request(
        server.port(),
        "POST",
        "/apps/" + application_id + "/identity/sign-in",
        "email=hosted%40example.test&password=correct+horse+battery+staple&device_name=browser",
        "application/x-www-form-urlencoded");
    EXPECT_NE(signed_in.find(" 302 "), std::string::npos) << signed_in;

    const std::string jwks = http_request(server.port(),
                                          "GET",
                                          "/v1/apps/" + application_id + "/.well-known/jwks.json",
                                          "",
                                          "application/json");
    EXPECT_NE(jwks.find(" 200 "), std::string::npos) << jwks;
    const json keys = response_json(jwks);
    ASSERT_EQ(keys.at("keys").size(), 1);
    EXPECT_EQ(keys.at("keys").at(0).at("kid"), "app-" + application_id + "-1");
    server.stop();
}

TEST(ScimHttp, RecordedClientSessionProvisionsUsersGroupsAndRevokesAccess) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto organization = store->create_organization({"", "SCIM HTTP", ""});
    ASSERT_TRUE(organization.has_value());
    const std::string token = "recorded-scim-token";
    const auto provider = store->register_sso_provider({organization->id,
                                                        "lake",
                                                        "recorded-client",
                                                        "oidc",
                                                        "https://identity.example.test",
                                                        "",
                                                        "",
                                                        "recorded-client-id",
                                                        "recorded-client-secret",
                                                        token,
                                                        json::object(),
                                                        false});
    ASSERT_TRUE(provider.has_value());

    ServerOptions options;
    options.port = 0;
    options.store = store;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string denied = http_get(server.port(), "/scim/v2/Users");
    EXPECT_NE(denied.find(" 401 "), std::string::npos) << denied;
    EXPECT_NE(denied.find("urn:ietf:params:scim:api:messages:2.0:Error"), std::string::npos);

    const auto create_user = [&](std::string_view external_id,
                                 std::string_view email,
                                 std::string_view display_name,
                                 std::string_view taap_id) {
        const json body{
            {"schemas", json::array({"urn:ietf:params:scim:schemas:core:2.0:User"})},
            {"externalId", external_id},
            {"userName", email},
            {"displayName", display_name},
            {"active", true},
            {"urn:orama:params:scim:schemas:extension:taap:2.0:User", {{"taapId", taap_id}}}};
        return http_request(server.port(),
                            "POST",
                            "/scim/v2/Users",
                            body.dump(),
                            "application/scim+json",
                            "",
                            "Bearer " + token);
    };
    const std::string first_response =
        create_user("user-one", "one@example.test", "One", "taap:1:ed25519:user-one");
    EXPECT_NE(first_response.find(" 201 "), std::string::npos) << first_response;
    const json first = response_json(first_response);
    ASSERT_TRUE(first.is_object());
    const std::string first_id = first.at("id");
    const std::string second_response =
        create_user("user-two", "two@example.test", "Two", "taap:1:ed25519:user-two");
    EXPECT_NE(second_response.find(" 201 "), std::string::npos) << second_response;
    const json second = response_json(second_response);
    ASSERT_TRUE(second.is_object());
    const std::string second_id = second.at("id");

    const std::string filtered =
        http_get(server.port(),
                 "/scim/v2/Users?filter=userName%20eq%20%22one%40example.test%22",
                 "Bearer " + token);
    EXPECT_NE(filtered.find(" 200 "), std::string::npos) << filtered;
    const json filtered_body = response_json(filtered);
    ASSERT_TRUE(filtered_body.is_object());
    EXPECT_EQ(filtered_body.at("totalResults"), 1);
    EXPECT_EQ(filtered_body.at("Resources").at(0).at("id"), first_id);

    const json user_patch{
        {"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:PatchOp"})},
        {"Operations",
         json::array({{{"op", "replace"}, {"path", "displayName"}, {"value", "One Updated"}}})}};
    const std::string patched_user = http_request(server.port(),
                                                  "PATCH",
                                                  "/scim/v2/Users/" + first_id,
                                                  user_patch.dump(),
                                                  "application/scim+json",
                                                  "",
                                                  "Bearer " + token);
    EXPECT_NE(patched_user.find(" 200 "), std::string::npos) << patched_user;
    EXPECT_EQ(response_json(patched_user).at("displayName"), "One Updated");

    const json group_body{
        {"schemas", json::array({"urn:ietf:params:scim:schemas:core:2.0:Group"})},
        {"externalId", "engineering"},
        {"displayName", "Engineering"},
        {"members", json::array({{{"value", first_id}}, {{"value", second_id}}})}};
    const std::string group_response = http_request(server.port(),
                                                    "POST",
                                                    "/scim/v2/Groups",
                                                    group_body.dump(),
                                                    "application/scim+json",
                                                    "",
                                                    "Bearer " + token);
    EXPECT_NE(group_response.find(" 201 "), std::string::npos) << group_response;
    const json group = response_json(group_response);
    ASSERT_TRUE(group.is_object());
    const std::string group_id = group.at("id");
    auto memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_EQ(memberships->size(), 2);

    const json group_patch{
        {"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:PatchOp"})},
        {"Operations",
         json::array({{{"op", "remove"}, {"path", "members[value eq \"" + first_id + "\"]"}}})}};
    const std::string patched_group = http_request(server.port(),
                                                   "PATCH",
                                                   "/scim/v2/Groups/" + group_id,
                                                   group_patch.dump(),
                                                   "application/scim+json",
                                                   "",
                                                   "Bearer " + token);
    EXPECT_NE(patched_group.find(" 200 "), std::string::npos) << patched_group;
    memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_EQ(memberships->size(), 1);
    EXPECT_EQ(memberships->at(0).principal_id, second.at("id").get<std::string>());

    ASSERT_TRUE(
        store->create_federation_issuer({organization->id, provider->issuer, "email"}).has_value());
    const auto session = store->sign_in_sso({provider->id,
                                             provider->issuer,
                                             "user-two",
                                             "two@example.test",
                                             "Two",
                                             "",
                                             "recorded-client",
                                             {{"email", "two@example.test"}}});
    ASSERT_TRUE(session.has_value());
    const std::string deleted_user = http_request(server.port(),
                                                  "DELETE",
                                                  "/scim/v2/Users/" + second_id,
                                                  "",
                                                  "application/scim+json",
                                                  "",
                                                  "Bearer " + token);
    EXPECT_NE(deleted_user.find(" 204 "), std::string::npos) << deleted_user;
    EXPECT_FALSE(store->context_for(session->secret).has_value());
    memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_TRUE(memberships->empty());

    const std::string deleted_group = http_request(server.port(),
                                                   "DELETE",
                                                   "/scim/v2/Groups/" + group_id,
                                                   "",
                                                   "application/scim+json",
                                                   "",
                                                   "Bearer " + token);
    EXPECT_NE(deleted_group.find(" 204 "), std::string::npos) << deleted_group;
    EXPECT_FALSE(store->get_team(group_id).has_value());
    const auto events = lake::test::audit_events(*store, {.limit = 100});
    ASSERT_TRUE(events.has_value());
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.action == "scim_delete_user" && event.draft.outcome == "success";
    }));
    server.stop();
}

TEST(Registry, EverythingElseNeedsAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("list_users");
    EXPECT_FALSE(r.ok);
    // No credential at all is 401 unauthenticated, saying so (lake#450).
    EXPECT_EQ(r.error.code, "unauthenticated");
    EXPECT_EQ(r.error.reason, "missing");
    EXPECT_EQ(rest_status(r), 401);
}

TEST(Registry, ValidatesInput) {
    Surface s;
    ApiResult r = s.call("create_customer", json::object());
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(r.error.path, "/customer_type");
    r = s.call("get_user", {{"id", "x"}, {"bogus", 1}});
    EXPECT_EQ(r.error.path, "/bogus");
    r = s.call("list_users", {{"limit", "ten"}});
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(s.call("no_such").error.code, "unknown_capability");
}

TEST(Registry, NestedArraysReportEscapedJsonPointers) {
    Capability capability;
    capability.input_schema = {
        {"type", "object"},
        {"properties",
         {{"rows",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties", {{"a~/b", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
              {"required", {"a~/b"}},
              {"additionalProperties", false}}}}}}},
        {"required", {"rows"}},
        {"additionalProperties", false}};
    json input{{"rows", {{{"a~/b", {1, 2}}}}}};
    EXPECT_TRUE(validate_input(capability, input).ok);
    input["rows"][0]["a~/b"][1] = "wrong";
    const auto wrong_item = validate_input(capability, input);
    EXPECT_EQ(wrong_item.error.code, "invalid_input");
    EXPECT_EQ(wrong_item.error.path, "/rows/0/a~0~1b/1");
    input["rows"][0] = json::object();
    EXPECT_EQ(validate_input(capability, input).error.path, "/rows/0/a~0~1b");
    input["rows"][0] = false;
    EXPECT_EQ(validate_input(capability, input).error.path, "/rows/0");
}

TEST(Audit, RegistryRecordsEachCapabilityOutcomeAndDetectsDeletedRows) {
    Surface s;
    for (const auto& capability : s.registry.list()) {
        s.call(capability->name);
    }
    auto events = lake::test::audit_events(s.store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), s.registry.list().size());
    for (const auto& capability : s.registry.list()) {
        const auto count =
            std::count_if(events->begin(), events->end(), [&](const lake::audit::Event& event) {
                return event.draft.action == capability->name;
            });
        EXPECT_EQ(count, 1) << capability->name;
    }
    EXPECT_TRUE(lake::audit::verify(s.store, *events));

    std::vector<lake::audit::Event> deleted = *events;
    deleted.erase(deleted.begin() + 1);
    const auto verification = lake::audit::verify(s.store, deleted);
    EXPECT_FALSE(verification);
    EXPECT_EQ(verification.error().code, "invalid_chain");
}

TEST(Audit, DeniedAuthorizationAndJsonLinesExportAreRecorded) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult denied = s.call("list_users");
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthenticated");

    s.ctx.authorized = true;
    const ApiResult exported = s.call("export_audit");
    ASSERT_TRUE(exported.ok) << exported.error.message;
    std::istringstream lines(exported.value["jsonl"].get<std::string>());
    std::string line;
    std::vector<json> decoded;
    while (std::getline(lines, line)) {
        if (!line.empty()) {
            decoded.push_back(json::parse(line));
        }
    }
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0]["outcome"], "denied");
    EXPECT_EQ(decoded[0]["action"], "list_users");

    auto events = lake::test::audit_events(s.store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 2u);
    EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(Audit, IdentityRotatesAndRetentionFloorIsEnforced) {
    Surface s;
    const ApiResult initial = s.call("identity");
    ASSERT_TRUE(initial.ok) << initial.error.message;
    const std::string initial_identity = initial.value["taap_id"];
    const ApiResult rotated = s.call("rotate_identity");
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_NE(rotated.value["taap_id"], initial_identity);

    lake::control::InMemoryStore externally_seeded_store;
    const lake::audit::IdentityOptions externally_seeded_options{
        .configured_seed = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        .instance_kek = "test-instance-kek",
    };
    auto externally_seeded =
        lake::audit::identity(externally_seeded_store, externally_seeded_options);
    ASSERT_TRUE(externally_seeded) << externally_seeded.error().message;
    auto externally_rotated =
        lake::audit::rotate_identity(externally_seeded_store, externally_seeded_options);
    ASSERT_TRUE(externally_rotated) << externally_rotated.error().message;
    lake::audit::EventDraft externally_seeded_event{
        .kind = "administrative",
        .principal = "admin",
        .object = "lake",
        .action = "verify_external_rotation",
        .outcome = "success",
        .request_id = lake::control::new_uuid_v4(),
    };
    auto recorded = lake::audit::record(
        externally_seeded_store, externally_seeded_event, externally_seeded_options);
    ASSERT_TRUE(recorded) << recorded.error().message;
    auto externally_seeded_events =
        lake::test::audit_events(externally_seeded_store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(externally_seeded_events);
    EXPECT_TRUE(lake::audit::verify(externally_seeded_store, *externally_seeded_events));

    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult accepted =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 500}});
    EXPECT_TRUE(accepted.ok);
    const ApiResult lowered =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 400}});
    EXPECT_FALSE(lowered.ok);
    EXPECT_EQ(lowered.error.code, "conflict");
    const ApiResult refused =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 399}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");

    auto events = lake::test::audit_events(s.store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(events);
    const auto retention =
        std::find_if(events->rbegin(), events->rend(), [](const lake::audit::Event& event) {
            return event.draft.kind == "retention" && event.draft.outcome == "failure";
        });
    ASSERT_NE(retention, events->rend());
    EXPECT_EQ(retention->draft.action, "set_audit_retention");
    EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(PostgresAudit, RegistryEventsPersistAndVerify) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = store->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const auto before = lake::test::audit_events(*store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(before) << before.error().message;
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    ApiContext context;
    context.store = store.get();
    context.node_name = "postgres-test";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    context.authorized = true;
    context.acting_identity = "admin";

    for (const auto& capability : registry.list()) {
        context.request_id.clear();
        registry.call(capability->name, json::object(), context);
    }
    context.authorized = false;
    context.request_id.clear();
    const ApiResult result = registry.call("list_users", json::object(), context);
    EXPECT_EQ(result.error.code, "unauthenticated");

    auto events = lake::test::audit_events(*store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_EQ(events->size(), before->size() + registry.list().size() + 1);
    EXPECT_EQ(events->back().draft.outcome, "denied");
    const auto verification = lake::audit::verify(*store, *events);
    EXPECT_TRUE(verification) << verification.error().message;
}

TEST(Customers, IndividualLifecycleWithGeneratedHandle) {
    Surface s;
    const ApiResult user =
        s.call("create_user", {{"email", "Ada@Example.com"}, {"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const std::string user_id = user.value["id"];
    EXPECT_EQ(user_id.size(), 36u);

    const ApiResult customer =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string name = customer.value["name"];
    EXPECT_TRUE(lake::control::valid_handle(name)) << name;
    EXPECT_EQ(customer.value["user_id"], user_id);
    EXPECT_TRUE(customer.value["organization_id"].is_null());

    const ApiResult again =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    EXPECT_EQ(again.error.code, "conflict"); // one customer per user
    EXPECT_EQ(rest_status(again), 409);

    const ApiResult dup_email = s.call("create_user", {{"email", "ada@example.com"}});
    EXPECT_EQ(dup_email.error.code, "conflict"); // case-insensitive email

    const ApiResult listed = s.call("list_customers");
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["items"].size(), 1u);
    const ApiResult got = s.call("get_customer", {{"id", customer.value["id"]}});
    ASSERT_TRUE(got.ok);
    EXPECT_EQ(got.value["name"], name);
    EXPECT_EQ(s.call("get_customer", {{"id", "missing"}}).error.code, "not_found");
}
TEST(Customers, OrganizationAndTheTenantRules) {
    Surface s;
    const ApiResult org = s.call("create_organization",
                                 {{"display_name", "Orama"}, {"taap_id", "taap:1:ed25519:abc"}});
    ASSERT_TRUE(org.ok);
    const std::string org_id = org.value["id"];
    EXPECT_EQ(s.call("create_organization", {{"taap_id", "taap:1:ed25519:abc"}}).error.code,
              "conflict");

    // xor: an organization customer must not carry a user_id; type must match the
    // id given.
    ApiResult bad = s.call(
        "create_customer",
        {{"customer_type", "organization"}, {"organization_id", org_id}, {"user_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad = s.call("create_customer", {{"customer_type", "individual"}, {"organization_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad =
        s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", "nope"}});
    EXPECT_EQ(bad.error.code, "invalid_input"); // FK

    // Handle format and the single global namespace.
    bad = s.call(
        "create_customer",
        {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "Orama Inc"}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    const ApiResult ok =
        s.call("create_customer",
               {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "orama"}});
    ASSERT_TRUE(ok.ok) << ok.error.message;
    const ApiResult user = s.call("create_user", {{"display_name", "Bob"}});
    const ApiResult taken =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "orama"}});
    EXPECT_EQ(taken.error.code, "conflict");
}

TEST(Rest, PathsMethodsAndQueryCoercion) {
    Surface s;
    const auto list = s.registry.find("list_users");
    const auto create = s.registry.find("create_user");
    EXPECT_EQ(rest_path(*list), "/v1/list_users");
    EXPECT_EQ(rest_method(*list), "GET");
    EXPECT_EQ(rest_method(*create), "POST");
    const json input = rest_input_from_query(*list, {{"limit", "5"}, {"other", "x"}});
    EXPECT_EQ(input["limit"], 5);
    EXPECT_EQ(input["other"], "x");
    const json envelope = rest_envelope(ApiResult::failure("not_found", "gone", "/id"));
    EXPECT_FALSE(envelope["ok"]);
    EXPECT_EQ(envelope["error"]["code"], "not_found");
    EXPECT_EQ(rest_status(ApiResult::failure("mfa_required", "verify MFA")), 403);
    EXPECT_EQ(rest_status(ApiResult::failure("store_unavailable", "")), 503);
}

// A GET read's array input (lake#576): repeated keys, a comma list and a JSON array all decode
// as the array, each item coerced by the declared item type.
TEST(Rest, ArrayInputsDecodeFromRepeatedKeysCommaListsAndJsonArrays) {
    Surface s;
    const auto permissions = s.registry.find("my_permissions");
    ASSERT_NE(permissions, nullptr);
    EXPECT_EQ(rest_input_from_query(*permissions, {{"actions", "read"}, {"actions", "admin"}})
                  .at("actions"),
              json::array({"read", "admin"}));
    EXPECT_EQ(rest_input_from_query(*permissions, {{"actions", "read,admin"}}).at("actions"),
              json::array({"read", "admin"}));
    EXPECT_EQ(
        rest_input_from_query(*permissions, {{"actions", R"(["read","admin"])"}}).at("actions"),
        json::array({"read", "admin"}));
    EXPECT_EQ(rest_input_from_query(*permissions, {{"actions", "read"}}).at("actions"),
              json::array({"read"}));
    EXPECT_EQ(rest_input_from_query(*permissions, {{"actions", ""}}).at("actions"), json::array());
    Capability numbers{
        .name = "numbers",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"steps", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}}}};
    EXPECT_EQ(rest_input_from_query(numbers, {{"steps", "1,2"}, {"steps", "3"}}).at("steps"),
              json::array({1, 2, 3}));
}

// lake#594: a newcomer reads every operation by a human title, and an MCP client warns only before
// a delete or destroy.
TEST(Mcp, EveryToolHasAHumanTitleAndOnlyDeletesAreDestructive) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "0.0.1");
    std::vector<std::string> untitled;
    for (const auto& capability : registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
        if (capability->title.empty() || capability->title == capability->name ||
            capability->title.contains('_')) {
            untitled.push_back(capability->name + " (" + capability->title + ")");
        }
    }
    EXPECT_TRUE(untitled.empty()) << json(untitled).dump();
    const json tools = lake::api::mcp_tools(registry, lake::api::SchemaAudience::Caller);
    const auto tool = [&](std::string_view name) {
        return *std::ranges::find_if(tools, [&](const json& t) { return t["name"] == name; });
    };
    EXPECT_EQ(tool("create_drive")["annotations"]["destructiveHint"], false);
    EXPECT_EQ(tool("create_drive")["annotations"]["readOnlyHint"], false);
    EXPECT_EQ(tool("delete_workspace")["annotations"]["destructiveHint"], true);
    EXPECT_EQ(tool("destroy_secret_versions")["annotations"]["destructiveHint"], true);
    EXPECT_EQ(tool("set_member_role")["annotations"]["idempotentHint"], true);
    EXPECT_EQ(tool("create_drive")["annotations"]["idempotentHint"], false);
    EXPECT_EQ(tool("list_drives")["annotations"]["readOnlyHint"], true);
    // predict is a read in tools/list and in mcp_setup alike.
    EXPECT_EQ(tool("predict")["annotations"]["readOnlyHint"], true);
    EXPECT_FALSE(lake::api::capability_writes(*registry.find("predict")));
    // workspace_sql also takes POST with a JSON body, so a long statement need not fit a URL.
    const json doc = openapi_document(registry, "0.0.1");
    EXPECT_TRUE(doc["paths"]["/v1/workspace_sql"].contains("get"));
    EXPECT_TRUE(doc["paths"]["/v1/workspace_sql"].contains("post"));
    // Every operation's summary is its human title.
    EXPECT_EQ(doc["paths"]["/v1/delete_workspace"]["post"]["summary"], "Delete a workspace");
}

TEST(Rest, OpenApiCoversEveryCapability) {
    Surface s;
    const json doc = openapi_document(s.registry, "0.0.1");
    EXPECT_EQ(doc["openapi"], "3.1.0");
    for (const auto& capability : s.registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
        const std::string method = rest_method(*capability) == "GET" ? "get" : "post";
        ASSERT_TRUE(doc["paths"].contains(rest_path(*capability))) << capability->name;
        EXPECT_TRUE(doc["paths"][rest_path(*capability)].contains(method)) << capability->name;
        // What the call does, apart from how it is sent (UX review 2026-09-27: Ask Orama took
        // workspace_query, a POST, for a change and never read the workspace).
        EXPECT_EQ(doc["paths"][rest_path(*capability)][method].value("x-lake-action", ""),
                  capability->action)
            << capability->name;
    }
    EXPECT_EQ(doc["paths"]["/v1/workspace_query"]["post"]["x-lake-action"], "read");
    EXPECT_EQ(doc["paths"]["/v1/workspace_act"]["post"]["x-lake-action"], "write");
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/push"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/pull"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/find-missing"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/chunks"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/chunks/{digest}"));
    EXPECT_TRUE(doc["paths"].contains("/v1/attach_model_provenance"));
    EXPECT_TRUE(doc["paths"].contains("/v1/verify_model_provenance"));
    EXPECT_TRUE(doc["paths"].contains("/v1/resolve_conflict"));
    const json& conflict =
        doc["paths"]["/v1/list_conflicts"]["get"]["responses"]["200"]["content"]["application/json"]
           ["schema"]["properties"]["value"]["properties"]["items"]["items"];
    EXPECT_EQ(conflict["properties"]["kind"]["enum"],
              json::array({"text", "structural", "crdt-suspicious", "suspicious-merge"}));
    EXPECT_EQ(conflict["properties"]["region"]["properties"]["path"]["type"], "string");
    EXPECT_EQ(conflict["properties"]["explanation"]["type"], "string");
    EXPECT_TRUE(doc["paths"].contains("/v1/transparency_inclusion_proof"));
    EXPECT_TRUE(doc["paths"].contains("/v1/transparency_consistency_proof"));
    const json& inclusion_value =
        doc["paths"]["/v1/transparency_inclusion_proof"]["get"]["responses"]["200"]["content"]
           ["application/json"]["schema"]["properties"]["value"];
    EXPECT_EQ(inclusion_value["properties"]["audit_path"]["items"]["pattern"], "^[0-9a-f]{64}$");
    EXPECT_EQ(inclusion_value["properties"]["checkpoint"]["type"], "object");
    EXPECT_TRUE(doc["paths"].contains("/mcp"));
    EXPECT_TRUE(doc["paths"]["/v1/server_info"]["get"]["security"].empty());
    // Handler-owned authorization still requires a bearer before accepting an HTTP body.
    const json& gateway = doc["paths"]["/v1/model_completion"]["post"];
    EXPECT_FALSE(gateway.contains("security")); // inherits the document's bearer requirement
    EXPECT_TRUE(gateway["responses"].contains("401"));
    const json& push = doc["paths"]["/v1/repositories/{repository_id}/sync/push"]["post"];
    const json& request_header = push["requestBody"]["content"]["application/vnd.liquid.sync-v1"]
                                     ["schema"]["x-liquid-sync-header-schema"];
    EXPECT_EQ(request_header["properties"]["atomic"]["type"], "boolean");
    const json& outcome =
        push["responses"]["200"]["content"]["application/vnd.liquid.sync-v1"]["schema"]
            ["x-liquid-sync-header-schema"]["properties"]["outcomes"]["items"];
    EXPECT_EQ(outcome["properties"]["name"]["type"], "string");
    EXPECT_EQ(outcome["properties"]["outcome"]["enum"], json::array({"applied", "refused"}));
    EXPECT_EQ(outcome["properties"]["reason"]["type"], "string");
}

TEST(SyncRegistry, DeclaresTransportPolicyAndRecordsDenial) {
    Surface surface;
    const auto push = surface.registry.find("push");
    const auto pull = surface.registry.find("pull");
    const auto refs = surface.registry.find("refs");
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    ASSERT_NE(refs, nullptr);
    EXPECT_EQ(push->transport, CapabilityTransport::LiquidSync);
    EXPECT_EQ(push->action, "write");
    EXPECT_EQ(pull->transport, CapabilityTransport::LiquidSync);
    EXPECT_EQ(pull->action, "read");
    EXPECT_EQ(refs->transport, CapabilityTransport::Json);

    surface.ctx.authorized = false;
    const ApiResult denied = surface.call("push", {{"repository_id", "missing"}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthenticated");
    const auto events = lake::test::audit_events(surface.store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_FALSE(events->empty());
    EXPECT_EQ(events->back().draft.action, "push");
    EXPECT_EQ(events->back().draft.outcome, "denied");
}

TEST(SyncRegistry, RevokedOfflineGrantRefusesPublicationWithChangeAndReason) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    const auto issued = surface.store.sign_up_password({"lake",
                                                        "offline-writer@example.test",
                                                        "correct horse battery staple",
                                                        "Offline writer",
                                                        "offline device",
                                                        "{}"});
    ASSERT_TRUE(issued) << issued.error().message;
    const auto principal = surface.store.context_for(issued->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto customer = lake::test::personal_customer(surface.store, principal->user_id);
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        surface.store.create_repository({customer->id, "offline-sync", "", "private",
                                         lake::test::repository_workspace(
                                             surface.store, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto object = surface.store.find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(surface.store.create_grant({principal->taap_id,
                                            object->id,
                                            "write",
                                            "offline-sync-attestation",
                                            "admin",
                                            "offline-sync"}));
    surface.ctx.authorized = true;
    surface.ctx.operator_credential = false;
    surface.ctx.session_credential = true;
    surface.ctx.acting_user_id = principal->user_id;
    surface.ctx.acting_identity = principal->taap_id;
    surface.ctx.credential_device_id = principal->device_id;
    surface.ctx.scopes = principal->scopes;
    const std::string scope = "repository:" + repository->id + ":read";
    const ApiResult grant = surface.call("issue_offline_grant",
                                         {{"device_id", principal->device_id},
                                          {"scopes", json::array({scope})},
                                          {"duration_seconds", 3600}});
    ASSERT_TRUE(grant.ok) << grant.error.message;
    ASSERT_TRUE(surface.call("revoke_offline_grant", {{"id", grant.value["id"]}}).ok);

    surface.ctx.sync_store = &repository_store;
    surface.ctx.sync_wire_request = true;
    surface.ctx.offline_grant_id = grant.value["id"];
    surface.ctx.reauthorization_results = json::array();
    const ApiResult pushed = surface.call("push", {{"repository_id", repository->id}});
    ASSERT_FALSE(pushed.ok);
    EXPECT_EQ(pushed.error.code, "unauthorized");
    ASSERT_EQ(surface.ctx.reauthorization_results.size(), 1U);
    EXPECT_EQ(surface.ctx.reauthorization_results[0]["change"], "change-refused");
    EXPECT_FALSE(surface.ctx.reauthorization_results[0]["allowed"]);
    EXPECT_EQ(surface.ctx.reauthorization_results[0]["reason"],
              "offline grant was revoked before publication");
    const auto events = lake::test::audit_events(surface.store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push_change" && event.draft.outcome == "denied" &&
               event.draft.details.value("change", "") == "change-refused" &&
               event.draft.details.value("reason", "") ==
                   "offline grant was revoked before publication";
    }));
}

TEST(Mcp, InitializeListAndCall) {
    Surface s;
    const auto init = mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, s.ctx, "0.0.1");
    ASSERT_TRUE(init);
    EXPECT_EQ(json::parse(*init)["result"]["serverInfo"]["name"], "lake");
    EXPECT_EQ(json::parse(*init)["result"]["capabilities"]["tools"]["listChanged"], true);
    EXPECT_FALSE(mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, s.ctx, "0.0.1"));

    const auto tools = json::parse(*mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, s.ctx, "0.0.1"));
    const auto json_capabilities = std::ranges::count_if(s.registry.list(), [](const auto& entry) {
        return entry->transport == CapabilityTransport::Json;
    });
    EXPECT_EQ(tools["result"]["tools"].size(), json_capabilities);
    EXPECT_TRUE(std::ranges::any_of(tools["result"]["tools"], [](const json& tool) {
        return tool.value("name", "") == "attach_model_provenance";
    }));
    EXPECT_TRUE(std::ranges::any_of(tools["result"]["tools"], [](const json& tool) {
        return tool.value("name", "") == "verify_model_provenance";
    }));
    const auto inclusion_tool =
        std::ranges::find_if(tools["result"]["tools"], [](const json& tool) {
            return tool.value("name", "") == "transparency_inclusion_proof";
        });
    ASSERT_NE(inclusion_tool, tools["result"]["tools"].end());
    EXPECT_TRUE(inclusion_tool->at("outputSchema")
                    .at("properties")
                    .at("value")
                    .at("properties")
                    .contains("audit_path"));
    EXPECT_TRUE(std::ranges::any_of(tools["result"]["tools"], [](const json& tool) {
        return tool.value("name", "") == "transparency_consistency_proof";
    }));
    const auto resolve_conflict =
        std::ranges::find_if(tools["result"]["tools"], [](const json& tool) {
            return tool.value("name", "") == "resolve_conflict";
        });
    ASSERT_NE(resolve_conflict, tools["result"]["tools"].end());
    EXPECT_EQ(resolve_conflict->at("inputSchema").at("properties").at("resolution").at("enum"),
              json::array({"accept_proposal", "left", "right", "blob"}));

    const json call = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "create_user"}, {"arguments", {{"display_name", "Eve"}}}}}};
    const auto reply = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_FALSE(reply["result"]["isError"]);
    EXPECT_EQ(reply["result"]["structuredContent"]["value"]["display_name"], "Eve");

    s.ctx.authorized = false;
    const auto denied = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_TRUE(denied["result"]["isError"]);
    EXPECT_EQ(denied["result"]["structuredContent"]["error"]["code"], "unauthenticated");
    const auto unknown = json::parse(*mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "nope"}}, s.ctx, "0.0.1"));
    EXPECT_EQ(unknown["error"]["code"], -32601);
}

// lake#441: one registry, one schema. Every MCP tool's input and output schema is the schema
// OpenAPI gives its REST route, byte for byte, for each audience.
TEST(Mcp, ToolSchemasAreByteIdenticalToOpenApi) {
    Surface s;
    for (const SchemaAudience audience : {SchemaAudience::Caller, SchemaAudience::AgentTask}) {
        SCOPED_TRACE(audience == SchemaAudience::Caller ? "caller" : "agent_task");
        s.ctx.credential_kind = audience == SchemaAudience::AgentTask ? "agent_task" : "";
        const json openapi = openapi_document(s.registry, "0.0.1", audience);
        const json listed = json::parse(*mcp_dispatch(
            s.registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, s.ctx, "0.0.1"));
        const json& tools = listed.at("result").at("tools");
        ASSERT_EQ(tools, mcp_tools(s.registry, audience));
        ASSERT_FALSE(tools.empty());
        std::size_t workspace_relaxed = 0;
        for (const json& tool : tools) {
            const std::string name = tool.at("name");
            SCOPED_TRACE(name);
            const auto capability = s.registry.find(name);
            ASSERT_NE(capability, nullptr);
            const json& route = openapi.at("paths").at(rest_path(*capability));
            const std::string method = rest_method(*capability) == "GET" ? "get" : "post";
            const json& operation = route.at(method);
            EXPECT_EQ(tool.at("description"), operation.at("description"));
            EXPECT_EQ(tool.at("title"), operation.at("summary"));
            const json& input = tool.at("inputSchema");
            if (method == "post") {
                EXPECT_EQ(input.dump(),
                          operation.at("requestBody")
                              .at("content")
                              .at("application/json")
                              .at("schema")
                              .dump());
            } else {
                const json properties = input.value("properties", json::object());
                const json required = input.value("required", json::array());
                ASSERT_EQ(operation.at("parameters").size(), properties.size());
                for (const json& parameter : operation.at("parameters")) {
                    const std::string key = parameter.at("name");
                    ASSERT_TRUE(properties.contains(key)) << key;
                    EXPECT_EQ(parameter.at("schema").dump(), properties.at(key).dump()) << key;
                    EXPECT_EQ(parameter.at("required").get<bool>(),
                              std::ranges::find(required, json(key)) != required.end())
                        << key;
                }
                if (capability->post_alias) {
                    EXPECT_EQ(input.dump(),
                              route.at("post")
                                  .at("requestBody")
                                  .at("content")
                                  .at("application/json")
                                  .at("schema")
                                  .dump());
                }
            }
            const json& success =
                operation.at("responses").at("200").at("content").at("application/json").at(
                    "schema");
            if (tool.contains("outputSchema")) {
                EXPECT_EQ(tool.at("outputSchema").dump(), success.dump());
            } else {
                EXPECT_TRUE(capability->output_schema.empty());
            }
            const json declared = capability->input_schema.value("required", json::array());
            if (std::ranges::find(declared, "workspace_id") != declared.end()) {
                const json shown = input.value("required", json::array());
                const bool listed_required =
                    std::ranges::find(shown, "workspace_id") != shown.end();
                EXPECT_EQ(listed_required, audience == SchemaAudience::Caller);
                workspace_relaxed += listed_required ? 0 : 1;
            }
        }
        // The task's workspace fills workspace_id, so a task credential is shown it as optional.
        if (audience == SchemaAudience::AgentTask) {
            EXPECT_GT(workspace_relaxed, 0U);
        }
    }
}

// lake#441: the session identifier carries the tool list, so an upgraded lake knows the client
// holds an older one.
TEST(Mcp, SessionsNameTheToolListTheyBeganOn) {
    Surface s;
    const std::string digest = mcp_tool_list_digest(s.registry);
    ASSERT_EQ(digest.size(), 64U);
    EXPECT_EQ(digest, mcp_tool_list_digest(s.registry));
    const std::string session = mcp_session_id(digest);
    EXPECT_TRUE(session.starts_with(digest + "."));
    EXPECT_NE(session, mcp_session_id(digest));
    EXPECT_FALSE(mcp_session_tool_list_changed(session, digest));
    EXPECT_FALSE(mcp_session_tool_list_changed("", digest));
    EXPECT_TRUE(mcp_session_tool_list_changed(digest, digest));
    EXPECT_TRUE(mcp_session_tool_list_changed("not-a-lake-session", digest));

    Registry upgraded;
    register_core_capabilities(upgraded, "0.0.1");
    EXPECT_EQ(mcp_tool_list_digest(upgraded), digest);
    upgraded.add(Capability{.name = "added_after_upgrade",
                            .title = "Added after an upgrade",
                            .description = "A tool the client has not listed.",
                            .input_schema = {{"type", "object"},
                                             {"properties", json::object()},
                                             {"additionalProperties", false}},
                            .output_schema = {{"type", "object"}},
                            .requires_auth = false,
                            .handler = [](const json&, ApiContext&) {
                                return ApiResult::success(json::object());
                            }});
    const std::string upgraded_digest = mcp_tool_list_digest(upgraded);
    EXPECT_NE(upgraded_digest, digest);
    EXPECT_TRUE(mcp_session_tool_list_changed(session, upgraded_digest));
    EXPECT_EQ(mcp_tool_list_changed_notification(),
              json({{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}));
}

// lake#684: a workspace endpoint's digest is Lake's own until a connection re-exports tools, and
// follows what each re-exports; a session that listed the current tools is not told again.
TEST(Mcp, WorkspaceDigestFollowsExportedToolsAndListedSessionsAreNotToldAgain) {
    const std::string lake(64, '1');
    EXPECT_EQ(mcp_workspace_tool_list_digest(lake, {}), lake);
    EXPECT_EQ(mcp_workspace_tool_list_digest(lake, {{"c-1", ""}}), lake) << "an empty digest";
    const std::string one = mcp_workspace_tool_list_digest(lake, {{"c-1", std::string(64, 'a')}});
    ASSERT_EQ(one.size(), 64U);
    EXPECT_NE(one, lake);
    EXPECT_NE(one, mcp_workspace_tool_list_digest(lake, {{"c-1", std::string(64, 'b')}}));
    EXPECT_NE(one, mcp_workspace_tool_list_digest(std::string(64, '2'),
                                                  {{"c-1", std::string(64, 'a')}}))
        << "an upgrade still changes it";
    EXPECT_EQ(mcp_workspace_tool_list_digest(
                  lake, {{"c-2", std::string(64, 'b')}, {"c-1", std::string(64, 'a')}}),
              mcp_workspace_tool_list_digest(
                  lake, {{"c-1", std::string(64, 'a')}, {"c-2", std::string(64, 'b')}}))
        << "in connection order, however the rows came";

    McpSessionLists lists(2);
    const std::string session = mcp_session_id(lake);
    EXPECT_FALSE(lists.changed(session, lake)) << "judged by the list it began on";
    EXPECT_TRUE(lists.changed(session, one));
    lists.listed(session, one);
    EXPECT_FALSE(lists.changed(session, one));
    EXPECT_TRUE(lists.changed(session, lake));
    lists.listed("", one);
    lists.listed(mcp_session_id(lake), one);
    lists.listed(mcp_session_id(lake), one);
    EXPECT_TRUE(lists.changed(session, one)) << "forgotten past capacity: its first list again";
}

// lake#441: the published bounds are enforced, so REST cannot accept what MCP's schema refuses.
TEST(Registry, EnforcesEnumsAndNumericBoundsOnEveryTransport) {
    Registry registry;
    registry.add(Capability{
        .name = "bounded",
        .title = "Bounded",
        .description = "Takes a bounded fraction and a mode.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"fraction", {{"type", "number"}, {"minimum", 0}, {"maximum", 0.9}}},
                           {"above", {{"type", "integer"}, {"exclusiveMinimum", 0}}},
                           {"below", {{"type", "number"}, {"exclusiveMaximum", 1}}},
                           {"mode", {{"type", "string"}, {"enum", {"left", "right"}}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .handler = [](const json&, ApiContext&) { return ApiResult::success(json::object()); }});
    ApiContext context;
    const auto refused = [&](const json& input, const std::string& path) {
        const ApiResult result = registry.call("bounded", input, context);
        EXPECT_FALSE(result.ok) << input;
        EXPECT_EQ(result.error.code, "invalid_input") << input;
        EXPECT_EQ(result.error.path, path) << input;
    };
    EXPECT_TRUE(registry.call("bounded", {{"fraction", 0.9}, {"mode", "left"}}, context).ok);
    EXPECT_TRUE(registry.call("bounded", {{"fraction", 0}, {"above", 1}, {"below", 0.99}}, context)
                    .ok);
    refused({{"fraction", 1.0}}, "/fraction");
    refused({{"fraction", -0.1}}, "/fraction");
    refused({{"above", 0}}, "/above");
    refused({{"below", 1}}, "/below");
    refused({{"mode", "middle"}}, "/mode");

    const json tools = json::parse(*mcp_dispatch(
        registry,
        {{"jsonrpc", "2.0"},
         {"id", 1},
         {"method", "tools/call"},
         {"params", {{"name", "bounded"}, {"arguments", {{"fraction", 1.0}}}}}},
        context,
        "0.0.1"));
    EXPECT_TRUE(tools["result"]["isError"]);
    EXPECT_EQ(tools["result"]["structuredContent"]["error"]["path"], "/fraction");
}

TEST(Repositories, OwnedByACustomerNamedWithinIt) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const std::string cid = customer.value["id"];
    // The description is the repository's record in its workspace (lake#315), so it is Ready.
    const std::string workspace = s.workspace(cid);

    ApiResult repo = s.call("create_repository",
                            {{"customer_id", cid},
                             {"workspace_id", workspace},
                             {"name", "liquid"},
                             {"description", "the tool"}});
    ASSERT_TRUE(repo.ok) << repo.error.message;
    EXPECT_EQ(repo.value["description"], "the tool");
    EXPECT_EQ(s.call("get_repository", {{"id", repo.value["id"]}}).value["description"],
              "the tool");
    EXPECT_EQ(repo.value["visibility"], "private");
    EXPECT_EQ(repo.value["customer_id"], cid);
    EXPECT_EQ(repo.value["workspace_id"], workspace);

    const ApiResult duplicate = s.call(
        "create_repository",
        {{"customer_id", cid}, {"workspace_id", workspace}, {"name", "liquid"}});
    EXPECT_EQ(duplicate.error.code, "conflict");
    EXPECT_EQ(duplicate.error.message, "a repository named 'liquid' already exists");
    EXPECT_EQ(duplicate.error.path, "/name");
    EXPECT_EQ(s.call("create_repository",
                     {{"customer_id", cid}, {"workspace_id", workspace}, {"name", "Bad Name"}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(s.call("create_repository",
                     {{"customer_id", "nope"}, {"workspace_id", workspace}, {"name", "x"}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(s.call("create_repository",
                     {{"customer_id", cid},
                      {"workspace_id", workspace},
                      {"name", "y"},
                      {"visibility", "secret"}})
                  .error.code,
              "invalid_input");
    ASSERT_TRUE(s.call("create_repository",
                       {{"customer_id", cid},
                        {"workspace_id", workspace},
                        {"name", "lake"},
                        {"visibility", "public"}})
                    .ok);

    const ApiResult all = s.call("list_repositories");
    ASSERT_TRUE(all.ok);
    EXPECT_EQ(all.value["items"].size(), 2u);
    EXPECT_EQ(all.value["items"][0]["name"], "lake"); // newest first
    const ApiResult mine = s.call("list_repositories", {{"customer_id", cid}, {"limit", 1}});
    EXPECT_EQ(mine.value["items"].size(), 1u);
    const ApiResult none = s.call("list_repositories", {{"customer_id", "other"}});
    EXPECT_EQ(none.value["items"].size(), 0u);
    EXPECT_EQ(s.call("get_repository", {{"id", repo.value["id"]}}).value["name"], "liquid");
    EXPECT_EQ(s.call("get_repository", {{"id", "missing"}}).error.code, "not_found");

    const ApiResult deleted = s.call("delete_repository", {{"id", repo.value["id"]}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    const ApiResult recreated = s.call(
        "create_repository",
        {{"customer_id", cid}, {"workspace_id", workspace}, {"name", "liquid"}});
    ASSERT_TRUE(recreated.ok) << recreated.error.message;
    EXPECT_EQ(recreated.value["name"], "liquid");
    EXPECT_NE(recreated.value["id"], repo.value["id"]);
}

// lake#315: every repository belongs to one workspace of its space, named on create and never
// changed. Two spaces, two workspaces in one space, and every way to name the wrong one.
TEST(Repositories, BelongToOneWorkspaceOfTheirSpace) {
    Surface s;
    RepositoryFixtureStore repository_store;
    s.ctx.sync_store = &repository_store;
    const auto space = [&](const std::string& name) {
        const ApiResult user = s.call("create_user", {{"display_name", name}});
        const ApiResult customer = s.call(
            "create_customer",
            {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", name}});
        return customer.value["id"].get<std::string>();
    };
    const std::string ada = space("ada");
    const std::string grace = space("grace");
    const auto research = s.store.create_workspace({ada, "research", "dev", ""});
    const auto product = s.store.create_workspace({ada, "product", "prod", ""});
    const auto foreign = s.store.create_workspace({grace, "elsewhere", "dev", ""});
    const auto withdrawn = s.store.create_workspace({ada, "withdrawn", "dev", ""});
    ASSERT_TRUE(research && product && foreign && withdrawn);
    ASSERT_TRUE(s.store.update_workspace(withdrawn->id, {.desired = "deleted"}));
    // Their Zettas hold the repositories' forge records (lake#341).
    s.ready(research->id);
    s.ready(foreign->id);

    const auto create = [&](const json& workspace, const std::string& name) {
        json input{{"customer_id", ada}, {"name", name}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return s.call("create_repository", input);
    };
    for (const json& wrong :
         {json(), json(""), json("not-a-workspace"), json(withdrawn->id)}) {
        const ApiResult refused = create(wrong, "refused");
        EXPECT_EQ(refused.error.code, "invalid_input") << wrong;
        EXPECT_EQ(refused.error.path, "/workspace_id") << wrong;
    }
    // Another space's workspace: the registry names the space beside it as the one at fault, as
    // for every workspace-scoped capability (lake#450).
    const ApiResult foreign_workspace = create(foreign->id, "refused");
    EXPECT_EQ(foreign_workspace.error.code, "invalid_input");
    EXPECT_EQ(foreign_workspace.error.path, "/customer_id");

    const ApiResult in_research = create(research->id, "notes");
    ASSERT_TRUE(in_research.ok) << in_research.error.message;
    EXPECT_EQ(in_research.value["workspace_id"], research->id);
    const ApiResult in_product = create(product->id, "service");
    ASSERT_TRUE(in_product.ok) << in_product.error.message;
    // Names stay unique per space, across its workspaces.
    EXPECT_EQ(create(product->id, "notes").error.code, "conflict");

    // Reads return the owning workspace, and a list can be narrowed to one workspace.
    EXPECT_EQ(s.call("get_repository", {{"id", in_product.value["id"]}}).value["workspace_id"],
              product->id);
    const ApiResult research_only =
        s.call("list_repositories", {{"customer_id", ada}, {"workspace_id", research->id}});
    ASSERT_EQ(research_only.value["items"].size(), 1u);
    EXPECT_EQ(research_only.value["items"][0]["name"], "notes");
    EXPECT_EQ(s.call("list_repositories", {{"customer_id", ada}}).value["items"].size(), 2u);
    EXPECT_EQ(s.call("list_repositories", {{"workspace_id", foreign->id}}).value["items"].size(),
              0u);

    // The space and the workspace never change: a transfer is refused.
    const ApiResult transfer = s.call("transfer_repository",
                                      {{"id", in_research.value["id"]}, {"customer_id", grace}});
    EXPECT_EQ(transfer.error.code, "conflict");
    EXPECT_EQ(transfer.error.path, "/customer_id");
    EXPECT_EQ(s.call("get_repository", {{"id", in_research.value["id"]}}).value["customer_id"],
              ada);

    // The export import names its own target workspace; the archive's is not reused.
    const ApiResult exported =
        s.call("export_repository", {{"repository_id", in_research.value["id"]}});
    ASSERT_TRUE(exported.ok) << exported.error.message;
    const ApiResult no_workspace = s.call(
        "import_repository",
        {{"customer_id", grace}, {"name", "copy"}, {"archive", exported.value["archive"]}});
    EXPECT_EQ(no_workspace.error.code, "invalid_input");
    EXPECT_EQ(no_workspace.error.path, "/workspace_id");
    const ApiResult archive_workspace = s.call("import_repository",
                                               {{"customer_id", grace},
                                                {"workspace_id", research->id},
                                                {"name", "copy"},
                                                {"archive", exported.value["archive"]}});
    EXPECT_EQ(archive_workspace.error.code, "invalid_input");
    const ApiResult imported = s.call("import_repository",
                                      {{"customer_id", grace},
                                       {"workspace_id", foreign->id},
                                       {"name", "copy"},
                                       {"archive", exported.value["archive"]}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    EXPECT_EQ(imported.value["workspace_id"], foreign->id);
    const ApiResult github = s.call("import_github_repository",
                                    {{"customer_id", ada},
                                     {"workspace_id", foreign->id},
                                     {"name", "example"},
                                     {"git_directory", "example.git"},
                                     {"recording", json::object()}});
    EXPECT_EQ(github.error.code, "invalid_input");
    EXPECT_EQ(github.error.path, "/customer_id");

    // Deleting a workspace deletes its repositories with it (lake#514): the
    // live one is tombstoned with its retention ended, and the workspace takes no new one.
    const ApiResult removed = s.call(
        "delete_workspace",
        {{"customer_id", ada}, {"workspace_id", product->id}, {"confirm", "product"}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_EQ(removed.value["deletion"]["counts"]["live_repositories"], 1);
    EXPECT_FALSE(s.store.get_repository(in_product.value["id"])->deleted_at.empty());
    EXPECT_EQ(create(product->id, "after").error.path, "/workspace_id");
    EXPECT_TRUE(create(research->id, "service").ok) << "the deleted repository's name is free";
}

TEST(ApiErrors, DatabaseDiagnosticsStayOutOfErrorMessages) {
    testing::internal::CaptureStderr();
    const ApiResult result = ApiResult::failure(
        "conflict",
        "ERROR: duplicate key value violates unique constraint\nDETAIL: Key already exists");
    const std::string log = testing::internal::GetCapturedStderr();

    EXPECT_EQ(result.error.message, "the requested change conflicts with existing data");
    EXPECT_EQ(result.error.message.find("ERROR:"), std::string::npos);
    EXPECT_EQ(result.error.message.find("DETAIL:"), std::string::npos);
    EXPECT_NE(log.find("duplicate key value"), std::string::npos);
}

TEST(Forge, PersistsIssueLifecycleAndMapsReviewCommentAnchors) {
    Surface surface;
    const ApiResult user =
        surface.call("create_user", {{"display_name", "Ada"}, {"taap_id", "taap:1:ed25519:ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", surface.workspace(customer.value["id"])},
         {"name", "forge"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];

    const ApiResult issue = surface.call(
        "create_issue",
        {{"repository_id", repository_id},
         {"title", "Document anchors"},
         {"body", "Discuss `src/lib.cpp`."},
         {"labels", json::array({"docs"})},
         {"assignees", json::array({"taap:1:ed25519:ada"})},
         {"references", json::array({{"change", "change-1"}, {"path", "src/lib.cpp"}})}});
    ASSERT_TRUE(issue.ok) << issue.error.message;
    EXPECT_EQ(issue.value["value"]["state"], "open");
    EXPECT_TRUE(surface
                    .call("update_issue",
                          {{"repository_id", repository_id},
                           {"issue_id", issue.value["id"]},
                           {"state", "closed"}})
                    .ok);
    EXPECT_TRUE(surface
                    .call("create_issue_comment",
                          {{"repository_id", repository_id},
                           {"issue_id", issue.value["id"]},
                           {"body", "Resolved by change-2."}})
                    .ok);

    const ApiResult review = surface.call("create_review",
                                          {{"repository_id", repository_id},
                                           {"change_id", "change-1"},
                                           {"reviewers", json::array({"taap:1:ed25519:ada"})}});
    ASSERT_TRUE(review.ok) << review.error.message;
    const ApiResult comment = surface.call(
        "create_review_comment",
        {{"repository_id", repository_id},
         {"review_id", review.value["id"]},
         {"body", "Use a more specific name."},
         {"anchor",
          {{"change", "change-1"}, {"path", "src/lib.cpp"}, {"line", 8}, {"column", 4}}}});
    ASSERT_TRUE(comment.ok) << comment.error.message;

    const ApiResult mapped = surface.call(
        "map_review_anchors",
        {{"repository_id", repository_id},
         {"source_change", "change-1"},
         {"target_change", "change-2"},
         {"path", "src/lib.cpp"},
         {"hunks",
          json::array({{{"old_line", 1}, {"old_count", 0}, {"new_line", 1}, {"new_count", 2}}})}});
    ASSERT_TRUE(mapped.ok) << mapped.error.message;
    ASSERT_EQ(mapped.value["items"].size(), 1);
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["change"], "change-2");
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["line"], 10);
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["disposition"], "shifted");

    // Reviewers and comment authors read by name and handle (lake#1326); an unknown one is absent.
    const json ada = {{"kind", "user"}, {"name", "Ada"}, {"handle", "ada"}};
    const ApiResult reviews = surface.call("list_reviews", {{"repository_id", repository_id}});
    ASSERT_TRUE(reviews.ok) << reviews.error.message;
    EXPECT_EQ(reviews.value["people"]["taap:1:ed25519:ada"], ada);
    const ApiResult comments =
        surface.call("list_review_comments", {{"repository_id", repository_id}});
    ASSERT_TRUE(comments.ok) << comments.error.message;
    EXPECT_TRUE(comments.value.contains("people"));
    // Issues are unchanged.
    EXPECT_FALSE(
        surface.call("list_issues", {{"repository_id", repository_id}}).value.contains("people"));
}

// lake#1326: the identities a forge record names, in either author shape.
TEST(Forge, ARecordsSignersAreItsOpenerReviewersApproverAndAuthor) {
    std::vector<std::string> signers;
    lake::api::forge_record_signers({{"opened_by", "taap:1:ed25519:opener"},
                                     {"reviewers", json::array({"taap:1:ed25519:r", 7, ""})},
                                     {"approval", {{"by", "taap:1:ed25519:approver"}}}},
                                    signers);
    lake::api::forge_record_signers(
        {{"author", {{"taap_id", "taap:1:ed25519:actor"}, {"algorithm", "ed25519"}}}}, signers);
    lake::api::forge_record_signers({{"author", "taap:1:ed25519:plain"}}, signers);
    lake::api::forge_record_signers(json::array(), signers);
    EXPECT_EQ(signers,
              (std::vector<std::string>{"taap:1:ed25519:opener",
                                        "taap:1:ed25519:r",
                                        "taap:1:ed25519:approver",
                                        "taap:1:ed25519:actor",
                                        "taap:1:ed25519:plain"}));
}

// lake#1326: an identity no account acts for is absent from `people`, and a repository's access
// list names its principals.
TEST(Repositories, PeopleNameKnownAccountsOnlyAndTheAccessList) {
    Surface s;
    EXPECT_EQ(lake::api::signer_people(s.ctx, {"taap:1:ed25519:nobody"}), json::object());
    const ApiResult user = s.call("create_user",
                                  {{"display_name", "Grace"},
                                   {"taap_id", "taap:1:ed25519:grace"},
                                   {"email", "grace@example.test"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    EXPECT_EQ(lake::api::signer_people(s.ctx, {"taap:1:ed25519:grace", "taap:1:ed25519:nobody"}),
              json({{"taap:1:ed25519:grace",
                     {{"kind", "user"}, {"name", "Grace"}, {"handle", "grace"}}}}));
    const ApiResult repository =
        s.call("create_repository",
               {{"customer_id", customer.value["id"]},
                {"workspace_id", lake::test::repository_workspace(s.store, customer.value["id"])},
                {"name", "access"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const auto object = s.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(s.store.create_grant({"taap:1:ed25519:grace",
                                      object->id,
                                      "write",
                                      "people-attestation",
                                      "admin",
                                      "people-access"}));
    const ApiResult access = s.call(
        "entity_effective_access", {{"object_type", "repository"}, {"id", repository.value["id"]}});
    ASSERT_TRUE(access.ok) << access.error.message;
    ASSERT_TRUE(access.value.contains("people"));
    EXPECT_TRUE(std::ranges::any_of(access.value["items"], [](const json& entry) {
        return entry.value("principal_taap_id", "") == "taap:1:ed25519:grace";
    })) << access.value.dump();
    EXPECT_EQ(access.value["people"]["taap:1:ed25519:grace"],
              json({{"kind", "user"}, {"name", "Grace"}, {"handle", "grace"}}));
    EXPECT_EQ(access.value.dump().find("grace@example.test"), std::string::npos);
}

TEST(Export, ManifestSignerMustBeNamedAfterItsSigningKey) {
    lake::control::InMemoryStore source;
    lake::control::InMemoryStore attacker;
    const lake::audit::IdentityOptions options{.instance_kek = "export-binding-test"};
    const json manifest = {{"format", "lake_export_manifest/1"}, {"entries", json::array()}};
    const auto honest = lake::audit::sign_export_manifest(source, manifest, options);
    const auto resigned = lake::audit::sign_export_manifest(attacker, manifest, options);
    ASSERT_TRUE(honest);
    ASSERT_TRUE(resigned);
    ASSERT_NE((*honest)["identity"]["taap_id"], (*resigned)["identity"]["taap_id"]);
    ASSERT_TRUE(lake::audit::verify_export_manifest(manifest, *honest));
    ASSERT_TRUE(lake::audit::verify_export_manifest(manifest, *resigned));

    json forged = *resigned;
    forged["identity"]["taap_id"] = (*honest)["identity"]["taap_id"];
    const auto refused = lake::audit::verify_export_manifest(manifest, forged);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "invalid_signature");

    for (const json& bad_name : {json(""),
                                 json("taap:1:ed25519:unknown"),
                                 json((*honest)["identity"]["taap_id"].get<std::string>() + "="),
                                 json(nullptr),
                                 json(17)}) {
        json malformed = *honest;
        malformed["identity"]["taap_id"] = bad_name;
        EXPECT_FALSE(lake::audit::verify_export_manifest(manifest, malformed));
    }
    json malformed = *honest;
    malformed["identity"]["version"] = 1;
    EXPECT_FALSE(lake::audit::verify_export_manifest(manifest, malformed));
    malformed = *honest;
    malformed["identity"]["public_key"] = "invalid-base64";
    EXPECT_FALSE(lake::audit::verify_export_manifest(manifest, malformed));
    malformed = *resigned;
    malformed["signature"] = (*honest)["signature"];
    EXPECT_FALSE(lake::audit::verify_export_manifest(manifest, malformed));

    // Identity rotation mints another key-derived name, which must still verify portably.
    ASSERT_TRUE(lake::audit::rotate_identity(source, options));
    const auto rotated = lake::audit::sign_export_manifest(source, manifest, options);
    ASSERT_TRUE(rotated);
    EXPECT_NE((*rotated)["identity"]["taap_id"], (*honest)["identity"]["taap_id"]);
    EXPECT_TRUE(lake::audit::verify_export_manifest(manifest, *rotated));
}

TEST(Export, VerifiesAndRecreatesPortableRepositoryFacts) {
    Surface source;
    RepositoryFixtureStore source_store;
    source.ctx.sync_store = &source_store;
    const ApiResult user = source.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = source.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = source.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", source.workspace(customer.value["id"])},
         {"name", "portable"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    ASSERT_TRUE(source
                    .call("create_issue",
                          {{"repository_id", repository.value["id"]}, {"title", "Portable"}})
                    .ok);
    const auto object = source.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(
        source.store
            .create_grant(
                {"taap:1:ed25519:reader", object->id, "read", "grant-proof", "admin", "test"})
            .has_value());
    // An approved review travels as history, without its approval.
    const ApiResult review = source.call(
        "create_review", {{"repository_id", repository.value["id"]}, {"change_id", "change-1"}});
    ASSERT_TRUE(review.ok) << review.error.message;
    ASSERT_TRUE(source.store.create_grant(
        {"taap:1:ed25519:reviewer", object->id, "admin", "reviewer-proof", "admin", "test"}));
    source.ctx.acting_identity = "taap:1:ed25519:reviewer";
    source.ctx.operator_credential = false;
    ASSERT_TRUE(source
                    .call("update_review",
                          {{"repository_id", repository.value["id"]},
                           {"review_id", review.value["id"]},
                           {"state", "approved"}})
                    .ok);
    source.ctx.acting_identity = "admin";
    source.ctx.operator_credential = true;
    ASSERT_TRUE(source
                    .call("create_issue_comment",
                          {{"repository_id", repository.value["id"]},
                           {"issue_id",
                            source.call("list_issues", {{"repository_id", repository.value["id"]}})
                                .value["items"][0]["id"]},
                           {"body", "An imported comment"}})
                    .ok);
    ASSERT_TRUE(source
                    .call("create_release",
                          {{"repository_id", repository.value["id"]},
                           {"tag", "v1.0.0"},
                           {"change_id", "change-1"},
                           {"artifact_ids", json::array({"result-1"})}})
                    .ok);
    ASSERT_TRUE(source
                    .call("set_branch_protection",
                          {{"repository_id", repository.value["id"]},
                           {"ref", "refs/heads/main"},
                           {"required_verification", true}})
                    .ok);

    ApiResult exported = source.call(
        "export_repository", {{"repository_id", repository.value["id"]}, {"include_git", true}});
    ASSERT_TRUE(exported.ok) << exported.error.message;
    EXPECT_EQ(exported.value["archive"]["format"], "lake_export/1");
    const auto resign_archive = [&](json archive) {
        for (json& entry : archive["manifest"]["entries"]) {
            entry["sha256"] = lake::control::identity_crypto::sha256_hex(
                archive["payload"][entry.at("name").get<std::string>()].dump());
        }
        const auto signature = lake::audit::sign_export_manifest(
            source.store, archive["manifest"], {.instance_kek = source.ctx.instance_kek});
        EXPECT_TRUE(signature);
        if (signature) {
            archive["signature"] = *signature;
        }
        return archive;
    };
    // A valid source signature does not authenticate supplied metadata trust claims.
    exported.value["archive"]["payload"]["forge"][0]["value"]["import_provenance"] = {
        {"trust", "trusted"}};
    exported.value["archive"]["payload"]["artifacts"][0]["import_provenance"] = {
        {"trust", "trusted"}};
    for (json& record : exported.value["archive"]["payload"]["forge"]) {
        if (record["kind"] == "issue" || record["kind"] == "issue_comment") {
            record["value"]["author"] = "taap:1:ed25519:claimed-author";
        } else if (record["kind"] == "review") {
            record["value"]["opened_by"] = "taap:1:ed25519:claimed-opener";
        }
    }
    exported.value["archive"] = resign_archive(exported.value["archive"]);

    Surface destination;
    RepositoryFixtureStore destination_store;
    destination.ctx.sync_store = &destination_store;
    const ApiResult target_user = destination.call("create_user", {{"display_name", "Grace"}});
    ASSERT_TRUE(target_user.ok) << target_user.error.message;
    const ApiResult target_customer = destination.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", target_user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(target_customer.ok) << target_customer.error.message;
    const auto customer_object =
        destination.store.find_policy_object("customer", target_customer.value["id"]);
    ASSERT_TRUE(customer_object);
    ASSERT_TRUE(destination.store.create_grant({"taap:1:ed25519:importer",
                                                customer_object->id,
                                                "admin",
                                                "importer-permission",
                                                "admin",
                                                "test"}));
    destination.ctx.acting_identity = "taap:1:ed25519:importer";
    destination.ctx.operator_credential = false;
    json forged = exported.value["archive"];
    lake::control::InMemoryStore attacker;
    const auto resigned = lake::audit::sign_export_manifest(
        attacker, forged["manifest"], {.instance_kek = "export-attacker-test"});
    ASSERT_TRUE(resigned);
    forged["signature"] = *resigned;
    forged["signature"]["identity"]["taap_id"] =
        exported.value["archive"]["signature"]["identity"]["taap_id"];
    const ApiResult refused =
        destination.call("import_repository",
                         {{"customer_id", target_customer.value["id"]},
                          {"workspace_id", destination.workspace(target_customer.value["id"])},
                          {"name", "forged-copy"},
                          {"archive", forged}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_signature");
    EXPECT_TRUE(destination_store.imported.empty());
    const auto absent = destination.store.list_repositories(target_customer.value["id"], 100);
    ASSERT_TRUE(absent);
    EXPECT_TRUE(absent->empty());
    for (const std::string member : {"artifacts", "forge", "grants"}) {
        json malformed = exported.value["archive"];
        malformed["payload"][member] = json::array({nullptr});
        malformed = resign_archive(std::move(malformed));
        const auto rejected =
            destination.call("import_repository",
                             {{"customer_id", target_customer.value["id"]},
                              {"workspace_id", destination.workspace(target_customer.value["id"])},
                              {"name", "malformed-" + member},
                              {"archive", malformed}});
        EXPECT_FALSE(rejected.ok);
        EXPECT_EQ(rejected.error.code, "invalid_input");
        EXPECT_TRUE(destination_store.imported.empty());
    }
    const ApiResult imported = destination.call(
        "import_repository",
        {{"customer_id", target_customer.value["id"]},
         {"workspace_id",
          destination.workspace(target_customer.value["id"])},
         {"name", "portable-copy"},
         {"archive", exported.value["archive"]}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    const json import_audit = destination.ctx.audit_details;
    const auto portable =
        destination_store.imported.find(target_customer.value["id"].get<std::string>() + "/" +
                                        imported.value["id"].get<std::string>());
    ASSERT_NE(portable, destination_store.imported.end());
    EXPECT_EQ(portable->second.history, exported.value["archive"]["payload"]["history"]);
    ASSERT_EQ(portable->second.artifacts.size(),
              exported.value["archive"]["payload"]["artifacts"].size());
    EXPECT_EQ(portable->second.artifacts[0]["id"], "result-1");
    EXPECT_EQ(portable->second.artifacts[0]["action"], "action-1");
    EXPECT_EQ(portable->second.artifacts[0]["verification"], "untrusted");
    EXPECT_EQ(portable->second.artifacts[0]["attestations"][0]["verified"], false);
    EXPECT_EQ(portable->second.artifacts[0]["import_provenance"]["trust"], "untrusted");
    const auto issues = destination.call("list_issues", {{"repository_id", imported.value["id"]}});
    ASSERT_TRUE(issues.ok);
    ASSERT_EQ(issues.value["items"].size(), 1);
    EXPECT_EQ(issues.value["items"][0]["value"]["author"], destination.ctx.acting_identity);
    EXPECT_EQ(issues.value["items"][0]["value"]["import_provenance"]["source_author"],
              "taap:1:ed25519:claimed-author");
    EXPECT_EQ(issues.value["items"][0]["value"]["import_provenance"]["trust"], "untrusted");
    const ApiResult reviews =
        destination.call("list_reviews", {{"repository_id", imported.value["id"]}});
    ASSERT_EQ(reviews.value["items"].size(), 1);
    EXPECT_EQ(reviews.value["items"][0]["value"]["state"], "approved");
    EXPECT_FALSE(reviews.value["items"][0]["value"].contains("approval"));
    EXPECT_EQ(reviews.value["items"][0]["value"]["import_provenance"]["trust"], "untrusted");
    EXPECT_EQ(reviews.value["items"][0]["value"]["opened_by"], destination.ctx.acting_identity);
    EXPECT_EQ(reviews.value["items"][0]["value"]["import_provenance"]["source_opened_by"],
              "taap:1:ed25519:claimed-opener");
    const auto comments =
        destination.call("list_issue_comments", {{"repository_id", imported.value["id"]}});
    ASSERT_TRUE(comments.ok) << comments.error.message;
    ASSERT_EQ(comments.value["items"].size(), 1);
    EXPECT_EQ(comments.value["items"][0]["value"]["import_provenance"]["trust"], "untrusted");
    EXPECT_EQ(comments.value["items"][0]["value"]["import_provenance"]["imported_by"],
              destination.ctx.acting_identity);
    EXPECT_EQ(comments.value["items"][0]["value"]["author"], destination.ctx.acting_identity);
    EXPECT_EQ(comments.value["items"][0]["value"]["import_provenance"]["source_author"],
              "taap:1:ed25519:claimed-author");
    const auto releases =
        destination.call("list_releases", {{"repository_id", imported.value["id"]}});
    ASSERT_TRUE(releases.ok);
    ASSERT_EQ(releases.value["items"].size(), 1);
    EXPECT_TRUE(releases.value["items"][0]["value"]["artifacts"].empty());
    EXPECT_EQ(releases.value["items"][0]["value"]["import_provenance"]["artifacts"][0]["id"],
              "result-1");
    const auto target_object =
        destination.store.find_policy_object("repository", imported.value["id"]);
    ASSERT_TRUE(target_object);
    const auto grants = destination.store.list_grants({target_object->id}, std::nullopt, 100);
    ASSERT_TRUE(grants);
    ASSERT_EQ(grants->size(), 2);
    for (const auto& grant : *grants) {
        EXPECT_EQ(grant.granted_by_taap_id, destination.ctx.acting_identity);
        EXPECT_FALSE(grant.attestation_digest.empty());
        EXPECT_NE(grant.attestation_digest, "grant-proof");
        EXPECT_NE(grant.attestation_digest, "reviewer-proof");
    }
    EXPECT_EQ(import_audit["source_signer"],
              exported.value["archive"]["signature"]["identity"]["taap_id"]);
    EXPECT_EQ(import_audit["source_trust"], "untrusted");
    ASSERT_EQ(import_audit["grant_attestations"].size(), 2);
    for (const auto& attestation : import_audit["grant_attestations"]) {
        EXPECT_EQ(attestation["statement"]["granted_by"], destination.ctx.acting_identity);
        EXPECT_EQ(attestation["statement"]["object_id"], target_object->id);
        EXPECT_TRUE(std::ranges::any_of(*grants, [&](const auto& grant) {
            return grant.attestation_digest == attestation["digest"].get<std::string>();
        }));
    }
    const auto stored = destination.store.get_repository(imported.value["id"]);
    ASSERT_TRUE(stored);
    const auto protection =
        liquid::runtime::block_on(load_branch_protection(destination.ctx, *stored));
    ASSERT_TRUE(protection && *protection);
    const auto publication = evaluate_branch_protection(
        destination.store,
        destination.ctx.acting_identity,
        destination.ctx.authorization_cache,
        **protection,
        {.name = "refs/heads/main", .new_head = "change-1", .fast_forward = true});
    ASSERT_FALSE(publication) << "imported release claims must not authorize publication";
    EXPECT_EQ(publication.error().code, "conflict");

    json altered = exported.value["archive"];
    altered["payload"]["history"]["changes"][0]["message"] = "changed";
    EXPECT_EQ(destination.call(
        "import_repository",
        {{"customer_id", target_customer.value["id"]},
         {"workspace_id",
          destination.workspace(target_customer.value["id"])},
         {"name", "tampered"},
         {"archive", altered}})
                  .error.code,
              "invalid_digest");
}

// A webhook's signing secret never leaves lake in an archive, and only a repository administrator's
// archive names who holds access (lake#933). An imported webhook signs with a new secret.
TEST(Export, AnArchiveCarriesNoWebhookSecretAndGrantsOnlyForAnAdministrator) {
    Surface source;
    RepositoryFixtureStore source_store;
    source.ctx.sync_store = &source_store;
    const ApiResult user = source.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = source.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = source.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", source.workspace(customer.value["id"])},
         {"name", "hooked"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    const std::string secret = "lsr-024-webhook-signing-secret";
    ASSERT_TRUE(source
                    .call("create_webhook",
                          {{"repository_id", repository_id},
                           {"url", "https://example.test/lake"},
                           {"events", json::array({"issue"})},
                           {"secret", secret}})
                    .ok);
    const auto object = source.store.find_policy_object("repository", repository_id);
    ASSERT_TRUE(object) << object.error().message;
    const std::string reader = "taap:1:ed25519:export-reader";
    ASSERT_TRUE(source.store
                    .create_grant({reader, object->id, "read", "reader-proof", "admin", "test"})
                    .has_value());

    const auto webhooks_of = [](const json& archive) {
        json found = json::array();
        for (const json& record : archive["payload"]["forge"]) {
            if (record.value("kind", "") == "webhook") {
                found.push_back(record);
            }
        }
        return found;
    };

    // A reader's archive: the webhook's endpoint and events, never its secret, and no grants.
    source.ctx.acting_identity = reader;
    source.ctx.operator_credential = false;
    const ApiResult read_export =
        source.call("export_repository", {{"repository_id", repository_id}});
    ASSERT_TRUE(read_export.ok) << read_export.error.message;
    const json& read_archive = read_export.value["archive"];
    EXPECT_EQ(read_export.value.dump().find(secret), std::string::npos);
    const json read_webhooks = webhooks_of(read_archive);
    ASSERT_EQ(read_webhooks.size(), 1u);
    EXPECT_EQ(read_webhooks[0]["value"]["url"], "https://example.test/lake");
    EXPECT_FALSE(read_webhooks[0]["value"].contains("secret"));
    EXPECT_EQ(read_archive["payload"]["grants"], json::array());

    // An administrator's archive keeps the grants, still without the secret.
    source.ctx.acting_identity = "admin";
    source.ctx.operator_credential = true;
    const ApiResult admin_export =
        source.call("export_repository", {{"repository_id", repository_id}});
    ASSERT_TRUE(admin_export.ok) << admin_export.error.message;
    const json& admin_archive = admin_export.value["archive"];
    EXPECT_EQ(admin_export.value.dump().find(secret), std::string::npos);
    EXPECT_FALSE(webhooks_of(admin_archive)[0]["value"].contains("secret"));
    EXPECT_TRUE(std::ranges::any_of(admin_archive["payload"]["grants"], [&](const json& grant) {
        return grant.value("principal_taap_id", "") == reader;
    }));

    // The imported webhook signs with a secret of its own: neither the source's nor an empty one.
    Surface destination;
    RepositoryFixtureStore destination_store;
    destination.ctx.sync_store = &destination_store;
    const ApiResult target_user = destination.call("create_user", {{"display_name", "Grace"}});
    ASSERT_TRUE(target_user.ok) << target_user.error.message;
    const ApiResult target_customer = destination.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", target_user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(target_customer.ok) << target_customer.error.message;
    const ApiResult imported =
        destination.call("import_repository",
                         {{"customer_id", target_customer.value["id"]},
                          {"workspace_id", destination.workspace(target_customer.value["id"])},
                          {"name", "hooked-copy"},
                          {"archive", read_archive}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    std::string delivered_payload;
    std::string delivered_signature;
    destination.ctx.webhook_sender = [&](const std::string&,
                                         const json& headers,
                                         const std::string& payload) -> WebhookDeliveryResult {
        delivered_payload = payload;
        delivered_signature = headers.at("x-lake-signature-256").get<std::string>();
        return {.status = 204};
    };
    ASSERT_TRUE(destination
                    .call("create_issue",
                          {{"repository_id", imported.value["id"]}, {"title", "Trigger hook"}})
                    .ok);
    ASSERT_FALSE(delivered_payload.empty());
    const auto signed_with = [&](std::string_view key) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        HMAC(EVP_sha256(),
             key.data(),
             static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(delivered_payload.data()),
             delivered_payload.size(),
             digest,
             &size);
        return "sha256=" + lake::control::identity_crypto::base64url_encode(
                               std::string_view(reinterpret_cast<const char*>(digest), size));
    };
    EXPECT_NE(delivered_signature, signed_with(""));
    EXPECT_NE(delivered_signature, signed_with(secret));
}

TEST(GitHubImport, MapsRecordedIssuesReviewsAndWorkflowReport) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                        "github-import-recording.json");
    ASSERT_TRUE(input.good());
    const json recording = json::parse(input);
    // The checkout is read from the customer's own folder of the staging root (lake#925).
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lake-import-" + lake::control::new_uuid_v4());
    const std::string customer_id = customer.value["id"];
    std::filesystem::create_directories(root / customer_id / "example.git");
    surface.ctx.git_import_root = root.string();
    const ApiResult imported = surface.call(
        "import_github_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", surface.workspace(customer.value["id"])},
         {"name", "example"},
         {"git_directory", "example.git"},
         {"recording", recording}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    std::filesystem::remove_all(root);
    EXPECT_EQ(imported.value["fidelity"]["format"], "github_import_fidelity/1");
    EXPECT_EQ(imported.value["fidelity"]["workflows"][0]["wrap_replacements"][0]["replacement"],
              "liquid wrap");
    EXPECT_EQ(surface.call("list_issues", {{"repository_id", imported.value["repository"]["id"]}})
                  .value["items"]
                  .size(),
              1);
    EXPECT_EQ(surface.call("list_reviews", {{"repository_id", imported.value["repository"]["id"]}})
                  .value["items"]
                  .size(),
              1);
}

// import_github_repository reads only a checkout in the caller's own folder of the staging root:
// never a server path the caller chose, another customer's folder, or a link out (lake#925).
TEST(GitHubImport, ReadsOnlyTheCallersStagingFolder) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string customer_id = customer.value["id"];
    const auto import_from = [&](const std::string& directory) {
        return surface.call("import_github_repository",
                            {{"customer_id", customer_id},
                             {"workspace_id", surface.workspace(customer_id)},
                             {"name", "example-" + lake::control::new_uuid_v4().substr(0, 8)},
                             {"git_directory", directory},
                             {"recording", json::object()}});
    };
    // No staging root: the import is not offered.
    EXPECT_EQ(import_from("example.git").error.code, "unavailable");

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lake-import-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(root / customer_id / "example.git");
    std::filesystem::create_directories(root / "someone-else" / "theirs.git");
    std::filesystem::create_symlink(root / "someone-else", root / customer_id / "link");
    surface.ctx.git_import_root = root.string();
    for (const std::string refused : {std::string("/tmp/lake-export-1"),
                                      (root / customer_id / "example.git").string(),
                                      std::string("../someone-else/theirs.git"),
                                      std::string("example.git/../../someone-else/theirs.git"),
                                      std::string("./example.git"),
                                      std::string("a//b"),
                                      std::string("link/theirs.git"),
                                      std::string("missing.git")}) {
        const ApiResult result = import_from(refused);
        EXPECT_EQ(result.error.code, "invalid_input") << refused;
        EXPECT_EQ(result.error.path, "/git_directory") << refused;
    }
    EXPECT_TRUE(import_from("example.git").ok);
    std::filesystem::remove_all(root);
}

TEST(Forge, ReleaseRetainsVerifiedArtifactAttestations) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", surface.workspace(customer.value["id"])},
         {"name", "releases"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;

    const ApiResult release = surface.call("create_release",
                                           {{"repository_id", repository.value["id"]},
                                            {"tag", "v1.0.0"},
                                            {"change_id", "change-1"},
                                            {"artifact_ids", json::array({"result-1"})}});
    ASSERT_TRUE(release.ok) << release.error.message;
    EXPECT_EQ(release.value["value"]["artifacts"][0]["id"], "result-1");
    EXPECT_EQ(release.value["value"]["artifacts"][0]["attestations"][0]["id"], "attestation-1");
}

TEST(Forge, ReleaseRejectsArtifactsBoundToAnotherSource) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const auto user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const auto customer = surface.call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", user.value["id"]},
                                        {"name", "release-binding"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call("create_repository",
                                         {{"customer_id", customer.value["id"]},
                                          {"workspace_id", surface.workspace(customer.value["id"])},
                                          {"name", "releases"}});
    ASSERT_TRUE(repository.ok);
    const std::string id = repository.value["id"];
    const json input = {{"repository_id", id},
                        {"tag", "v1"},
                        {"change_id", "change-C"},
                        {"artifact_ids", json::array({"result-B"})}};
    repository_store.verified_source = RepositoryArtifactSource{id, "change-B"};
    EXPECT_EQ(surface.call("create_release", input).error.code, "conflict");
    repository_store.verified_source = RepositoryArtifactSource{"another-repository", "change-C"};
    EXPECT_EQ(surface.call("create_release", input).error.code, "conflict");
    EXPECT_TRUE(surface.call("list_releases", {{"repository_id", id}}).value["items"].empty());
    repository_store.verified_source = RepositoryArtifactSource{id, "change-C"};
    const auto bound = surface.call("create_release", input);
    ASSERT_TRUE(bound.ok) << bound.error.message;
    EXPECT_EQ(bound.value["value"]["artifacts"][0].value("source_binding", ""), "verified");
    repository_store.verified_source.reset();
    const auto legacy = surface.call("create_release", input);
    ASSERT_TRUE(legacy.ok) << legacy.error.message;
    EXPECT_EQ(legacy.value["value"]["artifacts"][0].value("source_binding", ""),
              "source not bound");
}

TEST(Forge, RequiredVerificationRechecksEveryReleaseArtifactSource) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const auto user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const auto customer = surface.call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", user.value["id"]},
                                        {"name", "verified-push"}});
    ASSERT_TRUE(customer.ok);
    const auto created = surface.call("create_repository",
                                      {{"customer_id", customer.value["id"]},
                                       {"workspace_id", surface.workspace(customer.value["id"])},
                                       {"name", "protected"}});
    ASSERT_TRUE(created.ok);
    const auto repository = surface.store.get_repository(created.value["id"]);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(surface
                    .call("set_branch_protection",
                          {{"repository_id", repository->id},
                           {"ref", "main"},
                           {"required_verification", true}})
                    .ok);
    // A legacy/imported record claims verified evidence, including forged source metadata.
    surface.workspaces->forge(*repository,
                              "release",
                              {{"change_id", "change-C"},
                               {"artifacts",
                                json::array({{{"id", "result-B"},
                                              {"source_binding", "verified"},
                                              {"repository_id", repository->id},
                                              {"change_id", "change-C"}}})}},
                              surface.ctx.instance_kek);
    const BranchRefUpdate update{.name = "main", .new_head = "change-C", .fast_forward = true};
    const auto evaluate = [&] {
        auto loaded = liquid::runtime::block_on(load_branch_protection(surface.ctx, *repository));
        EXPECT_TRUE(loaded && *loaded);
        if (!loaded || !*loaded) {
            return false;
        }
        return evaluate_branch_protection(
                   surface.store, "admin", surface.ctx.authorization_cache, **loaded, update)
            .has_value();
    };
    EXPECT_FALSE(evaluate()) << "unbound snapshots cannot satisfy verification";
    repository_store.verified_source = RepositoryArtifactSource{repository->id, "change-B"};
    EXPECT_FALSE(evaluate());
    repository_store.verified_source = RepositoryArtifactSource{"another-repository", "change-C"};
    EXPECT_FALSE(evaluate());
    repository_store.verified_source = RepositoryArtifactSource{repository->id, "change-C"};
    EXPECT_TRUE(evaluate());
    // Revalidate on the next push, even if the release snapshot previously passed.
    repository_store.verified_source.reset();
    EXPECT_FALSE(evaluate());
    repository_store.artifact_error = SyncStoreError{"store_unavailable", "evidence unavailable"};
    auto unavailable = liquid::runtime::block_on(load_branch_protection(surface.ctx, *repository));
    ASSERT_TRUE(unavailable);
    EXPECT_FALSE(*unavailable);
    repository_store.artifact_error = SyncStoreError{"not_found", "evidence removed"};
    EXPECT_FALSE(evaluate());
    repository_store.artifact_error.reset();
    // Every selected artifact must bind; a matching artifact never excuses an unbound one.
    repository_store.verified_source = RepositoryArtifactSource{repository->id, "change-D"};
    repository_store.artifact_sources["unbound"] = std::nullopt;
    surface.workspaces->forge(
        *repository,
        "release",
        {{"change_id", "change-D"},
         {"artifacts", json::array({{{"id", "bound"}}, {{"id", "unbound"}}})}},
        surface.ctx.instance_kek);
    auto mixed = liquid::runtime::block_on(load_branch_protection(surface.ctx, *repository));
    ASSERT_TRUE(mixed && *mixed);
    EXPECT_FALSE(evaluate_branch_protection(
        surface.store,
        "admin",
        surface.ctx.authorization_cache,
        **mixed,
        BranchRefUpdate{.name = "main", .new_head = "change-D", .fast_forward = true}));
    // Malformed legacy evidence fails closed without throwing on a non-object item.
    surface.workspaces->forge(*repository,
                              "release",
                              {{"change_id", "change-E"}, {"artifacts", json::array({17})}},
                              surface.ctx.instance_kek);
    auto loaded = liquid::runtime::block_on(load_branch_protection(surface.ctx, *repository));
    ASSERT_TRUE(loaded && *loaded);
    EXPECT_FALSE(evaluate_branch_protection(
        surface.store,
        "admin",
        surface.ctx.authorization_cache,
        **loaded,
        BranchRefUpdate{.name = "main", .new_head = "change-E", .fast_forward = true}));
}

TEST(Forge, WebhookDeliveryRetriesAndRetainsAnEventLog) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", surface.workspace(customer.value["id"])},
         {"name", "hooks"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    const ApiResult webhook = surface.call("create_webhook",
                                           {{"repository_id", repository_id},
                                            {"url", "https://example.test/lake"},
                                            {"events", json::array({"issue"})},
                                            {"secret", "delivery-secret"}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;

    int calls = 0;
    surface.ctx.webhook_sender = [&calls](const std::string&,
                                          const json& headers,
                                          const std::string&) -> WebhookDeliveryResult {
        EXPECT_TRUE(headers.at("x-lake-signature-256").get<std::string>().starts_with("sha256="));
        ++calls;
        if (calls == 1) {
            return {.status = 503, .error = "service unavailable"};
        }
        return {.status = 204};
    };
    ASSERT_TRUE(
        surface.call("create_issue", {{"repository_id", repository_id}, {"title", "Trigger hook"}})
            .ok);

    const ApiResult listed =
        surface.call("list_webhook_deliveries", {{"repository_id", repository_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1);
    EXPECT_FALSE(listed.value["items"][0]["value"]["delivered"]);
    const ApiResult retried = surface.call(
        "retry_webhook_delivery",
        {{"repository_id", repository_id}, {"delivery_id", listed.value["items"][0]["id"]}});
    ASSERT_TRUE(retried.ok) << retried.error.message;
    EXPECT_TRUE(retried.value["value"]["delivered"]);
    EXPECT_EQ(retried.value["value"]["attempt"], 2);
    EXPECT_EQ(calls, 2);
}

TEST(Forge, BranchProtectionRequiresAnApprovedReview) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", surface.workspace(customer.value["id"])},
         {"name", "protected"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    ASSERT_TRUE(surface
                    .call("set_branch_protection",
                          {{"repository_id", repository_id},
                           {"ref", "refs/heads/main"},
                           {"required_review", true},
                           {"allowed_pushers", json::array({"admin"})}})
                    .ok);

    const BranchRefUpdate update{
        .name = "refs/heads/main", .new_head = "change-1", .fast_forward = true};
    // The evaluation receives the repository with its workspace (lake#315).
    const auto stored = surface.store.get_repository(repository_id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->workspace_id, repository.value["workspace_id"].get<std::string>());
    // Its rules, reviews and releases are read from that workspace before a push (lake#341).
    const auto load = [&] {
        auto loaded =
            liquid::runtime::block_on(load_branch_protection(surface.ctx, *stored));
        EXPECT_TRUE(loaded && *loaded);
        return std::move(**loaded);
    };
    const auto rules = load();
    ASSERT_EQ(rules.rules.size(), 1u);
    const auto rejected = evaluate_branch_protection(
        surface.store, "admin", surface.ctx.authorization_cache, rules, update);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "conflict");

    const ApiResult review = surface.call(
        "create_review", {{"repository_id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(review.ok) << review.error.message;
    // Someone other than the pusher, with review on the repository, approves.
    const auto object = surface.store.find_policy_object("repository", repository_id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant({"taap:1:ed25519:reviewer",
                                            object->id,
                                            "admin",
                                            "attestation-reviewer",
                                            "admin",
                                            "request-reviewer"}));
    surface.ctx.acting_identity = "taap:1:ed25519:reviewer";
    surface.ctx.operator_credential = false;
    const ApiResult approval = surface.call("update_review",
                                            {{"repository_id", repository_id},
                                             {"review_id", review.value["id"]},
                                             {"state", "approved"}});
    ASSERT_TRUE(approval.ok) << approval.error.message;
    surface.ctx.acting_identity = "admin";
    surface.ctx.operator_credential = true;
    const auto approved = load();
    ASSERT_EQ(approved.reviews.size(), 1u);
    EXPECT_TRUE(evaluate_branch_protection(
        surface.store, "admin", surface.ctx.authorization_cache, approved, update));
    EXPECT_TRUE(approved.releases.empty()) << "no rule asks for verification";
}

// A required review is satisfied only by someone other than the pusher who holds `review`
// when the push is evaluated, and never by the person who opened the review.
TEST(Forge, ARequiredReviewNeedsASecondPersonWithReview) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository =
        surface.call("create_repository",
                     {{"customer_id", customer.value["id"]},
                      {"workspace_id", surface.workspace(customer.value["id"])},
                      {"name", "two-person"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    ASSERT_TRUE(surface
                    .call("set_branch_protection",
                          {{"repository_id", repository_id},
                           {"ref", "refs/heads/main"},
                           {"required_review", true}})
                    .ok);
    const auto object = surface.store.find_policy_object("repository", repository_id);
    ASSERT_TRUE(object);
    const std::string author = "taap:1:ed25519:review-author";
    const std::string peer = "taap:1:ed25519:review-peer";
    const std::string writer = "taap:1:ed25519:review-writer";
    std::string peer_grant;
    for (const auto& [principal, role] :
         {std::pair{author, "admin"}, std::pair{peer, "admin"}, std::pair{writer, "write"}}) {
        const auto grant = surface.store.create_grant(
            {principal, object->id, role, "attestation-" + principal, "admin", "request"});
        ASSERT_TRUE(grant) << grant.error().message;
        if (principal == peer) {
            peer_grant = grant->id;
        }
    }
    const auto as = [&](const std::string& identity) {
        surface.ctx.acting_identity = identity;
        surface.ctx.operator_credential = false;
        surface.ctx.authorization_cache = {};
    };
    const auto review_state = [&](const json& review, const std::string& state) {
        return surface.call(
            "update_review",
            {{"repository_id", repository_id}, {"review_id", review["id"]}, {"state", state}});
    };
    const auto stored = surface.store.get_repository(repository_id);
    ASSERT_TRUE(stored);
    const BranchRefUpdate update{
        .name = "refs/heads/main", .new_head = "change-1", .fast_forward = true};
    const auto push_as = [&](const std::string& pusher) {
        auto loaded = liquid::runtime::block_on(load_branch_protection(surface.ctx, *stored));
        EXPECT_TRUE(loaded && *loaded);
        lake::control::AuthorizationCache cache;
        return evaluate_branch_protection(surface.store, pusher, cache, **loaded, update);
    };

    // One person opens, approves and pushes: the approval is refused and the push with it.
    as(author);
    const ApiResult own = surface.call(
        "create_review", {{"repository_id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(own.ok) << own.error.message;
    EXPECT_EQ(own.value["value"]["opened_by"], author);
    EXPECT_EQ(review_state(own.value, "approved").error.code, "forbidden");
    const auto alone = push_as(author);
    ASSERT_FALSE(alone);
    EXPECT_EQ(alone.error().rule, "required_review");

    // A writer without `review` cannot approve at all.
    as(writer);
    const ApiResult opened_by_writer = surface.call(
        "create_review", {{"repository_id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(opened_by_writer.ok) << opened_by_writer.error.message;
    EXPECT_EQ(review_state(opened_by_writer.value, "approved").error.code, "unauthorized");

    // The author approves the writer's review: it names them, and counts for the writer's push
    // but never for the author's own.
    as(author);
    const ApiResult approved_by_author = review_state(opened_by_writer.value, "approved");
    ASSERT_TRUE(approved_by_author.ok) << approved_by_author.error.message;
    EXPECT_EQ(approved_by_author.value["value"]["approval"]["by"], author);
    EXPECT_EQ(approved_by_author.value["value"]["approval"]["change_id"], "change-1");
    EXPECT_TRUE(push_as(writer));
    const auto self_approved = push_as(author);
    ASSERT_FALSE(self_approved);
    EXPECT_EQ(self_approved.error().rule, "required_review");

    // A second person's approval lets the author's push through.
    as(peer);
    ASSERT_TRUE(review_state(own.value, "approved").ok);
    EXPECT_TRUE(push_as(author));

    // Reopening withdraws an approval.
    as(author);
    const ApiResult reopened = review_state(own.value, "open");
    ASSERT_TRUE(reopened.ok) << reopened.error.message;
    EXPECT_TRUE(reopened.value["value"]["approval"].is_null());
    EXPECT_FALSE(push_as(author));

    // Approval is checked when the push is: once the peer loses `review`, theirs no longer counts.
    as(peer);
    ASSERT_TRUE(review_state(own.value, "approved").ok);
    EXPECT_TRUE(push_as(author));
    ASSERT_TRUE(surface.store.revoke_grant(peer_grant, false));
    const auto revoked = push_as(author);
    ASSERT_FALSE(revoked);
    EXPECT_EQ(revoked.error().rule, "required_review");
}

TEST(Repositories, AnonymousCallersOnlySeePublicRepositories) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult private_repository =
        s.call(
            "create_repository",
            {{"customer_id", customer.value["id"]},
             {"workspace_id", lake::test::repository_workspace(s.store, customer.value["id"])},
             {"name", "private-repository"}});
    const ApiResult public_repository = s.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(s.store, customer.value["id"])},
         {"name", "public-repository"},
         {"visibility", "public"}});
    ASSERT_TRUE(private_repository.ok);
    ASSERT_TRUE(public_repository.ok);

    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    const ApiResult listed = s.call("list_repositories");
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1u);
    EXPECT_EQ(listed.value["items"][0]["id"], public_repository.value["id"]);
    EXPECT_TRUE(s.call("get_repository", {{"id", public_repository.value["id"]}}).ok);
    const ApiResult hidden = s.call("get_repository", {{"id", private_repository.value["id"]}});
    EXPECT_EQ(hidden.error.code, "unauthenticated");

    for (const auto& capability : s.registry.list()) {
        if (capability->name.starts_with("list_")) {
            EXPECT_TRUE(capability->authorizes_candidates) << capability->name;
        }
    }
}

TEST(WebApps, SourceTokenReadsOnlyItsPrivateRepositoryRevision) {
    Surface surface;
    RepositoryFixtureStore content;
    surface.ctx.sync_store = &content;
    const auto user =
        surface.store.create_user({"taap:app-source-owner", "source@example.test", "Source"});
    ASSERT_TRUE(user);
    const auto customer =
        surface.store.create_customer({"individual", user->id, "", "app-source-owner"});
    ASSERT_TRUE(customer);
    const auto repository = surface.store.create_repository({customer->id, "app", "", "private",
                                                             lake::test::repository_workspace(
                                                                 surface.store, customer->id)});
    ASSERT_TRUE(repository);
    const auto other = surface.store.create_repository({customer->id, "other", "", "public",
                                                        lake::test::repository_workspace(
                                                            surface.store, customer->id)});
    ASSERT_TRUE(other);
    lake::control::NewWebApp input;
    input.customer_id = customer->id;
    const auto app_workspace = surface.store.create_workspace({customer->id, "apps", "dev", ""});
    ASSERT_TRUE(app_workspace);
    input.workspace_id = app_workspace->id;
    input.name = "reader";
    input.mode = "runtime";
    input.source_repository = repository->id;
    input.source_revision = "main";
    input.port = 8080;
    input.host = "reader.apps.example.test";
    const auto app = surface.store.create_web_app(input);
    ASSERT_TRUE(app);
    ASSERT_TRUE(surface.store.set_web_app_token_hashes(
        app->id, {{"source", lake::control::identity_crypto::sha256_hex("operator-source")}}));
    const auto principal = surface.store.context_for("operator-source");
    ASSERT_TRUE(principal);
    surface.ctx.operator_credential = false;
    surface.ctx.session_credential = true;
    surface.ctx.acting_identity = principal->taap_id;
    surface.ctx.credential_kind = principal->kind;
    surface.ctx.credential_workload_id = principal->workload_id;
    surface.ctx.credential_customer_id = principal->workload_customer_id;
    surface.ctx.credential_repository_id = principal->workload_repository_id;
    surface.ctx.credential_revision = principal->workload_revision;
    surface.ctx.credential_token_hash = principal->workload_token_hash;
    surface.ctx.scopes = principal->scopes;
    const auto tree = surface.call("repository_tree", {{"id", repository->id}, {"ref", "main"}});
    ASSERT_TRUE(tree.ok) << tree.error.message;
    EXPECT_EQ(tree.value["items"][0]["path"], "readme.md");
    const auto file = surface.call(
        "repository_file", {{"id", repository->id}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(file.ok) << file.error.message;
    EXPECT_EQ(file.value["contents"], "new\nshared\n");
    EXPECT_EQ(
        surface.call("repository_file", {{"id", other->id}, {"ref", "main"}, {"path", "readme.md"}})
            .error.code,
        "unauthorized");
    EXPECT_EQ(
        surface.call("repository_tree", {{"id", repository->id}, {"ref", "other"}}).error.code,
        "unauthorized");
    EXPECT_EQ(surface.call("repository_refs", {{"id", repository->id}}).error.code, "unauthorized");
}

TEST(Repositories, PublicRepositoryContentCapabilitiesUseTheRepositoryStore) {
    Surface s;
    RepositoryFixtureStore repository_store;
    s.ctx.sync_store = &repository_store;
    // The fixture's changes are signed by this identity (lake#1326).
    const ApiResult user = s.call("create_user",
                                  {{"display_name", "Ada"},
                                   {"taap_id", "taap:1:ed25519:author"},
                                   {"email", "ada@example.test"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = s.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(s.store, customer.value["id"])},
         {"name", "public-repository"},
         {"visibility", "public"}});
    ASSERT_TRUE(repository.ok);
    const std::string repository_id = repository.value["id"];

    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    EXPECT_EQ(s.call("repository_refs", {{"id", repository_id}}).value["items"][0]["name"], "main");
    EXPECT_EQ(s.call("repository_tree", {{"id", repository_id}, {"ref", "main"}})
                  .value["items"][0]["path"],
              "readme.md");
    EXPECT_EQ(
        s.call("repository_file", {{"id", repository_id}, {"ref", "main"}, {"path", "readme.md"}})
            .value["contents"],
        "new\nshared\n");
    // The File page's syntax-unit shading (lake#561): only on request, only for parsed formats.
    const ApiResult source =
        s.call("repository_file",
               {{"id", repository_id}, {"ref", "main"}, {"path", "src/main.cpp"}, {"units", true}});
    ASSERT_TRUE(source.ok) << source.error.message;
    ASSERT_TRUE(source.value.contains("units"));
    ASSERT_FALSE(source.value["units"].empty());
    EXPECT_LE(source.value["units"][0]["start_line"], source.value["units"][0]["end_line"]);
    EXPECT_TRUE(source.value["units"][0]["kind"].is_number_integer());
    EXPECT_FALSE(s.call("repository_file",
                        {{"id", repository_id}, {"ref", "main"}, {"path", "src/main.cpp"}})
                     .value.contains("units"));
    EXPECT_FALSE(
        s.call("repository_file",
               {{"id", repository_id}, {"ref", "main"}, {"path", "readme.md"}, {"units", true}})
            .value.contains("units"));
    // The model page's space/name (resolve_model): a public repository needs no credential.
    const ApiResult resolved =
        s.call("resolve_model", {{"namespace", "ada"}, {"name", "public-repository"}});
    ASSERT_TRUE(resolved.ok) << resolved.error.message;
    EXPECT_EQ(resolved.value["repository_id"], repository_id);
    EXPECT_EQ(resolved.value["revision"], "main");
    EXPECT_EQ(s.call("resolve_model", {{"namespace", "ada"}, {"name", "missing"}}).error.code,
              "not_found");
    const ApiResult changes = s.call("list_changes", {{"id", repository_id}});
    EXPECT_EQ(changes.value["items"].size(), 1u);
    // A visitor reads the author by name and handle, never the email (lake#1326).
    const json ada = {{"kind", "user"}, {"name", "Ada"}, {"handle", "ada"}};
    EXPECT_EQ(changes.value["people"], json({{"taap:1:ed25519:author", ada}}));
    EXPECT_EQ(changes.value.dump().find("ada@example.test"), std::string::npos);
    const ApiResult change =
        s.call("get_change", {{"id", repository_id}, {"change_id", "change-1"}});
    EXPECT_EQ(change.value["message"], "initial");
    EXPECT_EQ(change.value["people"], json({{"taap:1:ed25519:author", ada}}));
    const ApiResult difference =
        s.call("change_diff", {{"id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(difference.ok) << difference.error.message;
    EXPECT_EQ(difference.value["items"][0]["path"], "readme.md");
    EXPECT_EQ(difference.value["items"][0]["line_hunks"][0]["added_lines"][0], "hello");
    const ApiResult conflicts = s.call("list_conflicts", {{"id", repository_id}});
    ASSERT_TRUE(conflicts.ok) << conflicts.error.message;
    EXPECT_EQ(conflicts.value["items"][0]["region"]["path"], "readme.md");
    EXPECT_EQ(conflicts.value["items"][0]["kind"], "suspicious-merge");
    EXPECT_EQ(conflicts.value["items"][0]["explanation"], "Both sides changed the greeting.");
    EXPECT_EQ(conflicts.value["items"][0]["reasons"][0], "The edits choose different words.");
    const ApiResult artifacts = s.call("list_artifacts", {{"id", repository_id}, {"limit", 1}});
    ASSERT_TRUE(artifacts.ok) << artifacts.error.message;
    EXPECT_EQ(artifacts.value["items"][0]["id"], "result-1");
    EXPECT_TRUE(artifacts.value["done"]);
    const ApiResult artifact =
        s.call("get_artifact", {{"id", repository_id}, {"result_id", "result-1"}});
    ASSERT_TRUE(artifact.ok) << artifact.error.message;
    EXPECT_EQ(artifact.value["outputs"][0]["path"], "build/app");
    EXPECT_EQ(artifact.value["attestations"][0]["profile"], "slsa_provenance/1");
    EXPECT_EQ(artifact.value["trace"]["id"], "trace-1");
}

// lake#913: a policy object counts only inside its own account. One account cannot
// create objects carrying another account's resource identifiers (a folder, an environment, the
// account's secret mounts), and objects already in the store that carry them, made before the
// fix, are never adopted: the account the identifiers name keeps its resources.
void expect_policy_objects_stay_in_their_account(std::shared_ptr<lake::control::Store> store,
                                                 const std::string& database_uri) {
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    RepositoryFixtureStore repository_store;
    lake::test::RecordWorkspaces workspaces(store, database_uri);
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto call_as = [&](const std::string& taap, const std::string& name, json input) {
        ApiContext context;
        context.store = store.get();
        context.sync_store = &repository_store;
        context.workspaces = workspaces.service.get();
        context.instance_kek = std::string(drive_kek);
        context.node_name = "test";
        context.authorized = true;
        context.acting_identity = taap;
        const auto capability = registry.find(name);
        if (capability && capability->async_handler) {
            auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
            return completed ? std::move(*completed)
                             : ApiResult::failure("store_error", completed.error().message);
        }
        return registry.call(name, input, context);
    };
    struct Account {
        std::string taap, customer, customer_object, workspace, repository, repository_object;
    };
    // An account: its person, its customer with the person as admin, and a repository.
    const auto account = [&](const std::string& name) {
        Account made{.taap = "taap:1:ed25519:" + name + "-" + suffix};
        const auto user =
            store->create_user({made.taap, name + "-" + suffix + "@example.test", name});
        if (!user) {
            throw std::runtime_error("create_user: " + user.error().message);
        }
        const auto customer =
            store->create_customer({"individual", user->id, "", name + "-" + suffix});
        if (!customer) {
            throw std::runtime_error("create_customer: " + customer.error().message);
        }
        made.customer = customer->id;
        made.customer_object = store->find_policy_object("customer", customer->id)->id;
        const auto granted =
            store->create_grant({made.taap,
                                 made.customer_object,
                                 "admin",
                                 lake::control::identity_crypto::sha256_hex(made.taap),
                                 "admin",
                                 lake::control::new_uuid_v4()});
        if (!granted) {
            throw std::runtime_error("create_grant: " + granted.error().message);
        }
        made.workspace = workspaces.create(customer->id, "home");
        const auto repository =
            store->create_repository({customer->id, "drive", "", "private", made.workspace});
        if (!repository) {
            throw std::runtime_error("create_repository: " + repository.error().message);
        }
        made.repository = repository->id;
        made.repository_object = store->find_policy_object("repository", repository->id)->id;
        return made;
    };
    const Account owner = account("owner");
    const Account intruder = account("intruder");
    // The fixture tree's `shared` folder; repository_tree answers its node id to any reader.
    const std::string node = "22222222222222222222222222222222";
    const std::string folder_id = owner.repository + ":" + node;
    const std::string branch_id = owner.repository + ":refs/heads/main";
    const std::string environment_id =
        lake::control::platform_environment_id(owner.repository, "production");
    const std::string secrets_id = "secrets/" + owner.customer;

    // create_object refuses every identifier of the owner's, under the intruder's own objects.
    const auto create =
        [&](const std::string& type, const std::string& external_id, const std::string& parent) {
            return call_as(
                intruder.taap,
                "create_object",
                {{"object_type", type}, {"external_id", external_id}, {"parent_id", parent}});
        };
    EXPECT_EQ(create("path", folder_id, intruder.repository_object).error.code, "invalid_input");
    EXPECT_EQ(create("path", branch_id, intruder.repository_object).error.code, "invalid_input");
    EXPECT_EQ(create("environment", environment_id, intruder.repository_object).error.code,
              "invalid_input");
    EXPECT_EQ(create("environment", secrets_id, intruder.customer_object).error.code,
              "invalid_input");
    for (const auto& [type, id] : {std::pair{"path", folder_id},
                                   std::pair{"path", branch_id},
                                   std::pair{"environment", environment_id},
                                   std::pair{"environment", secrets_id}}) {
        EXPECT_EQ(store->find_policy_object(type, id).error().code, "not_found") << id;
    }
    // A path object in its own repository is still made.
    const ApiResult own =
        create("path", intruder.repository + ":" + node, intruder.repository_object);
    EXPECT_TRUE(own.ok) << own.error.message;

    // Objects that already carry the owner's identifiers under the intruder's objects, as
    // create_object made them before this fix.
    for (const auto& [type, id, parent] :
         {std::tuple{"path", folder_id, intruder.repository_object},
          std::tuple{"path", branch_id, intruder.repository_object},
          std::tuple{"environment", environment_id, intruder.customer_object},
          std::tuple{"environment", secrets_id, intruder.customer_object}}) {
        if (store->find_policy_object(type, id)) {
            continue; // create_object made it above (only without this fix)
        }
        const auto planted = store->create_policy_object(
            {type, id, parent, intruder.taap, lake::control::new_uuid_v4(), true});
        ASSERT_TRUE(planted) << planted.error().message;
    }
    // The intruder's admin does not reach the owner's folder: it cannot upload into it.
    const ApiResult upload =
        call_as(intruder.taap,
                "upload_drive_files",
                {{"repository_id", owner.repository},
                 {"ref", "main"},
                 {"path", "shared"},
                 {"files", json::array({{{"name", "planted.txt"}, {"content", "aGk="}}})}});
    EXPECT_EQ(upload.error.code, "unauthorized") << upload.error.message;
    EXPECT_TRUE(repository_store.committed_operations.empty());
    // The owner still reads it, and sharing it refuses to adopt the planted object.
    const ApiResult browsed =
        call_as(owner.taap,
                "drive_browse",
                {{"repository_id", owner.repository}, {"ref", "main"}, {"path", "shared"}});
    EXPECT_TRUE(browsed.ok) << browsed.error.message;
    EXPECT_EQ(call_as(owner.taap,
                      "share_drive_folder",
                      {{"repository_id", owner.repository},
                       {"ref", "main"},
                       {"path", "shared"},
                       {"principal_taap_id", owner.taap},
                       {"role", "read"}})
                  .error.code,
              "conflict");
    // The branch rule, the environment and the account's secret mounts are not adopted either.
    EXPECT_EQ(call_as(owner.taap,
                      "set_branch_protection",
                      {{"repository_id", owner.repository},
                       {"ref", "refs/heads/main"},
                       {"required_review", true}})
                  .error.code,
              "conflict");
    EXPECT_EQ(lake::control::environment_policy_object(*store, owner.repository, "production")
                  .error()
                  .code,
              "conflict");
    const std::string mount_id = lake::control::new_uuid_v4();
    const auto mount =
        store->create_secret_mount_route({mount_id, owner.customer, "", owner.workspace});
    ASSERT_FALSE(mount);
    EXPECT_EQ(mount.error().code, "conflict");
    EXPECT_FALSE(store->get_secret_mount_route(mount_id));
    EXPECT_EQ(store->find_policy_object("secret_mount", mount_id).error().code, "not_found");

    // Moving the owner's folder carries only the folder's own object (lake#929); the planted
    // object holding its id stays under the intruder's repository and is never moved into the
    // owner's, where it would then count as the folder's (lake#913).
    const ApiResult moved = call_as(owner.taap,
                                    "move_drive_path",
                                    {{"repository_id", owner.repository},
                                     {"ref", "main"},
                                     {"path", "shared"},
                                     {"destination", "moved-shared"}});
    EXPECT_TRUE(moved.ok) << moved.error.message;
    const auto planted_folder = store->find_policy_object("path", folder_id);
    ASSERT_TRUE(planted_folder) << planted_folder.error().message;
    EXPECT_EQ(planted_folder->parent_id, intruder.repository_object);
}

TEST(PolicyObjectOwnership, MemoryStore) {
    expect_policy_objects_stay_in_their_account(std::make_shared<lake::control::InMemoryStore>(),
                                                {});
}

TEST(PolicyObjectOwnership, PostgresStore) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    std::shared_ptr<lake::control::Store> store = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping());
    expect_policy_objects_stay_in_their_account(store, database_uri);
}

TEST(Drive, FolderSharingMatchesAuthorizeAndLinksExpire) {
    Surface surface;
    DriveWorkspaces workspaces(surface);
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:owner"}, {"display_name", "Owner"}});
    const ApiResult recipient = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:recipient"}, {"display_name", "Recipient"}});
    const ApiResult outsider = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:outsider"}, {"display_name", "Outsider"}});
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(recipient.ok);
    ASSERT_TRUE(outsider.ok);
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "owner"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const auto repository_object =
        surface.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(repository_object);
    ASSERT_TRUE(surface.store.create_grant({"taap:1:ed25519:owner",
                                            repository_object->id,
                                            "admin",
                                            "attestation-owner",
                                            "admin",
                                            "request-owner"}));
    const std::string workspace = workspaces.create(customer.value["id"], "home");

    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const ApiResult shared = surface.call("share_drive_folder",
                                          {{"repository_id", repository.value["id"]},
                                           {"ref", "main"},
                                           {"path", "shared"},
                                           {"principal_taap_id", "taap:1:ed25519:recipient"},
                                           {"role", "read"}});
    ASSERT_TRUE(shared.ok) << shared.error.message;
    const std::string folder_object = shared.value["object"]["id"];

    surface.ctx.acting_identity = "taap:1:ed25519:recipient";
    const ApiResult visible = surface.call(
        "drive_browse",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}});
    ASSERT_TRUE(visible.ok) << visible.error.message;
    ASSERT_EQ(visible.value["items"].size(), 1u);
    EXPECT_EQ(visible.value["items"][0]["path"], "shared/readme.md");

    const ApiResult shared_with_me = surface.call("drive_shared_with_me");
    ASSERT_TRUE(shared_with_me.ok) << shared_with_me.error.message;
    ASSERT_EQ(shared_with_me.value["items"].size(), 1u);
    EXPECT_EQ(shared_with_me.value["items"][0]["repository"]["id"], repository.value["id"]);
    EXPECT_EQ(shared_with_me.value["items"][0]["path"], "shared");
    EXPECT_EQ(shared_with_me.value["items"][0]["shared_by"], "taap:1:ed25519:owner");
    EXPECT_EQ(shared_with_me.value["items"][0]["actions"], json::array({"read"}));

    surface.ctx.acting_identity = "taap:1:ed25519:outsider";
    const ApiResult hidden = surface.call(
        "drive_browse",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}});
    EXPECT_EQ(hidden.error.code, "unauthorized");
    EXPECT_TRUE(surface.call("drive_shared_with_me").value["items"].empty());

    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const ApiResult effective = surface.call("drive_effective_access",
                                             {{"repository_id", repository.value["id"]},
                                              {"workspace_id", workspace},
                                              {"ref", "main"},
                                              {"path", "shared"}});
    ASSERT_TRUE(effective.ok) << effective.error.message;
    for (const json& entry : effective.value["items"]) {
        for (const std::string action : {"read", "write", "share"}) {
            lake::control::AuthorizationCache cache;
            const auto decision = lake::control::authorize(
                surface.store, entry["principal_taap_id"], folder_object, action, cache);
            ASSERT_TRUE(decision);
            const auto actions = entry["actions"].get<std::vector<std::string>>();
            EXPECT_EQ(decision->allowed, std::ranges::find(actions, action) != actions.end());
        }
    }

    const ApiResult active_link = surface.call("create_drive_share_link",
                                               {{"repository_id", repository.value["id"]},
                                                {"workspace_id", workspace},
                                                {"ref", "main"},
                                                {"path", "shared"},
                                                {"role", "read"},
                                                {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(active_link.ok) << active_link.error.message;
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    repository_store.shared_folder_moved = true;
    const ApiResult moved_link = surface.call(
        "resolve_drive_share_link", {{"token", active_link.value["token"]}, {"ref", "main"}});
    ASSERT_TRUE(moved_link.ok) << moved_link.error.message;
    EXPECT_EQ(moved_link.value["path"], "renamed");
    EXPECT_EQ(moved_link.value["items"][0]["path"], "renamed/readme.md");
    const json file_input{
        {"token", active_link.value["token"]}, {"ref", "main"}, {"path", "renamed/readme.md"}};
    const ApiResult preview = surface.call("preview_drive_share_link", file_input);
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_NE(preview.value["html"].get<std::string>().find("new"), std::string::npos);
    const ApiResult downloaded = surface.call("download_drive_share_link", file_input);
    ASSERT_TRUE(downloaded.ok) << downloaded.error.message;
    EXPECT_EQ(downloaded.value["content_base64"], base64_bytes(bytes("new\nshared\n")));
    EXPECT_EQ(surface.ctx.sync_response, bytes("new\nshared\n"));
    for (const std::string name :
         {"resolve_drive_share_link", "preview_drive_share_link", "download_drive_share_link"}) {
        for (const std::string path : {"",
                                       "readme.md",
                                       "shared/readme.md",
                                       "renamed-other/file",
                                       "renamed/../readme.md",
                                       "renamed//readme.md",
                                       "renamed/./readme.md",
                                       "/renamed/readme.md",
                                       "renamed/..",
                                       "renamed/.lake-folder"}) {
            // An operator session must not add authority to a bearer link either.
            surface.ctx.acting_identity = "admin";
            surface.ctx.authorized = true;
            const ApiResult denied = surface.call(
                name, {{"token", active_link.value["token"]}, {"ref", "main"}, {"path", path}});
            EXPECT_EQ(denied.error.code, "unauthorized") << name << " " << path;
            EXPECT_TRUE(surface.ctx.sync_response.empty());
        }
    }
    const json document = openapi_document(surface.registry, "0.0.1");
    const auto tools = mcp_dispatch(surface.registry,
                                    {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                    surface.ctx,
                                    "0.0.1");
    ASSERT_TRUE(tools);
    const json listed = json::parse(*tools)["result"]["tools"];
    for (const std::string name : {"preview_drive_share_link", "download_drive_share_link"}) {
        EXPECT_TRUE(document["paths"].contains("/v1/" + name));
        const auto tool =
            std::ranges::find_if(listed, [&](const json& entry) { return entry["name"] == name; });
        ASSERT_NE(tool, listed.end());
        EXPECT_EQ(tool->at("inputSchema"), surface.registry.find(name)->input_schema);
        EXPECT_EQ(tool->at("outputSchema"),
                  success_envelope_schema(surface.registry.find(name)->output_schema));
    }
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    // The share capabilities read the workspace that holds the link, so MCP runs them as the
    // server does, through the asynchronous dispatcher.
    const json mcp_call{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "tools/call"},
        {"params", {{"name", "download_drive_share_link"}, {"arguments", file_input}}}};
    const auto mcp_completed = liquid::runtime::block_on(
        mcp_dispatch_async(surface.registry, mcp_call, surface.ctx, "0.0.1"));
    ASSERT_TRUE(mcp_completed);
    const auto& mcp_download = *mcp_completed;
    ASSERT_TRUE(mcp_download);
    EXPECT_EQ(json::parse(*mcp_download)["result"]["structuredContent"]["value"]["content_base64"],
              downloaded.value["content_base64"]);
    surface.ctx.authorized = true;
    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const ApiResult revoked = surface.call(
        "revoke_drive_share_link", {{"workspace_id", workspace}, {"link_id", active_link.value["id"]}});
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    EXPECT_EQ(surface
                  .call("resolve_drive_share_link",
                        {{"token", active_link.value["token"]}, {"ref", "main"}})
                  .error.code,
              "unauthorized");

    for (const std::string name : {"preview_drive_share_link", "download_drive_share_link"}) {
        EXPECT_EQ(surface.call(name, file_input).error.code, "unauthorized");
        EXPECT_TRUE(surface.ctx.sync_response.empty());
    }

    // An expired link, written as an earlier create would have: its record in the workspace and
    // its grant in the policy store.
    const std::string expired_id = lake::control::new_uuid_v4();
    const std::string expired_token =
        lake::control::drive_share_token(workspace, expired_id, "expired-secret");
    {
        auto records =
            lake::api::workspace_records(surface.ctx, customer.value["id"], workspace);
        ASSERT_TRUE(records) << records.error().message;
        lake::control::DriveShareLink expired;
        expired.id = expired_id;
        expired.repository_id = repository.value["id"];
        expired.object_id = folder_object;
        expired.node_id = "22222222222222222222222222222222";
        expired.path = "shared";
        expired.role = "read";
        expired.token_hash = share_token_hash(expired_token);
        expired.expires_at = "2020-01-01T00:00:00Z";
        expired.created_by_taap_id = "taap:1:ed25519:owner";
        lake::control::DriveShareLinks links(*records);
        ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(
            links.append(expired, lake::control::RecordWrite{expired_id, 0}))));
        ASSERT_TRUE(surface.store.create_grant({"share-link:" + expired_id, folder_object, "read",
                                                "drive_share_link/" + expired_id,
                                                "taap:1:ed25519:owner", "request-expired"}));
    }
    const ApiResult expired =
        surface.call("resolve_drive_share_link", {{"token", expired_token}, {"ref", "main"}});
    EXPECT_EQ(expired.error.code, "unauthorized");
    for (const std::string name : {"preview_drive_share_link", "download_drive_share_link"}) {
        EXPECT_EQ(
            surface
                .call(name,
                      {{"token", expired_token}, {"ref", "main"}, {"path", "renamed/readme.md"}})
                .error.code,
            "unauthorized");
        EXPECT_TRUE(surface.ctx.sync_response.empty());
    }
}

// lake#585: a person shares a folder with someone by email or handle, from the space's people;
// lake writes and signs the grant's attestation, and a digest a caller makes up is refused.
TEST(Drive, SharingByEmailOrHandleIsAttestedByLake) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const auto user = [&](const std::string& name) {
        const ApiResult made = surface.call("create_user",
                                            {{"taap_id", "taap:1:ed25519:" + name},
                                             {"email", name + "@example.com"},
                                             {"display_name", name}});
        EXPECT_TRUE(made.ok) << made.error.message;
        return made;
    };
    const ApiResult owner = user("owner");
    const ApiResult ana = user("ana");
    const ApiResult outsider = user("outsider");
    ASSERT_TRUE(surface
                    .call("create_customer",
                          {{"customer_type", "individual"},
                           {"user_id", ana.value["id"]},
                           {"name", "ana-handle"}})
                    .ok);
    const ApiResult organization = surface.call(
        "create_organization", {{"taap_id", "taap:1:ed25519:studio"}, {"display_name", "Studio"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const ApiResult customer = surface.call("create_customer",
                                            {{"customer_type", "organization"},
                                             {"organization_id", organization.value["id"]},
                                             {"name", "studio"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    for (const ApiResult* member : {&owner, &ana}) {
        const ApiResult joined = surface.call("create_membership",
                                              {{"organization_id", organization.value["id"]},
                                               {"principal_type", "user"},
                                               {"principal_id", member->value["id"]},
                                               {"principal_taap_id", member->value["taap_id"]},
                                               {"role", member == &owner ? "owner" : "member"}});
        ASSERT_TRUE(joined.ok) << joined.error.message;
    }
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "drive"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const auto repository_object =
        surface.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(repository_object);
    ASSERT_TRUE(surface.store.create_grant({"taap:1:ed25519:owner",
                                            repository_object->id,
                                            "admin",
                                            "attestation-owner",
                                            "admin",
                                            "request-owner"}));
    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const json folder{
        {"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}};
    const auto with = [&](json extra) {
        json input = folder;
        input.update(extra);
        return surface.call("share_drive_folder", input);
    };

    // The people a folder can be shared with by name: the organization and its members.
    const ApiResult people = surface.call("drive_share_people", folder);
    ASSERT_TRUE(people.ok) << people.error.message;
    std::map<std::string, json> by_id;
    for (const json& person : people.value["people"]) {
        by_id[person["taap_id"]] = person;
    }
    ASSERT_TRUE(by_id.contains("taap:1:ed25519:ana"));
    EXPECT_EQ(by_id["taap:1:ed25519:ana"]["email"], "ana@example.com");
    EXPECT_EQ(by_id["taap:1:ed25519:ana"]["handle"], "ana-handle");
    EXPECT_EQ(by_id[organization.value["taap_id"]]["kind"], "organization");
    EXPECT_FALSE(by_id.contains("taap:1:ed25519:outsider"));

    // By email, any case: lake resolves the person and signs the attestation itself.
    const ApiResult shared = with({{"recipient", "Ana@Example.com"}, {"role", "read"}});
    ASSERT_TRUE(shared.ok) << shared.error.message;
    EXPECT_EQ(shared.value["grant"]["principal_taap_id"], "taap:1:ed25519:ana");
    const std::string digest = shared.value["attestation"]["digest"];
    EXPECT_TRUE(digest.starts_with("sha256:")) << digest;
    EXPECT_EQ(shared.value["grant"]["attestation_digest"], digest);
    EXPECT_EQ(shared.value["attestation"]["statement"]["principal_taap_id"], "taap:1:ed25519:ana");
    EXPECT_EQ(shared.value["attestation"]["statement"]["role"], "read");
    EXPECT_FALSE(shared.value["attestation"]["signature"].get<std::string>().empty());
    EXPECT_EQ(shared.value["attestation"]["signer"],
              shared.value["attestation"]["statement"]["issuer"]);

    // The same share again, by handle and with the digest lake answered: the same grant.
    const ApiResult again =
        with({{"recipient", "@ana-handle"}, {"role", "read"}, {"attestation_digest", digest}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["existing"], true);
    EXPECT_EQ(again.value["grant"]["id"], shared.value["grant"]["id"]);

    // A made-up digest, or one for another role, is refused at its field.
    const ApiResult made_up = with({{"recipient", "ana-handle"},
                                    {"role", "write"},
                                    {"attestation_digest", "sha256:" + std::string(64, 'a')}});
    EXPECT_EQ(made_up.error.code, "invalid_input");
    EXPECT_EQ(made_up.error.path, "/attestation_digest");
    EXPECT_EQ(with({{"recipient", "ana-handle"}, {"role", "write"}, {"attestation_digest", digest}})
                  .error.path,
              "/attestation_digest");

    // Someone outside the space is not found by email; a program names a TAAP id lake knows.
    const ApiResult outside = with({{"recipient", "outsider@example.com"}, {"role", "read"}});
    EXPECT_EQ(outside.error.code, "invalid_input");
    EXPECT_EQ(outside.error.path, "/recipient");
    const ApiResult unknown =
        with({{"principal_taap_id", "taap:1:ed25519:nobody"}, {"role", "read"}});
    EXPECT_EQ(unknown.error.path, "/principal_taap_id");
    const ApiResult program =
        with({{"principal_taap_id", "taap:1:ed25519:outsider"}, {"role", "read"}});
    ASSERT_TRUE(program.ok) << program.error.message;
    EXPECT_TRUE(program.value["attestation"]["digest"].get<std::string>().starts_with("sha256:"));
    EXPECT_EQ(
        with({{"recipient", "ana"}, {"principal_taap_id", "taap:1:ed25519:ana"}, {"role", "read"}})
            .error.code,
        "invalid_input");
    EXPECT_EQ(with({{"role", "read"}}).error.code, "invalid_input");
}

// Sharing must attenuate authority for both live grants and bearer links.
TEST(Drive, SharingCannotGrantActionsTheCallerDoesNotHold) {
    Surface surface;
    DriveWorkspaces workspaces(surface);
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const auto organization = surface.call(
        "create_organization", {{"taap_id", "taap:1:ed25519:studio"}, {"display_name", "Studio"}});
    ASSERT_TRUE(organization.ok);
    for (const std::string name : {"owner", "sharer", "member", "outsider"}) {
        const auto user = surface.call("create_user",
                                       {{"taap_id", "taap:1:ed25519:" + name},
                                        {"email", name + "@example.com"},
                                        {"display_name", name}});
        ASSERT_TRUE(user.ok);
        if (name != "outsider") {
            ASSERT_TRUE(surface
                            .call("create_membership",
                                  {{"organization_id", organization.value["id"]},
                                   {"principal_type", "user"},
                                   {"principal_id", user.value["id"]},
                                   {"principal_taap_id", user.value["taap_id"]},
                                   {"role", "member"}})
                            .ok);
        }
    }
    const auto customer = surface.call("create_customer",
                                       {{"customer_type", "organization"},
                                        {"organization_id", organization.value["id"]},
                                        {"name", "share-authority"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const auto repository_object =
        surface.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(repository_object);
    ASSERT_TRUE(surface.store.create_grant({"taap:1:ed25519:owner",
                                            repository_object->id,
                                            "admin",
                                            "owner-grant",
                                            "admin",
                                            "owner-request"}));
    const std::string workspace = workspaces.create(customer.value["id"], "home");
    const json folder{
        {"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}};
    const auto share = [&](json extra) {
        json input = folder;
        input.update(extra);
        return surface.call("share_drive_folder", input);
    };
    const auto link = [&](const std::string& role) {
        json input = folder;
        input.update(
            {{"workspace_id", workspace}, {"role", role}, {"expires_at", "2099-01-01T00:00:00Z"}});
        return surface.call("create_drive_share_link", input);
    };
    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const auto delegated =
        share({{"principal_taap_id", "taap:1:ed25519:sharer"}, {"role", "share"}});
    ASSERT_TRUE(delegated.ok) << delegated.error.message;
    const std::string object = delegated.value["object"]["id"];
    surface.ctx.acting_identity = "taap:1:ed25519:sharer";

    // Both recipient selectors must reject write before creating a grant.
    for (const json target : {json{{"recipient", "MEMBER@example.com"}},
                              json{{"principal_taap_id", "taap:1:ed25519:member"}}}) {
        json input = target;
        input["role"] = "write";
        const auto refused = share(input);
        EXPECT_FALSE(refused.ok);
        EXPECT_EQ(refused.error.code, "unauthorized");
        EXPECT_EQ(refused.error.path, "/role");
    }
    const auto refused_link = link("write");
    EXPECT_FALSE(refused_link.ok);
    EXPECT_EQ(refused_link.error.code, "unauthorized");
    EXPECT_EQ(refused_link.error.path, "/role");
    const auto grants = surface.store.list_grants({object}, std::nullopt, 100);
    ASSERT_TRUE(grants);
    EXPECT_EQ(grants->size(), 1u);
    const auto links = surface.call("drive_effective_access",
                                    {{"repository_id", repository.value["id"]},
                                     {"workspace_id", workspace},
                                     {"ref", "main"},
                                     {"path", "shared"}});
    ASSERT_TRUE(links.ok) << links.error.message;
    EXPECT_TRUE(links.value["links"].empty());

    for (const std::string role : {"read", "share"}) {
        const auto allowed = share({{"recipient", "member@example.com"}, {"role", role}});
        EXPECT_TRUE(allowed.ok) << allowed.error.message;
        const auto again = share({{"principal_taap_id", "taap:1:ed25519:member"}, {"role", role}});
        EXPECT_TRUE(again.ok) << again.error.message;
        const auto outside =
            share({{"principal_taap_id", "taap:1:ed25519:outsider"}, {"role", role}});
        EXPECT_FALSE(outside.ok);
        EXPECT_EQ(outside.error.code, "unauthorized");
        EXPECT_EQ(outside.error.path, "/principal_taap_id");
    }
    EXPECT_TRUE(link("read").ok);

    // An inherited write grant combines with folder share; write alone cannot re-share.
    ASSERT_TRUE(surface.store.create_grant({"taap:1:ed25519:sharer",
                                            repository_object->id,
                                            "write",
                                            "write-grant",
                                            "admin",
                                            "write-request"}));
    const auto allowed_write =
        share({{"principal_taap_id", "taap:1:ed25519:member"}, {"role", "write"}});
    EXPECT_TRUE(allowed_write.ok) << allowed_write.error.message;
    EXPECT_TRUE(link("write").ok);
    EXPECT_FALSE(share({{"principal_taap_id", "taap:1:ed25519:outsider"}, {"role", "write"}}).ok);
    const auto inherited =
        surface.store.list_grants({repository_object->id}, "taap:1:ed25519:sharer", 100);
    ASSERT_TRUE(inherited);
    ASSERT_EQ(inherited->size(), 1u);
    ASSERT_TRUE(surface.store.revoke_grant(inherited->front().id, false));
    const auto retry = share({{"principal_taap_id", "taap:1:ed25519:member"}, {"role", "write"}});
    EXPECT_FALSE(retry.ok);
    EXPECT_EQ(retry.error.path, "/role");
    EXPECT_FALSE(link("write").ok);

    surface.ctx.acting_identity = "taap:1:ed25519:member";
    // Remove the member's share role, leaving read and write only.
    const auto member_grants = surface.store.list_grants({object}, "taap:1:ed25519:member", 100);
    ASSERT_TRUE(member_grants);
    for (const auto& grant : *member_grants) {
        if (grant.role == "share") {
            ASSERT_TRUE(surface.store.revoke_grant(grant.id, false));
        }
    }
    EXPECT_FALSE(share({{"recipient", "owner@example.com"}, {"role", "read"}}).ok);
    EXPECT_FALSE(link("read").ok);

    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    EXPECT_TRUE(share({{"principal_taap_id", "taap:1:ed25519:outsider"}, {"role", "write"}}).ok);
}

// LSR-014 (lake#923): a diff or restore reads revisions other than the ref its object came from,
// and the node a path names can differ between them. A restricted folder renamed away, a shared
// one made under its old name: the old revision is the restricted folder's, and stays unreadable.
TEST(Drive, HistoryAuthorizesTheNodeAtEveryRevisionItReads) {
    Surface surface;
    RepositoryFixtureStore repositories;
    surface.ctx.sync_store = &repositories;
    const auto owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:history-owner"}, {"display_name", "Owner"}});
    ASSERT_TRUE(owner.ok);
    const auto customer = surface.call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", owner.value["id"]},
                                        {"name", "history-nodes"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call("create_repository",
                                         {{"customer_id", customer.value["id"]},
                                          {"workspace_id", surface.workspace(customer.value["id"])},
                                          {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const std::string repository_id = repository.value["id"];
    const auto object = surface.store.find_policy_object("repository", repository_id);
    ASSERT_TRUE(object);
    // Today's `shared` folder is shared with Bob; at change-old the same path was another node,
    // a restricted folder of its own.
    const auto shared_folder = surface.store.create_policy_object(
        {"path", repository_id + ":22222222222222222222222222222222", object->id, "admin", "test", false});
    const auto restricted_folder = surface.store.create_policy_object(
        {"path", repository_id + ":restricted-old", object->id, "admin", "test", false});
    ASSERT_TRUE(shared_folder && restricted_folder);
    ASSERT_TRUE(surface.store.create_grant(
        {"taap:1:ed25519:bob", shared_folder->id, "read", "test", "admin", "test"}));
    auto old_tree = *repositories.repository_tree("", "", "main");
    for (auto& entry : old_tree) {
        if (entry.path == "shared") {
            entry.node_id = "restricted-old";
        }
        if (entry.path == "shared/readme.md") {
            entry.node_id = "restricted-old-readme";
        }
    }
    repositories.trees_at["change-old"] = old_tree;
    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:bob";
    const json diff{{"repository_id", repository_id},
                    {"ref", "main"},
                    {"path", "shared/readme.md"},
                    {"left", "change-old"},
                    {"right", "main"}};
    EXPECT_EQ(surface.call("diff_drive_versions", diff).error.code, "unauthorized");
    json swapped = diff;
    swapped["left"] = "main";
    swapped["right"] = "change-old";
    EXPECT_EQ(surface.call("diff_drive_versions", swapped).error.code, "unauthorized");
    json restore{{"repository_id", repository_id},
                 {"ref", "main"},
                 {"path", "shared/readme.md"},
                 {"change_id", "change-old"}};
    EXPECT_EQ(surface.call("restore_drive_version", restore).error.code, "unauthorized");
    EXPECT_FALSE(repositories.restored);
    json same = diff;
    same["left"] = "main";
    EXPECT_NE(surface.call("diff_drive_versions", same).error.code, "unauthorized")
        << "both sides are the shared folder Bob may read";
}

// LSR-108 (lake#975): a path object that does not inherit cuts its folder off from the
// repository's grants for every read of repository content, as it does for Drive. A repository
// reader without a grant on the cut neither reads the folder's files nor sees them in the tree,
// and cannot read the repository whole; a grant there gives both back.
TEST(Drive, RepositoryReadsHonourRestrictedFolders) {
    Surface surface;
    RepositoryFixtureStore repositories;
    surface.ctx.sync_store = &repositories;
    const auto owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:cut-owner"}, {"display_name", "Owner"}});
    ASSERT_TRUE(owner.ok);
    const auto customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "cut-reads"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call("create_repository",
                                         {{"customer_id", customer.value["id"]},
                                          {"workspace_id", surface.workspace(customer.value["id"])},
                                          {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const std::string repository_id = repository.value["id"];
    const auto object = surface.store.find_policy_object("repository", repository_id);
    ASSERT_TRUE(object);
    // The fixture's `shared` folder is restricted: its object does not inherit.
    const auto restricted =
        surface.store.create_policy_object({"path",
                                            repository_id + ":22222222222222222222222222222222",
                                            object->id,
                                            "admin",
                                            "test",
                                            false});
    ASSERT_TRUE(restricted);
    const std::string reader = "taap:1:ed25519:cut-reader";
    ASSERT_TRUE(surface.store.create_grant({reader, object->id, "read", "test", "admin", "test"}));
    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = reader;

    const json restricted_file{
        {"id", repository_id}, {"ref", "main"}, {"path", "shared/readme.md"}};
    EXPECT_EQ(surface.call("repository_file", restricted_file).error.code, "unauthorized");
    EXPECT_TRUE(surface
                    .call("repository_file",
                          {{"id", repository_id}, {"ref", "main"}, {"path", "readme.md"}})
                    .ok);
    // A path the tree does not hold cannot be ruled out of the cut.
    EXPECT_EQ(surface
                  .call("repository_file",
                        {{"id", repository_id}, {"ref", "main"}, {"path", "./shared/readme.md"}})
                  .error.code,
              "unauthorized");
    const auto paths_of = [&] {
        std::set<std::string> paths;
        const auto tree = surface.call("repository_tree", {{"id", repository_id}, {"ref", "main"}});
        EXPECT_TRUE(tree.ok) << tree.error.message;
        for (const auto& item : tree.value["items"]) {
            paths.insert(item["path"].get<std::string>());
        }
        return paths;
    };
    auto listed = paths_of();
    EXPECT_TRUE(listed.contains("readme.md"));
    EXPECT_FALSE(listed.contains("shared"));
    EXPECT_FALSE(listed.contains("shared/readme.md"));
    EXPECT_EQ(surface.call("export_repository", {{"repository_id", repository_id}}).error.code,
              "unauthorized");

    // A grant on the restricted folder gives it back.
    ASSERT_TRUE(
        surface.store.create_grant({reader, restricted->id, "read", "test", "admin", "test"}));
    surface.ctx.authorization_cache = {};
    EXPECT_TRUE(surface.call("repository_file", restricted_file).ok);
    listed = paths_of();
    EXPECT_TRUE(listed.contains("shared"));
    EXPECT_TRUE(listed.contains("shared/readme.md"));
}

// LSR-015 (lake#924): a Drive write advances its branch, so the branch's rules apply to it as to a
// push. main allows only another person to push: an otherwise authorized writer's upload, restore
// and conflict resolution are refused and nothing is committed.
TEST(Drive, WritesFollowTheBranchRules) {
    Surface surface;
    RepositoryFixtureStore repositories;
    surface.ctx.sync_store = &repositories;
    const auto owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:rules-writer"}, {"display_name", "Writer"}});
    ASSERT_TRUE(owner.ok);
    const auto customer = surface.call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", owner.value["id"]},
                                        {"name", "branch-rules"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call("create_repository",
                                         {{"customer_id", customer.value["id"]},
                                          {"workspace_id", surface.workspace(customer.value["id"])},
                                          {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const auto object = surface.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant(
        {"taap:1:ed25519:rules-writer", object->id, "write", "test", "admin", "test"}));
    ASSERT_TRUE(surface
                    .call("set_branch_protection",
                          {{"repository_id", repository.value["id"]},
                           {"ref", "main"},
                           {"allowed_pushers", json::array({"taap:1:ed25519:someone-else"})}})
                    .ok);
    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:rules-writer";
    json upload{{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}};
    upload["files"] = json::array({{{"name", "notes.md"}, {"content", "bmV3"}}});
    const auto refused = surface.call("upload_drive_files", upload);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "unauthorized") << refused.error.message;
    EXPECT_TRUE(repositories.committed_operations.empty()) << "main is unchanged";
    const auto restore = surface.call("restore_drive_version",
                                      {{"repository_id", repository.value["id"]},
                                       {"ref", "main"},
                                       {"path", "shared/readme.md"},
                                       {"change_id", "change-1"}});
    EXPECT_FALSE(restore.ok);
    EXPECT_FALSE(repositories.restored);
}

// lake#1030: a folder's administrator creates path objects only for what lies in that folder.
// The node an object names must be in the repository's tree, and its parent must be the object of
// the nearest folder above the node that has one: a sibling, a file above the folder, a node
// deeper than its own folder object, a branch rule or an unknown node is refused.
TEST(Drive, CreateObjectPlacesAPathObjectByTheTree) {
    Surface surface;
    RepositoryFixtureStore repositories;
    surface.ctx.sync_store = &repositories;
    const auto owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:tree-owner"}, {"display_name", "Owner"}});
    ASSERT_TRUE(owner.ok);
    const auto customer = surface.call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", owner.value["id"]},
                                        {"name", "tree-objects"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call("create_repository",
                                         {{"customer_id", customer.value["id"]},
                                          {"workspace_id", surface.workspace(customer.value["id"])},
                                          {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const std::string repository_id = repository.value["id"];
    const auto object = surface.store.find_policy_object("repository", repository_id);
    ASSERT_TRUE(object);
    // The fixture's tree: readme.md (1111…), shared/ (2222…), shared/readme.md (3333…).
    const std::string top_file = repository_id + ":11111111111111111111111111111111";
    const std::string folder_node = repository_id + ":22222222222222222222222222222222";
    const std::string inner_file = repository_id + ":33333333333333333333333333333333";
    const auto folder = surface.store.create_policy_object(
        {"path", folder_node, object->id, "admin", "test", true});
    ASSERT_TRUE(folder);
    const std::string folder_admin = "taap:1:ed25519:folder-admin";
    ASSERT_TRUE(
        surface.store.create_grant({folder_admin, folder->id, "admin", "test", "admin", "test"}));
    const std::string repository_admin = "taap:1:ed25519:repository-admin";
    ASSERT_TRUE(surface.store.create_grant(
        {repository_admin, object->id, "admin", "test", "admin", "test"}));
    surface.ctx.operator_credential = false;
    const auto create =
        [&](const std::string& who, const std::string& id, const std::string& parent) {
            surface.ctx.acting_identity = who;
            surface.ctx.authorization_cache = {};
            return surface.call("create_object",
                                {{"object_type", "path"},
                                 {"external_id", id},
                                 {"parent_id", parent},
                                 {"inherit", false}});
        };
    // The folder administrator cannot claim a file beside the folder under it.
    EXPECT_EQ(create(folder_admin, top_file, folder->id).error.code, "invalid_input");
    // Nor a node that is not in the tree, nor a branch rule's id.
    EXPECT_EQ(create(folder_admin, repository_id + ":not-a-node", folder->id).error.code,
              "invalid_input");
    EXPECT_EQ(create(repository_admin, repository_id + ":refs/heads/main", object->id).error.code,
              "invalid_input");
    // The repository's administrator places the file inside the folder only under the folder.
    EXPECT_EQ(create(repository_admin, inner_file, object->id).error.code, "invalid_input");
    EXPECT_EQ(surface.store.find_policy_object("path", top_file).error().code, "not_found");
    // What lies in the folder is the folder administrator's to protect.
    const auto inner = create(folder_admin, inner_file, folder->id);
    EXPECT_TRUE(inner.ok) << inner.error.message;
    // The repository's administrator protects the file at the top, under the repository.
    const auto top = create(repository_admin, top_file, object->id);
    EXPECT_TRUE(top.ok) << top.error.message;
}

TEST(Drive, MutationChecksTheLockedTreeAndRestoreChecksMain) {
    Surface surface;
    RepositoryFixtureStore repositories;
    surface.ctx.sync_store = &repositories;
    const auto owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:writer"}, {"display_name", "Writer"}});
    ASSERT_TRUE(owner.ok);
    const auto customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "locked-tree"}});
    ASSERT_TRUE(customer.ok);
    const auto repository = surface.call("create_repository",
                                         {{"customer_id", customer.value["id"]},
                                          {"workspace_id", surface.workspace(customer.value["id"])},
                                          {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const auto object = surface.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(object);
    ASSERT_TRUE(surface.store.create_grant(
        {"taap:1:ed25519:writer", object->id, "write", "test", "admin", "test"}));
    // The ref moves after the registry reads it: main now contains a different, protected node.
    repositories.write_tree = *repositories.repository_tree("", "", "main");
    repositories.write_tree->back().node_id = "protected-file";
    const auto protected_file = surface.store.create_policy_object(
        {"path",
         repository.value["id"].get<std::string>() + ":protected-file",
         object->id,
         "admin",
         "test",
         false});
    ASSERT_TRUE(protected_file);
    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:writer";
    const json target{
        {"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}};
    json upload = target;
    upload["files"] = json::array({{{"name", "readme.md"}, {"content", "bmV3"}}});
    const auto denied = surface.call("upload_drive_files", upload);
    EXPECT_EQ(denied.error.code, "unauthorized");
    EXPECT_TRUE(repositories.committed_operations.empty());
    json restore = target;
    restore["path"] = "shared/readme.md";
    restore["ref"] = "change-1"; // A historical ref cannot authorize writes into today's main.
    restore["change_id"] = "change-1";
    const auto denied_restore = surface.call("restore_drive_version", restore);
    EXPECT_EQ(denied_restore.error.code, "unauthorized");
    EXPECT_FALSE(repositories.restored);
    repositories.conflict_path = "shared/readme.md";
    json conflict = restore;
    conflict.erase("change_id");
    conflict["conflict_id"] = "conflict-1";
    conflict["resolution"] = "left";
    EXPECT_EQ(surface.call("resolve_conflict", conflict).error.code, "unauthorized");
    EXPECT_TRUE(repositories.resolved_conflict.empty());

    auto contents = repository_store_contents(
        &repositories, customer.value["id"], repository.value["id"], "main", surface.ctx);
    const auto store_write = lake::control::settled(
        liquid::runtime::block_on(contents->write("shared/readme.md", "new", "text/plain")));
    ASSERT_FALSE(store_write);
    EXPECT_EQ(store_write.error().code, "unauthorized");
    EXPECT_TRUE(repositories.committed_operations.empty());
}

TEST(Drive, ShareDownloadsEncodeAcrossBlockBoundaries) {
    Surface surface;
    DriveWorkspaces workspaces(surface);
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult owner = surface.call("create_user", {{"display_name", "Owner"}});
    ASSERT_TRUE(owner.ok);
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "share-bytes"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const std::string workspace = workspaces.create(customer.value["id"], "home");
    const ApiResult link = surface.call("create_drive_share_link",
                                        {{"repository_id", repository.value["id"]},
                                         {"workspace_id", workspace},
                                         {"ref", "main"},
                                         {"path", "shared"},
                                         {"role", "read"},
                                         {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(link.ok) << link.error.message;
    std::string content(3 * 1024 * 1024 + 1, '\xff');
    content[3 * 1024 * 1024] = '\0';
    repository_store.blob_contents = content;
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    const ApiResult downloaded = surface.call(
        "download_drive_share_link",
        {{"token", link.value["token"]}, {"ref", "main"}, {"path", "shared/readme.md"}});
    ASSERT_TRUE(downloaded.ok) << downloaded.error.message;
    EXPECT_EQ(downloaded.value["size"], content.size());
    EXPECT_EQ(downloaded.value["content_base64"], base64_bytes(bytes(content)));
    EXPECT_EQ(surface.ctx.sync_response, bytes(content));
}

// lake#350: a link lives only in the workspace Zetta that holds it. Two spaces with two
// workspaces each; the token routes a signed-out recipient to that workspace and nowhere else.
TEST(Drive, ShareLinksLiveOnlyInTheWorkspaceThatHoldsThem) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    DriveWorkspaces workspaces(surface);
    struct Space {
        std::string owner, customer, repository;
        std::vector<std::string> workspaces;
    };
    std::vector<Space> spaces;
    for (const std::string name : {"first", "second"}) {
        Space space;
        space.owner = "taap:1:ed25519:" + name;
        const ApiResult user =
            surface.call("create_user", {{"taap_id", space.owner}, {"display_name", name}});
        ASSERT_TRUE(user.ok) << user.error.message;
        const ApiResult customer = surface.call(
            "create_customer",
            {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", name}});
        ASSERT_TRUE(customer.ok) << customer.error.message;
        space.customer = customer.value["id"];
        const ApiResult repository = surface.call(
            "create_repository",
            {{"customer_id", space.customer},
             {"workspace_id", lake::test::repository_workspace(surface.store, space.customer)},
             {"name", "drive"}});
        ASSERT_TRUE(repository.ok) << repository.error.message;
        space.repository = repository.value["id"];
        const auto object = surface.store.find_policy_object("repository", space.repository);
        ASSERT_TRUE(object);
        ASSERT_TRUE(surface.store.create_grant(
            {space.owner, object->id, "admin", "attestation-" + name, "admin", "request-" + name}));
        space.workspaces = {workspaces.create(space.customer, "home"),
                            workspaces.create(space.customer, "lab")};
        spaces.push_back(std::move(space));
    }
    const Space& first = spaces[0];
    const Space& second = spaces[1];
    surface.ctx.operator_credential = false;
    const auto as = [&](const Space& space) {
        surface.ctx.authorized = true;
        surface.ctx.acting_identity = space.owner;
    };
    const auto anonymous = [&] {
        surface.ctx.authorized = false;
        surface.ctx.acting_identity.clear();
    };
    const auto link_input = [](const Space& space, json workspace) {
        json input{{"repository_id", space.repository},
                   {"ref", "main"},
                   {"path", "shared"},
                   {"role", "read"},
                   {"expires_at", "2999-01-01T00:00:00Z"}};
        if (!workspace.is_null()) {
            input["workspace_id"] = workspace;
        }
        return input;
    };

    as(first);
    const ApiResult link = surface.call("create_drive_share_link", link_input(first, first.workspaces[0]));
    ASSERT_TRUE(link.ok) << link.error.message;
    EXPECT_EQ(link.value["workspace_id"], first.workspaces[0]);
    const std::string token = link.value["token"];
    const std::string link_id = link.value["id"];
    EXPECT_TRUE(token.starts_with(first.workspaces[0] + "." + link_id + "."))
        << "the token names the workspace and record that hold the link";
    // Stored only at its own workspace's endpoint, as a sealed record.
    for (const Space& space : spaces) {
        for (const std::string& id : space.workspaces) {
            EXPECT_EQ(workspaces.records(id).rows("drive_share_links").size(),
                      id == first.workspaces[0] ? 1U : 0U)
                << id;
        }
    }
    const std::string sealed =
        workspaces.records(first.workspaces[0]).rows("drive_share_links").at(link_id).dump();
    EXPECT_EQ(sealed.find(share_token_hash(token)), std::string::npos);
    EXPECT_EQ(sealed.find("shared"), std::string::npos);
    // Centrally only the synthetic grant names it.
    const auto grants = surface.store.list_grants({}, "share-link:" + link_id, 10);
    ASSERT_TRUE(grants);
    EXPECT_EQ(grants->size(), 1U);

    const auto listed = [&](const std::string& workspace) {
        return surface.call("drive_effective_access", {{"repository_id", first.repository},
                                                       {"workspace_id", workspace},
                                                       {"ref", "main"},
                                                       {"path", "shared"}});
    };
    ASSERT_EQ(listed(first.workspaces[0]).value["links"].size(), 1U);
    EXPECT_EQ(listed(first.workspaces[0]).value["links"][0]["id"], link_id);
    EXPECT_TRUE(listed(first.workspaces[1]).value["links"].empty())
        << "another workspace of the same space holds its own links";

    // Missing, malformed, unknown and foreign workspace ids name the field.
    for (const json& bad : {json(nullptr), json("not-a-workspace"),
                            json(lake::control::new_uuid_v4()), json(second.workspaces[0])}) {
        const ApiResult refused = surface.call("create_drive_share_link", link_input(first, bad));
        EXPECT_EQ(refused.error.code, "invalid_input") << bad;
        EXPECT_EQ(refused.error.path, "/workspace_id") << bad;
        if (!bad.is_null()) {
            const ApiResult listing = listed(bad.get<std::string>());
            EXPECT_EQ(listing.error.code, "invalid_input") << bad;
            EXPECT_EQ(listing.error.path, "/workspace_id") << bad;
        }
    }
    EXPECT_EQ(surface.call("revoke_drive_share_link", {{"link_id", link_id}}).error.path,
              "/workspace_id");

    // Least privilege: the other space's owner can neither make, list nor revoke this folder's
    // links, whichever workspace it names.
    as(second);
    EXPECT_EQ(surface.call("create_drive_share_link", link_input(first, second.workspaces[0]))
                  .error.code,
              "unauthorized");
    EXPECT_EQ(listed(first.workspaces[0]).error.code, "unauthorized");
    for (const std::string& workspace : {first.workspaces[0], second.workspaces[0]}) {
        EXPECT_EQ(surface
                      .call("revoke_drive_share_link",
                            {{"workspace_id", workspace}, {"link_id", link_id}})
                      .error.code,
                  "unauthorized");
    }

    // A signed-out recipient with only the token reaches the link; the same link id under another
    // workspace, a wrong secret, or a sealed copy moved to another workspace does not open.
    anonymous();
    const auto resolve = [&](const std::string& presented) {
        return surface.call("resolve_drive_share_link", {{"token", presented}, {"ref", "main"}});
    };
    ASSERT_TRUE(resolve(token).ok) << resolve(token).error.message;
    const std::string secret = token.substr(token.rfind('.') + 1);
    for (const std::string& forged :
         {lake::control::drive_share_token(first.workspaces[1], link_id, secret),
          lake::control::drive_share_token(second.workspaces[0], link_id, secret),
          lake::control::drive_share_token(first.workspaces[0], link_id, "wrong"),
          lake::control::drive_share_token(first.workspaces[0], lake::control::new_uuid_v4(), secret),
          std::string("no-such-legacy-token")}) {
        const ApiResult refused = resolve(forged);
        EXPECT_EQ(refused.error.code, "not_found") << forged;
        EXPECT_EQ(refused.error.path, "/token") << forged;
    }
    workspaces.records(first.workspaces[1]).collections["lake_drive_share_links"] =
        workspaces.records(first.workspaces[0]).collections["lake_drive_share_links"];
    EXPECT_EQ(resolve(lake::control::drive_share_token(first.workspaces[1], link_id, secret))
                  .error.code,
              "not_found")
        << "a sealed record copied to another workspace does not open there";

    // Revocation is immutable ownership too: only the holding workspace revokes it.
    as(first);
    const ApiResult elsewhere = surface.call(
        "revoke_drive_share_link", {{"workspace_id", first.workspaces[1]}, {"link_id", link_id}});
    EXPECT_EQ(elsewhere.error.code, "not_found");
    EXPECT_EQ(elsewhere.error.path, "/link_id");
    anonymous();
    EXPECT_TRUE(resolve(token).ok);

    // A workspace that is not ready refuses instead of falling back anywhere.
    workspaces.status(first.workspaces[0], "Provisioning");
    EXPECT_EQ(resolve(token).error.code, "workspace_not_ready");
    workspaces.ready(first.workspaces[0]);
    ASSERT_TRUE(resolve(token).ok);

    as(first);
    const ApiResult revoked = surface.call(
        "revoke_drive_share_link", {{"workspace_id", first.workspaces[0]}, {"link_id", link_id}});
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_FALSE(revoked.value["revoked_at"].is_null());
    const ApiResult again = surface.call(
        "revoke_drive_share_link", {{"workspace_id", first.workspaces[0]}, {"link_id", link_id}});
    EXPECT_EQ(again.error.code, "conflict");
    EXPECT_EQ(listed(first.workspaces[0]).value["links"][0]["active"], false);
    anonymous();
    EXPECT_EQ(resolve(token).error.code, "unauthorized");

    // Deleting the workspace takes its links with it.
    as(first);
    const ApiResult live = surface.call("create_drive_share_link", link_input(first, first.workspaces[0]));
    ASSERT_TRUE(live.ok) << live.error.message;
    ASSERT_TRUE(surface.store.update_workspace(first.workspaces[0], {.desired = "deleted"}));
    EXPECT_EQ(listed(first.workspaces[0]).error.path, "/workspace_id");
    anonymous();
    EXPECT_EQ(resolve(live.value["token"]).error.code, "not_found");
}

TEST(Drive, PathHistoryRestoresAsANewChange) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "history"}});
    ASSERT_TRUE(repository.ok);
    const ApiResult history = surface.call(
        "drive_history",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(history.ok) << history.error.message;
    ASSERT_EQ(history.value["items"].size(), 1u);
    EXPECT_EQ(history.value["items"][0]["id"], "change-1");
    const ApiResult restored = surface.call("restore_drive_version",
                                            {{"repository_id", repository.value["id"]},
                                             {"ref", "main"},
                                             {"path", "readme.md"},
                                             {"change_id", "change-1"}});
    ASSERT_TRUE(restored.ok) << restored.error.message;
    EXPECT_EQ(restored.value["id"], "restored-change");
    EXPECT_TRUE(repository_store.restored);
}

TEST(Drive, BrowseMutationsDownloadsDiffsAndConflictsUseRepositoryCapabilities) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "drive"}});
    ASSERT_TRUE(repository.ok);

    const ApiResult browse = surface.call("drive_browse",
                                          {{"repository_id", repository.value["id"]},
                                           {"ref", "main"},
                                           {"path", ""},
                                           {"sort", "modified"},
                                           {"direction", "desc"}});
    ASSERT_TRUE(browse.ok) << browse.error.message;
    EXPECT_EQ(browse.value["items"].front()["kind"], "tree");
    EXPECT_EQ(browse.value["items"][1]["modified_at"], "2026-09-04T00:00:00Z");

    const ApiResult folder = surface.call("create_drive_folder",
                                          {{"repository_id", repository.value["id"]},
                                           {"ref", "main"},
                                           {"path", ""},
                                           {"name", "notes"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    ASSERT_EQ(repository_store.committed_operations.size(), 1u);
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::Folder);
    EXPECT_EQ(repository_store.committed_operations.front().path, "notes");

    const ApiResult upload =
        surface.call("upload_drive_files",
                     {{"repository_id", repository.value["id"]},
                      {"ref", "main"},
                      {"path", "shared"},
                      {"files", json::array({{{"name", "note.txt"}, {"content", "aGVsbG8="}}})}});
    ASSERT_TRUE(upload.ok) << upload.error.message;
    ASSERT_EQ(repository_store.committed_operations.size(), 1u);
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::File);
    EXPECT_EQ(repository_store.committed_operations.front().path, "shared/note.txt");
    EXPECT_EQ(repository_store.committed_operations.front().contents, bytes("hello"));

    const ApiResult moved = surface.call("move_drive_path",
                                         {{"repository_id", repository.value["id"]},
                                          {"ref", "main"},
                                          {"path", "readme.md"},
                                          {"destination", "renamed.md"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::Moved);
    EXPECT_EQ(repository_store.committed_operations.front().source_path, "readme.md");
    EXPECT_EQ(repository_store.committed_operations.front().path, "renamed.md");

    const ApiResult deleted = surface.call(
        "delete_drive_path",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::DeletedFile);

    const ApiResult download = surface.call(
        "download_drive_file",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(download.ok) << download.error.message;
    EXPECT_EQ(download.value["blob"], "blob-2");
    EXPECT_EQ(surface.ctx.sync_response, bytes("new\nshared\n"));

    const ApiResult difference = surface.call("diff_drive_versions",
                                              {{"repository_id", repository.value["id"]},
                                               {"ref", "main"},
                                               {"path", "readme.md"},
                                               {"left", "change-1"},
                                               {"right", "main"}});
    ASSERT_TRUE(difference.ok) << difference.error.message;
    EXPECT_EQ(difference.value["lines"][0]["kind"], "removed");
    EXPECT_EQ(difference.value["lines"][0]["text"], "old");
    EXPECT_EQ(difference.value["lines"][1]["kind"], "added");
    EXPECT_EQ(difference.value["lines"][1]["text"], "new");

    const ApiResult conflict = surface.call(
        "drive_conflict",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(conflict.ok) << conflict.error.message;
    EXPECT_EQ(conflict.value["versions"].size(), 2u);
    EXPECT_EQ(conflict.value["difference"]["lines"][0]["kind"], "removed");
    EXPECT_EQ(conflict.value["conflict"]["kind"], "suspicious-merge");
    EXPECT_EQ(conflict.value["conflict"]["region"]["bytes"]["length"], 3u);

    const ApiResult resolved = surface.call("resolve_conflict",
                                            {{"repository_id", repository.value["id"]},
                                             {"ref", "main"},
                                             {"path", "readme.md"},
                                             {"conflict_id", "conflict-1"},
                                             {"resolution", "blob"},
                                             {"blob", "cmVzb2x2ZWQ="}});
    ASSERT_TRUE(resolved.ok) << resolved.error.message;
    EXPECT_EQ(resolved.value["conflict"], "conflict-1");
    EXPECT_EQ(resolved.value["value"], "blob-resolved");
    EXPECT_EQ(resolved.value["change"], "change-resolved");
    EXPECT_EQ(repository_store.resolved_conflict, "conflict-1");
    EXPECT_EQ(repository_store.conflict_resolution, "blob");
    EXPECT_EQ(repository_store.conflict_blob, bytes("resolved"));
}

TEST(Models, MetadataAndResolveUseRepositoryHistoryAndProvenance) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    const ApiResult repository = surface.call(
        "create_repository",
        {{"customer_id", customer.value["id"]},
         {"workspace_id", lake::test::repository_workspace(surface.store, customer.value["id"])},
         {"name", "fixture-model"},
         {"visibility", "public"}});
    ASSERT_TRUE(repository.ok);
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    const ApiResult model =
        surface.call("get_model", {{"id", repository.value["id"]}, {"revision", "main"}});
    ASSERT_TRUE(model.ok) << model.error.message;
    EXPECT_EQ(model.value["card"], "# Fixture model");
    EXPECT_TRUE(model.value["provenance_complete"]);
    EXPECT_TRUE(model.value["provenance"][0]["sbom"]);
    EXPECT_TRUE(model.value["provenance"][0]["slsa_provenance"]);
    const ApiResult file = surface.call(
        "resolve_model_file",
        {{"id", repository.value["id"]}, {"revision", "main"}, {"path", "weights.bin"}});
    ASSERT_TRUE(file.ok) << file.error.message;
    EXPECT_EQ(file.value["contents"], "fixture weights");
}

TEST(Models, AnonymousDatasetAndModelRevisionsRequirePublicRepositories) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const auto user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const auto customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    for (const std::string visibility : {"public", "private"}) {
        surface.ctx.authorized = true;
        surface.ctx.acting_identity = "admin";
        const auto repository =
            surface.call("create_repository",
                         {{"customer_id", customer.value["id"]},
                          {"workspace_id",
                           lake::test::repository_workspace(surface.store, customer.value["id"])},
                          {"name", visibility + "-metadata"},
                          {"visibility", visibility}});
        ASSERT_TRUE(repository.ok) << repository.error.message;
        surface.ctx.authorized = false;
        surface.ctx.acting_identity.clear();
        for (const std::string name : {"get_dataset", "get_model"}) {
            SCOPED_TRACE(name + "/" + visibility);
            const json input{{"id", repository.value["id"]}, {"revision", "main"}};
            const auto sync = surface.registry.call(name, input, surface.ctx);
            const auto async =
                liquid::runtime::block_on(surface.registry.call_async(name, input, surface.ctx));
            ASSERT_TRUE(async);
            for (const auto* result : {&sync, &*async}) {
                EXPECT_EQ(result->ok, visibility == "public") << result->error.message;
                if (visibility == "private") {
                    EXPECT_EQ(result->error.code, "unauthenticated");
                }
            }
        }
    }
}

TEST(HuggingFaceHttp, CommonModelAndDatasetCallsUseRepositoryCapabilities) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user =
        control->create_user({"taap:1:ed25519:hf-owner", "hf-owner@example.test", "Hub Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "fixture-model", "", "public",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "hub-test";
    options.admin_token = "hub-admin";
    options.instance_kek = "hub-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer hub-admin";

    const std::string preupload = http_request(server.port(),
                                               "POST",
                                               "/api/models/ada/fixture-model/preupload/main",
                                               R"({"files":[{"path":"config.json","size":12}]})",
                                               "application/json",
                                               {},
                                               authorization);
    EXPECT_NE(preupload.find(" 200 "), std::string::npos) << preupload;
    EXPECT_EQ(response_json(preupload)["files"][0]["uploadMode"], "regular");

    // huggingface_hub sends `gitIgnore` whenever a commit adds a .gitignore; the strict write
    // schema never sees it (lake#534).
    const std::string ignored = http_request(
        server.port(), "POST", "/api/models/ada/fixture-model/preupload/main",
        R"({"files":[{"path":".gitignore","size":6,"sample":"Ki5sb2cK"}],"gitIgnore":"*.log\n"})",
        "application/json", {}, authorization);
    EXPECT_NE(ignored.find(" 200 "), std::string::npos) << ignored;
    EXPECT_EQ(response_json(ignored)["files"][0]["path"], ".gitignore");

    const std::string config = "{\"hidden\":8}";
    const std::string commit_body = R"({"key":"header","value":{"summary":"initial model"}})"
                                    "\n" +
                                    nlohmann::json({{"key", "file"},
                                                    {"value",
                                                     {{"path", "config.json"},
                                                      {"content", base64_bytes(bytes(config))},
                                                      {"encoding", "base64"}}}})
                                        .dump() +
                                    "\n";
    const std::string committed = http_request(server.port(),
                                               "POST",
                                               "/api/models/ada/fixture-model/commit/main",
                                               commit_body,
                                               "application/x-ndjson",
                                               {},
                                               authorization);
    EXPECT_NE(committed.find(" 200 "), std::string::npos) << committed;
    const std::string revision = response_json(committed).value("commitOid", "");
    EXPECT_FALSE(revision.empty());

    const std::string info = http_request(server.port(),
                                          "GET",
                                          "/api/models/ada/fixture-model/revision/" + revision,
                                          "",
                                          "application/json");
    EXPECT_NE(info.find(" 200 "), std::string::npos) << info;
    EXPECT_EQ(response_json(info)["siblings"][0]["rfilename"], "config.json");
    const std::string dataset_info = http_request(
        server.port(), "GET", "/api/datasets/ada/fixture-model", "", "application/json");
    EXPECT_NE(dataset_info.find(" 200 "), std::string::npos) << dataset_info;
    const std::string tree = http_request(server.port(),
                                          "GET",
                                          "/api/models/ada/fixture-model/tree/main?recursive=true",
                                          "",
                                          "application/json");
    EXPECT_EQ(response_json(tree)[0]["path"], "config.json") << tree;
    const std::string download = http_request(server.port(),
                                              "GET",
                                              "/ada/fixture-model/resolve/main/config.json",
                                              "",
                                              "application/octet-stream");
    EXPECT_NE(download.find(config), std::string::npos) << download;
    const std::string head = http_request(server.port(),
                                          "HEAD",
                                          "/ada/fixture-model/resolve/main/config.json",
                                          "",
                                          "application/octet-stream");
    EXPECT_NE(head.find("X-Repo-Commit: " + revision), std::string::npos) << head;

    const std::string large(6U * 1024U * 1024U, 'w');
    const std::string oid = lake::control::identity_crypto::sha256_hex(large);
    const std::string batch = http_request(
        server.port(),
        "POST",
        "/ada/fixture-model.git/info/lfs/objects/batch",
        nlohmann::json(
            {{"operation", "upload"},
             {"transfers", nlohmann::json::array({"basic"})},
             {"objects", nlohmann::json::array({{{"oid", oid}, {"size", large.size()}}})}})
            .dump(),
        "application/vnd.git-lfs+json",
        {},
        authorization);
    EXPECT_NE(batch.find(" 200 "), std::string::npos) << batch;
    const std::string href = response_json(batch)["objects"][0]["actions"]["upload"]["href"];
    const std::size_t path_offset = href.find('/', href.find("//") + 2);
    ASSERT_NE(path_offset, std::string::npos);
    const std::string uploaded = http_request(
        server.port(), "PUT", href.substr(path_offset), large, "application/octet-stream");
    EXPECT_NE(uploaded.find(" 200 "), std::string::npos) << uploaded;
    const std::string lfs_commit =
        R"({"key":"header","value":{"summary":"weights"}})"
        "\n" +
        nlohmann::json({{"key", "lfsFile"},
                        {"value", {{"path", "weights.bin"}, {"oid", oid}, {"size", large.size()}}}})
            .dump() +
        "\n";
    const std::string lfs_committed = http_request(server.port(),
                                                   "POST",
                                                   "/api/models/ada/fixture-model/commit/main",
                                                   lfs_commit,
                                                   "application/x-ndjson",
                                                   {},
                                                   authorization);
    EXPECT_NE(lfs_committed.find(" 200 "), std::string::npos) << lfs_committed;
    // The model page's reads: the Hub name resolves to its repository, which get_model reads.
    const std::string resolved = http_request(server.port(),
                                              "GET",
                                              "/v1/resolve_model?namespace=ada&name=fixture-model",
                                              "",
                                              "text/plain",
                                              {},
                                              authorization);
    ASSERT_NE(resolved.find(" 200 "), std::string::npos) << resolved;
    const std::string model_page = http_request(
        server.port(),
        "GET",
        "/v1/get_model?id=" + response_json(resolved)["value"]["repository_id"].get<std::string>() +
            "&revision=" + response_json(resolved)["value"]["revision"].get<std::string>(),
        "",
        "text/plain",
        {},
        authorization);
    EXPECT_NE(model_page.find(" 200 "), std::string::npos) << model_page;
    EXPECT_NE(model_page.find("weights.bin"), std::string::npos) << model_page;

    // A repository belongs to one workspace (lake#315): named in the body, or for a hub client
    // that cannot add body fields, in the X-Lake-Workspace-Id header. There is no default.
    const std::string missing_workspace = http_request(
        server.port(),
        "POST",
        "/api/repos/create",
        R"({"name":"no-workspace","organization":"ada","type":"dataset","private":false})",
        "application/json",
        {},
        authorization);
    EXPECT_EQ(missing_workspace.find(" 200 "), std::string::npos) << missing_workspace;
    EXPECT_NE(missing_workspace.find("workspace_id must name"), std::string::npos)
        << missing_workspace;
    const std::string created = http_request(
        server.port(),
        "POST",
        "/api/repos/create",
        json{{"name", "new-dataset"},
             {"organization", "ada"},
             {"type", "dataset"},
             {"private", false},
             {"workspace_id", repository->workspace_id}}
            .dump(),
        "application/json",
        {},
        authorization);
    EXPECT_NE(created.find(" 200 "), std::string::npos) << created;
    EXPECT_EQ(response_json(created)["repo_id"], "ada/new-dataset");
    const std::string from_header = http_request(
        server.port(),
        "POST",
        "/api/repos/create",
        R"({"name":"header-dataset","organization":"ada","type":"dataset","private":false})",
        "application/json",
        {},
        authorization,
        "localhost",
        {{"X-Lake-Workspace-Id", repository->workspace_id}});
    EXPECT_NE(from_header.find(" 200 "), std::string::npos) << from_header;
    const std::string yaml = http_request(server.port(),
                                          "POST",
                                          "/api/validate-yaml",
                                          R"({"content":"---\n---\n","repoType":"model"})",
                                          "application/json",
                                          {},
                                          authorization);
    EXPECT_NE(yaml.find(" 200 "), std::string::npos) << yaml;
    EXPECT_TRUE(response_json(yaml)["errors"].empty());
    server.stop();
}

TEST(HuggingFaceClient, CommonFlowsRunWhenPythonClientIsInstalled) {
    if (std::system("python3 -c 'import huggingface_hub' >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "huggingface_hub is not installed; direct HTTP coverage remains active";
    }
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:client-owner", "client-owner@example.test", "Client Owner"});
    ASSERT_TRUE(user);
    const auto customer = control->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer);
    const std::string workspace = lake::test::repository_workspace(*control, customer->id);
    ServerOptions options;
    options.port = 0;
    options.node_name = "huggingface-client-test";
    options.admin_token = "hub-admin";
    options.instance_kek = "huggingface-client-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::filesystem::path script = temporary.path() / "huggingface_client_test.py";
    {
        std::ofstream output(script);
        output << R"py(import os
import pathlib
import sys
from huggingface_hub import HfApi, hf_hub_download, snapshot_download

endpoint = sys.argv[1]
root = pathlib.Path(sys.argv[2])
# create_repo names the workspace the repository belongs to (lake#315) in a header.
api = HfApi(endpoint=endpoint, token="hub-admin", headers={"X-Lake-Workspace-Id": sys.argv[3]})
api.create_repo("ada/client-model", repo_type="model", private=False)
api.upload_file(
    path_or_fileobj=b"model card",
    path_in_repo="README.md",
    repo_id="ada/client-model",
    repo_type="model",
)
folder = root / "folder"
folder.mkdir()
(folder / "config.json").write_text('{"layers": 2}')
(folder / "tokenizer.json").write_text('{"tokens": []}')
api.upload_folder(
    folder_path=folder,
    repo_id="ada/client-model",
    repo_type="model",
)
files = api.list_repo_files("ada/client-model", repo_type="model")
assert set(files) == {"README.md", "config.json", "tokenizer.json"}, files
info = api.model_info("ada/client-model")
assert info.sha
download = hf_hub_download(
    repo_id="ada/client-model",
    filename="config.json",
    endpoint=endpoint,
    token="hub-admin",
)
assert pathlib.Path(download).read_text() == '{"layers": 2}'
immutable = hf_hub_download(
    repo_id="ada/client-model",
    filename="tokenizer.json",
    revision=info.sha,
    endpoint=endpoint,
    token="hub-admin",
)
assert pathlib.Path(immutable).read_text() == '{"tokens": []}'
snapshot = snapshot_download(
    repo_id="ada/client-model",
    endpoint=endpoint,
    token="hub-admin",
)
assert (pathlib.Path(snapshot) / "README.md").read_bytes() == b"model card"
api.create_repo("ada/client-dataset", repo_type="dataset", private=False)
api.upload_file(
    path_or_fileobj=b'{"row": 1}\n',
    path_in_repo="data.jsonl",
    repo_id="ada/client-dataset",
    repo_type="dataset",
)
assert api.list_repo_files("ada/client-dataset", repo_type="dataset") == ["data.jsonl"]
dataset_download = hf_hub_download(
    repo_id="ada/client-dataset",
    filename="data.jsonl",
    repo_type="dataset",
    endpoint=endpoint,
    token="hub-admin",
)
assert pathlib.Path(dataset_download).read_bytes() == b'{"row": 1}\n'
dataset_snapshot = snapshot_download(
    repo_id="ada/client-dataset",
    repo_type="dataset",
    endpoint=endpoint,
    token="hub-admin",
)
assert (pathlib.Path(dataset_snapshot) / "data.jsonl").read_bytes() == b'{"row": 1}\n'
)py";
    }
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string command = "HF_HUB_DISABLE_XET=1 HF_HUB_DISABLE_TELEMETRY=1 "
                                "python3 '" +
                                script.string() + "' '" + endpoint + "' '" +
                                temporary.path().string() + "' '" + workspace + "'";
    EXPECT_EQ(std::system(command.c_str()), 0);
    server.stop();
}

TEST(ModelProvenance, SignedDatasetChainVerifiesAndTamperingIsRefused) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user =
        control->create_user({"taap:1:ed25519:trainer", "trainer@example.test", "Trainer"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "training"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto dataset =
        control->create_repository({customer->id, "dataset", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    const auto model =
        control->create_repository({customer->id, "model", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(dataset);
    ASSERT_TRUE(model);

    auto dataset_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "dataset"));
    auto model_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "model"));
    ASSERT_TRUE(dataset_source);
    ASSERT_TRUE(model_source);
    ASSERT_TRUE(liquid::runtime::block_on(
        dataset_source->write("training.jsonl", bytes("{\"example\":1}\n"))));
    const auto dataset_change = liquid::runtime::block_on(dataset_source->cut("dataset"));
    ASSERT_TRUE(dataset_change);
    ASSERT_TRUE(
        liquid::runtime::block_on(model_source->write("weights.bin", bytes("trained weights"))));
    const auto model_change = liquid::runtime::block_on(model_source->cut("model"));
    ASSERT_TRUE(model_change);

    ServerOptions options;
    options.port = 0;
    options.node_name = "provenance-test";
    options.admin_token = "provenance-admin";
    options.instance_kek = "provenance-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    liquid::sync_endpoint dataset_endpoint(
        origin + "/v1/repositories/" + dataset->id,
        liquid::sync_endpoint_options{.bearer_token = "provenance-admin"});
    liquid::sync_endpoint model_endpoint(
        origin + "/v1/repositories/" + model->id,
        liquid::sync_endpoint_options{.bearer_token = "provenance-admin"});
    ASSERT_TRUE(liquid::runtime::block_on(dataset_endpoint.push(*dataset_source)));
    ASSERT_TRUE(liquid::runtime::block_on(model_endpoint.push(*model_source)));

    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    liquid::attestation model_statement{
        .subject = model_change->value,
        .profile = "model_provenance/1",
        .at = at,
        .subject_kind = liquid::attestation_subject_kind::model,
        .attributes = {{"training_dataset", liquid::to_string(dataset_change->value)},
                       {"training_code", liquid::to_string(dataset_change->value)}}};
    model_statement = sign_attestation(std::move(model_statement));
    const liquid::slsa_resource_descriptor resource{
        "liquid:training", {{"blake3", liquid::to_string(dataset_change->value)}}, std::nullopt};
    liquid::attestation run_statement{
        .subject = model_change->value,
        .profile = "slsa_provenance/1",
        .at = at,
        .subject_kind = liquid::attestation_subject_kind::artifact,
        .slsa_provenance = liquid::slsa_provenance_v1{
            .build_definition = {.build_type = "https://lake.example.com/training/v1",
                                 .external_parameters = {{"train", "--epochs", "3"},
                                                         "/workspace",
                                                         {{"CUDA", "12.8"}}},
                                 .internal_parameters = {.toolchain = resource,
                                                         .sandbox = resource,
                                                         .platform = {{"os", "linux"}}},
                                 .resolved_dependencies = {resource}},
            .run_details = {.builder = {"taap:1:ed25519:trainer"},
                            .metadata = {"run-7", "2026-09-05T00:00:00Z", "2026-09-05T00:01:00Z"},
                            .byproducts = {resource}}}};
    run_statement = sign_attestation(std::move(run_statement));
    const auto model_verification = liquid::verify_attestation(model_statement);
    const auto run_verification = liquid::verify_attestation(run_statement);
    ASSERT_TRUE(model_verification);
    ASSERT_TRUE(run_verification);
    EXPECT_TRUE(model_verification->verified);
    EXPECT_TRUE(run_verification->verified);
    const auto model_encoded = liquid::encode_attestation(model_statement);
    const auto run_encoded = liquid::encode_attestation(run_statement);
    ASSERT_TRUE(model_encoded);
    ASSERT_TRUE(run_encoded);

    const nlohmann::json request{{"id", model->id},
                                 {"revision", "main"},
                                 {"dataset_repository_id", dataset->id},
                                 {"dataset_revision", "main"},
                                 {"model_attestation", base64_bytes(*model_encoded)},
                                 {"run_attestation", base64_bytes(*run_encoded)}};
    const std::string attached = http_request(server.port(),
                                              "POST",
                                              "/v1/attach_model_provenance",
                                              request.dump(),
                                              "application/json",
                                              {},
                                              "Bearer provenance-admin");
    EXPECT_NE(attached.find(" 200 "), std::string::npos) << attached;
    EXPECT_TRUE(response_json(attached)["ok"]);
    EXPECT_TRUE(response_json(attached)["value"]["verified"]);
    const std::string verified =
        http_request(server.port(),
                     "POST",
                     "/v1/verify_model_provenance",
                     nlohmann::json({{"id", model->id}, {"revision", "main"}}).dump(),
                     "application/json",
                     {},
                     "Bearer provenance-admin");
    EXPECT_NE(verified.find(" 200 "), std::string::npos) << verified;
    EXPECT_TRUE(response_json(verified)["value"]["verified"]);
    EXPECT_EQ(response_json(verified)["value"]["dataset_revision"],
              liquid::to_string(dataset_change->value));
    const std::string model_page = http_request(server.port(),
                                                "GET",
                                                "/v1/get_model?id=" + model->id + "&revision=main",
                                                "",
                                                "text/plain",
                                                {},
                                                "Bearer provenance-admin");
    EXPECT_NE(model_page.find("run-7"), std::string::npos) << model_page;
    EXPECT_NE(model_page.find(liquid::to_string(dataset_change->value)), std::string::npos)
        << model_page;

    std::vector<std::byte> tampered = *model_encoded;
    tampered.back() ^= std::byte{1};
    nlohmann::json tampered_request = request;
    tampered_request["model_attestation"] = base64_bytes(tampered);
    const std::string refused = http_request(server.port(),
                                             "POST",
                                             "/v1/attach_model_provenance",
                                             tampered_request.dump(),
                                             "application/json",
                                             {},
                                             "Bearer provenance-admin");
    EXPECT_NE(refused.find(" 400 "), std::string::npos) << refused;
    server.stop();
}

// A person's key in a LIQUID_HOME of its own, so Lake's process identity store never holds it.
struct ForeignSigner {
    liquid::signer signer;
    std::string document; // base64url canonical CBOR, as `liquid identity show --document` prints
};

ForeignSigner foreign_signer_at(const std::filesystem::path& home) {
    const char* previous = std::getenv("LIQUID_HOME");
    const std::optional<std::string> saved =
        previous == nullptr ? std::nullopt : std::optional<std::string>{previous};
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (saved) {
        setenv("LIQUID_HOME", saved->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    EXPECT_TRUE(signer) << (signer ? "" : signer.error().message);
    const auto document = signer->document();
    EXPECT_TRUE(document);
    const auto encoded = liquid::encode_identity_document(*document);
    EXPECT_TRUE(encoded);
    const std::string raw(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    return ForeignSigner{std::move(*signer), lake::control::identity_crypto::base64url_encode(raw)};
}

liquid::attestation sign_attestation_as(liquid::signer& signer, liquid::attestation value) {
    value.issuer = signer.identity();
    auto body = liquid::objects::encode_attestation_body(value);
    EXPECT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer.sign("attestation", *body));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    value.signature = std::move(*signature);
    return value;
}

// lake#1020: a provenance chain a registered person signed verifies through the identity document
// they registered with Lake (`identity_documents`), which Lake passes to liquid as the issuer's
// chain; the same chain signed by an identity no account registered is refused as an unknown
// signer.
TEST(ModelProvenance, ARegisteredIssuerVerifiesThroughTheirDocumentAndAnUnknownOneDoesNot) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    ForeignSigner trainer = foreign_signer_at(temporary.path() / "trainer-home");
    ForeignSigner stranger = foreign_signer_at(temporary.path() / "stranger-home");

    lake::control::NewPasswordUser sign_up;
    sign_up.pool_id = "lake";
    sign_up.email = "registered-trainer@example.test";
    sign_up.password = "correct horse battery staple";
    sign_up.display_name = "Trainer";
    sign_up.device_name = "laptop";
    sign_up.identity_document = trainer.document;
    const auto session = control->sign_up_password(sign_up);
    ASSERT_TRUE(session) << session.error().message;
    const auto customer = lake::test::personal_customer(*control, session->session.user_id);
    ASSERT_TRUE(customer) << customer.error().message;

    // The store answers the registered document, and nothing for the stranger.
    const auto registered =
        control->registered_identity_document(trainer.signer.identity().taap_id);
    ASSERT_TRUE(registered) << registered.error().message;
    EXPECT_EQ(*registered, trainer.document);
    const auto unknown = control->registered_identity_document(stranger.signer.identity().taap_id);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, "not_found");

    const auto dataset =
        control->create_repository({customer->id,
                                    "registered-dataset",
                                    "",
                                    "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    const auto model =
        control->create_repository({customer->id,
                                    "registered-model",
                                    "",
                                    "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(dataset);
    ASSERT_TRUE(model);
    auto dataset_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "dataset"));
    auto model_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "model"));
    ASSERT_TRUE(dataset_source);
    ASSERT_TRUE(model_source);
    ASSERT_TRUE(liquid::runtime::block_on(
        dataset_source->write("training.jsonl", bytes("{\"example\":1}\n"))));
    const auto dataset_change = liquid::runtime::block_on(dataset_source->cut("dataset"));
    ASSERT_TRUE(dataset_change);
    ASSERT_TRUE(
        liquid::runtime::block_on(model_source->write("weights.bin", bytes("trained weights"))));
    const auto model_change = liquid::runtime::block_on(model_source->cut("model"));
    ASSERT_TRUE(model_change);

    ServerOptions options;
    options.port = 0;
    options.node_name = "issuer-chain-test";
    options.admin_token = "issuer-chain-admin";
    options.instance_kek = "issuer-chain-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    liquid::sync_endpoint dataset_endpoint(
        origin + "/v1/repositories/" + dataset->id,
        liquid::sync_endpoint_options{.bearer_token = "issuer-chain-admin"});
    liquid::sync_endpoint model_endpoint(
        origin + "/v1/repositories/" + model->id,
        liquid::sync_endpoint_options{.bearer_token = "issuer-chain-admin"});
    ASSERT_TRUE(liquid::runtime::block_on(dataset_endpoint.push(*dataset_source)));
    ASSERT_TRUE(liquid::runtime::block_on(model_endpoint.push(*model_source)));

    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto chain_signed_by = [&](liquid::signer& signer) {
        liquid::attestation model_statement{
            .subject = model_change->value,
            .profile = "model_provenance/1",
            .at = at,
            .subject_kind = liquid::attestation_subject_kind::model,
            .attributes = {{"training_dataset", liquid::to_string(dataset_change->value)},
                           {"training_code", liquid::to_string(dataset_change->value)}}};
        const liquid::slsa_resource_descriptor resource{
            "liquid:training",
            {{"blake3", liquid::to_string(dataset_change->value)}},
            std::nullopt};
        liquid::attestation run_statement{
            .subject = model_change->value,
            .profile = "slsa_provenance/1",
            .at = at,
            .subject_kind = liquid::attestation_subject_kind::artifact,
            .slsa_provenance = liquid::slsa_provenance_v1{
                .build_definition = {.build_type = "https://lake.orama.com/training/v1",
                                     .external_parameters = {{"train", "--epochs", "3"},
                                                             "/workspace",
                                                             {{"CUDA", "12.8"}}},
                                     .internal_parameters = {.toolchain = resource,
                                                             .sandbox = resource,
                                                             .platform = {{"os", "linux"}}},
                                     .resolved_dependencies = {resource}},
                .run_details = {
                    .builder = {signer.identity().taap_id},
                    .metadata = {"run-1020", "2026-11-14T00:00:00Z", "2026-11-14T00:01:00Z"},
                    .byproducts = {resource}}}};
        const auto model_encoded =
            liquid::encode_attestation(sign_attestation_as(signer, std::move(model_statement)));
        const auto run_encoded =
            liquid::encode_attestation(sign_attestation_as(signer, std::move(run_statement)));
        EXPECT_TRUE(model_encoded);
        EXPECT_TRUE(run_encoded);
        return nlohmann::json{{"id", model->id},
                              {"revision", "main"},
                              {"dataset_repository_id", dataset->id},
                              {"dataset_revision", "main"},
                              {"model_attestation", base64_bytes(*model_encoded)},
                              {"run_attestation", base64_bytes(*run_encoded)}};
    };

    const std::string strange = http_request(server.port(),
                                             "POST",
                                             "/v1/attach_model_provenance",
                                             chain_signed_by(stranger.signer).dump(),
                                             "application/json",
                                             {},
                                             "Bearer issuer-chain-admin");
    EXPECT_NE(strange.find(" 400 "), std::string::npos) << strange;
    EXPECT_NE(strange.find("unknown signer"), std::string::npos) << strange;

    const std::string attached = http_request(server.port(),
                                              "POST",
                                              "/v1/attach_model_provenance",
                                              chain_signed_by(trainer.signer).dump(),
                                              "application/json",
                                              {},
                                              "Bearer issuer-chain-admin");
    EXPECT_NE(attached.find(" 200 "), std::string::npos) << attached;
    EXPECT_TRUE(response_json(attached)["value"]["verified"]) << attached;
    const std::string verified =
        http_request(server.port(),
                     "POST",
                     "/v1/verify_model_provenance",
                     nlohmann::json({{"id", model->id}, {"revision", "main"}}).dump(),
                     "application/json",
                     {},
                     "Bearer issuer-chain-admin");
    EXPECT_NE(verified.find(" 200 "), std::string::npos) << verified;
    EXPECT_TRUE(response_json(verified)["value"]["verified"]) << verified;
    server.stop();
}

TEST(RepositoryHttp, PublicRepositoriesAreReadableAndPrivateRepositoriesAreHidden) {
    TemporaryRepositoryRoot root;
    const auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"", "", "Ada"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer);
    const auto public_repository =
        store->create_repository({customer->id, "public-repository", "", "public",
                                  lake::test::repository_workspace(*store, customer->id)});
    const auto private_repository =
        store->create_repository({customer->id, "private-repository", "", "private",
                                  lake::test::repository_workspace(*store, customer->id)});
    const auto reformatting_repository =
        store->create_repository({customer->id, "reformatting-repository", "", "public",
                                  lake::test::repository_workspace(*store, customer->id)});
    ASSERT_TRUE(public_repository);
    ASSERT_TRUE(private_repository);
    ASSERT_TRUE(reformatting_repository);

    const std::filesystem::path repository_path =
        root.path() / customer->id / "repositories" / public_repository->id;
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(root.path() / customer->id / "store"));
    ASSERT_TRUE(customer_store) << customer_store.error().message;
    liquid::repository_options repository_options;
    repository_options.store = &*customer_store;
    auto opened =
        liquid::runtime::block_on(liquid::repository::create(repository_path, repository_options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("readme.md", bytes("public content"))));
    const auto first_change = liquid::runtime::block_on(repository.cut("initial public content"));
    ASSERT_TRUE(first_change) << first_change.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("readme.md", bytes("public content\nupdated\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.md", bytes("new file\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("MODEL_CARD.md", bytes("# Recorded fixture model\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("weights.bin", bytes("recorded fixture weights"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("nested/config.json", bytes("{\"fixture\":true}"))));
    const auto second_change = liquid::runtime::block_on(repository.cut("update public content"));
    ASSERT_TRUE(second_change) << second_change.error().message;
    const std::vector<std::byte> artifact_bytes = bytes("artifact output");
    const auto artifact_content = liquid::hash(liquid::algorithm::blake3, artifact_bytes);
    ASSERT_TRUE(artifact_content) << artifact_content.error().message;
    const liquid::blob remote_artifact{
        .chunks = {*artifact_content},
        .size = artifact_bytes.size(),
        .chunk_sizes = {artifact_bytes.size()},
        .content = *artifact_content,
        .remote = liquid::remote_blob_location{"test", "worker-1"},
    };
    const auto artifact_record =
        liquid::runtime::block_on(repository.store_remote_blob(remote_artifact));
    ASSERT_TRUE(artifact_record) << artifact_record.error().message;
    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_FALSE(history->empty());
    const liquid::action_input build_input{
        .command = {"remote-build"},
        .env = {{"LAKE_SOURCE_REPOSITORY_ID", public_repository->id},
                {"LAKE_SOURCE_CHANGE_ID", liquid::to_string(second_change->value)}},
        .outputs = {"build/app"},
        .inputs = history->front().root,
        .toolchain = history->front().root,
        .platform = host_platform()};
    auto first_result_value = liquid::action_result{
        .outputs = {{"build/app", 0100755, *artifact_record}},
        .stdout_output = *artifact_record,
        .stderr_output = *artifact_record,
        .timing = {.queued = std::chrono::milliseconds{1},
                   .sandbox = std::chrono::milliseconds{2},
                   .exec = std::chrono::milliseconds{3},
                   .hash = std::chrono::milliseconds{4},
                   .store = std::chrono::milliseconds{5}},
        .worker = "worker-1",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{100}},
        .cache = liquid::cache_status::hit,
    };
    first_result_value.started_at = std::chrono::sys_seconds{std::chrono::seconds{98}};
    first_result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{100}};
    const auto first_result = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, build_input, first_result_value));
    ASSERT_TRUE(first_result) << first_result.error().message;
    auto second_result_value = first_result_value;
    second_result_value.observed_at = std::chrono::sys_seconds{std::chrono::seconds{200}};
    second_result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{200}};
    const auto second_result = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, build_input, second_result_value));
    ASSERT_TRUE(second_result) << second_result.error().message;

    const std::filesystem::path reformatting_path =
        root.path() / customer->id / "repositories" / reformatting_repository->id;
    auto reformatting_opened = liquid::runtime::block_on(
        liquid::repository::create(reformatting_path, repository_options));
    ASSERT_TRUE(reformatting_opened) << reformatting_opened.error().message;
    auto reformatting_source = std::move(*reformatting_opened);
    ASSERT_TRUE(
        liquid::runtime::block_on(reformatting_source.write("format.cpp", bytes("int value;\n"))));
    const auto formatted_before =
        liquid::runtime::block_on(reformatting_source.cut("before formatting"));
    ASSERT_TRUE(formatted_before) << formatted_before.error().message;
    const auto formatting_change =
        liquid::runtime::block_on(reformatting_source.cut("reformatting"));
    ASSERT_TRUE(formatting_change) << formatting_change.error().message;

    lake::test::RecordWorkspaces release_workspaces(store);
    release_workspaces.ready(public_repository->workspace_id);
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.admin_token = "test-token";
    options.store = store;
    options.workspaces = release_workspaces.service;
    options.instance_kek = "6c616b652d666f7267652d7265636f7264732d746573742d696e7374616e6365";
    options.store_root = root.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    // What the web app's repository pages read, signed out: a public repository is readable
    // through the API, a private one is not (lake#561).
    const auto read = [&](const std::string& path, std::string_view authorization = {}) {
        const std::string response = http_get(server.port(), "/v1/" + path, authorization);
        EXPECT_NE(response.find(" 200 "), std::string::npos) << path << "\n" << response;
        return response_json(response)["value"];
    };
    const std::string listed = read("list_repositories?customer_id=" + customer->id).dump();
    EXPECT_NE(listed.find("public-repository"), std::string::npos) << listed;
    EXPECT_EQ(listed.find("private-repository"), std::string::npos) << listed;
    EXPECT_EQ(read("get_repository?id=" + public_repository->id)["name"], "public-repository");
    EXPECT_NE(
        read("repository_tree?id=" + public_repository->id + "&ref=main").dump().find("readme.md"),
        std::string::npos);
    EXPECT_NE(read("repository_file?id=" + public_repository->id + "&ref=main&path=readme.md")
                  .dump()
                  .find("public content"),
              std::string::npos);
    const std::string change = read("change_diff?id=" + public_repository->id +
                                    "&change_id=" + liquid::to_string(second_change->value))
                                   .dump();
    EXPECT_NE(change.find("readme.md"), std::string::npos) << change;
    EXPECT_NE(change.find("notes.md"), std::string::npos) << change;
    EXPECT_NE(change.find("updated"), std::string::npos) << change;
    // A reformatting-only change has no line to show: its one entry is modified with no hunks.
    const json reformatting = read("change_diff?id=" + reformatting_repository->id +
                                   "&change_id=" + liquid::to_string(formatting_change->value));
    const std::string reformatted = reformatting.dump();
    EXPECT_EQ(reformatted.find("int value;"), std::string::npos) << reformatted;
    const json artifacts = read("list_artifacts?id=" + public_repository->id + "&limit=1");
    EXPECT_EQ(artifacts["items"].size(), 1U) << artifacts.dump();
    EXPECT_EQ(artifacts["done"], false) << "a next page";
    EXPECT_FALSE(artifacts.value("next_cursor", std::string()).empty());
    const std::string artifact = read("get_artifact?id=" + public_repository->id +
                                      "&result_id=" + liquid::to_string(second_result->value))
                                     .dump();
    EXPECT_NE(artifact.find("build/app"), std::string::npos) << artifact;
    EXPECT_NE(artifact.find("slsa_provenance/1"), std::string::npos) << artifact;
    EXPECT_NE(artifact.find("sbom/1"), std::string::npos) << artifact;

    // Real signed action provenance must bind the selected result to this exact source.
    const auto release = [&](const std::string& change_id) {
        return response_json(http_request(
            server.port(),
            "POST",
            "/v1/create_release",
            json{{"repository_id", public_repository->id},
                 {"tag", "binding-proof"},
                 {"change_id", change_id},
                 {"artifact_ids", json::array({liquid::to_string(second_result->value)})}}
                .dump(),
            "application/json",
            {},
            "Bearer test-token"));
    };
    const auto bound_release = release(liquid::to_string(second_change->value));
    ASSERT_TRUE(bound_release.value("ok", false)) << bound_release.dump();
    EXPECT_EQ(bound_release["value"]["value"]["artifacts"][0].value("source_binding", ""),
              "verified");
    const auto mismatched_release = release(liquid::to_string(first_change->value));
    EXPECT_FALSE(mismatched_release.value("ok", true)) << mismatched_release.dump();
    EXPECT_EQ(mismatched_release["error"]["code"], "conflict");

    const std::string model_response =
        http_get(server.port(), "/api/models/" + public_repository->id);
    EXPECT_NE(model_response.find(" 200 "), std::string::npos) << model_response;
    const json model = response_json(model_response);
    ASSERT_TRUE(model.is_object());
    EXPECT_EQ(model["card"], "# Recorded fixture model\n");
    EXPECT_TRUE(model["provenance_complete"]);
    const std::string model_file =
        http_get(server.port(), "/models/" + public_repository->id + "/resolve/main/weights.bin");
    EXPECT_NE(model_file.find(" 200 "), std::string::npos) << model_file;
    EXPECT_NE(model_file.find("recorded fixture weights"), std::string::npos);
    const std::string nested_model_file = http_get(
        server.port(), "/models/" + public_repository->id + "/resolve/main/nested/config.json");
    EXPECT_NE(nested_model_file.find(" 200 "), std::string::npos) << nested_model_file;
    EXPECT_NE(nested_model_file.find("{\"fixture\":true}"), std::string::npos);

    const std::string drive =
        read("drive_browse?repository_id=" + public_repository->id + "&ref=main&path=",
             "Bearer test-token")
            .dump();
    EXPECT_NE(drive.find("readme.md"), std::string::npos) << drive;
    const std::string uploaded = http_request(
        server.port(),
        "POST",
        "/v1/upload_drive_files",
        json{{"repository_id", public_repository->id},
             {"ref", "main"},
             {"path", ""},
             {"files",
              json::array({{{"name", "browser.txt"}, {"content", "YnJvd3NlciB1cGxvYWQ="}}})}}
            .dump(),
        "application/json",
        {},
        "Bearer test-token");
    EXPECT_NE(uploaded.find(" 200 "), std::string::npos) << uploaded;
    EXPECT_NE(
        read("drive_preview?repository_id=" + public_repository->id + "&ref=main&path=browser.txt",
             "Bearer test-token")
            .dump()
            .find("browser upload"),
        std::string::npos);
    const std::string downloaded = http_get(
        server.port(),
        "/drive/download?repository_id=" + public_repository->id + "&ref=main&path=browser.txt",
        "Bearer test-token");
    EXPECT_NE(downloaded.find(" 200 "), std::string::npos) << downloaded;
    EXPECT_NE(downloaded.find("attachment; filename=\"browser.txt\""), std::string::npos)
        << downloaded;
    EXPECT_NE(downloaded.find("browser upload"), std::string::npos);
    EXPECT_NE(read("diff_drive_versions?repository_id=" + public_repository->id +
                       "&ref=main&path=readme.md&left=" + liquid::to_string(first_change->value) +
                       "&right=" + liquid::to_string(second_change->value),
                   "Bearer test-token")
                  .dump()
                  .find("updated"),
              std::string::npos);

    // A private repository: not through the API, and a download sends the person to sign in.
    const std::string private_api =
        http_get(server.port(), "/v1/get_repository?id=" + private_repository->id);
    EXPECT_NE(private_api.find(" 401 "), std::string::npos);
    const std::string private_file =
        http_get(server.port(),
                 "/file/raw?repository_id=" + private_repository->id + "&ref=main&path=readme.md");
    EXPECT_NE(private_file.find(" 302 "), std::string::npos) << private_file;
    EXPECT_NE(private_file.find("Location: /sign-in"), std::string::npos) << private_file;
    server.stop();
}

TEST(Repositories, LifecycleUsesPolicyAndAuditRegistry) {
    Surface surface;
    const ApiResult first_user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult second_user = surface.call("create_user", {{"display_name", "Grace"}});
    ASSERT_TRUE(first_user.ok);
    ASSERT_TRUE(second_user.ok);
    const ApiResult first_customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", first_user.value["id"]}, {"name", "ada"}});
    const ApiResult second_customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", second_user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(first_customer.ok);
    ASSERT_TRUE(second_customer.ok);
    const ApiResult created = surface.call(
        "create_repository",
        {{"customer_id", first_customer.value["id"]},
         {"workspace_id",
          lake::test::repository_workspace(surface.store, first_customer.value["id"])},
         {"name", "liquid"}});
    ASSERT_TRUE(created.ok);
    const std::string repository_id = created.value["id"];

    ASSERT_TRUE(surface
                    .call("set_repository_visibility",
                          {{"id", repository_id}, {"visibility", "organization"}})
                    .ok);
    ASSERT_TRUE(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "liquid-next"}}).ok);
    // A repository stays in its space and workspace (lake#315).
    const ApiResult transferred =
        surface.call("transfer_repository",
                     {{"id", repository_id}, {"customer_id", second_customer.value["id"]}});
    EXPECT_EQ(transferred.error.code, "conflict");
    const ApiResult deleted = surface.call("delete_repository", {{"id", repository_id}});
    const std::string deletion_request = surface.ctx.request_id;
    ASSERT_TRUE(deleted.ok);
    EXPECT_FALSE(deleted.value["deleted_at"].is_null());
    EXPECT_FALSE(deleted.value["retention_until"].is_null());
    EXPECT_EQ(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "again"}}).error.code,
        "conflict");

    const ApiResult events =
        surface.call("audit", {{"request_id", deletion_request}, {"limit", 1}});
    ASSERT_TRUE(events.ok);
    EXPECT_TRUE(std::ranges::any_of(events.value["items"], [](const json& event) {
        return event["action"] == "delete_repository" && event["outcome"] == "success";
    }));

    surface.ctx.acting_identity = "guest";
    EXPECT_EQ(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "denied"}}).error.code,
        "unauthorized");
}

TEST(ModelSync, MultiGigabyteRevisionsDeduplicateAndResumePull) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:model-owner", "model-owner@example.test", "Model Owner"});
    ASSERT_TRUE(user);
    const auto customer = control->create_customer({"individual", user->id, "", "models"});
    ASSERT_TRUE(customer);
    const auto repository =
        control->create_repository({customer->id, "large-model", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository);

    const std::filesystem::path weights = temporary.path() / "weights.bin";
    constexpr std::uint64_t logical_size = std::uint64_t{2} * 1024 * 1024 * 1024;
    {
        std::ofstream output(weights, std::ios::binary);
        ASSERT_TRUE(output);
    }
    std::filesystem::resize_file(weights, logical_size);
    const std::string block(1024 * 1024, 'm');
    {
        std::fstream file(weights, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file);
        file.seekp(64 * 1024 * 1024);
        file.write(block.data(), static_cast<std::streamsize>(block.size()));
        file.seekp(std::streamoff{1024} * 1024 * 1024);
        file.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "source-large"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write_file("weights.bin", weights)));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("model revision one")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "model-sync-test";
    options.admin_token = "model-sync-admin";
    options.instance_kek = "model-sync-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    liquid::sync_endpoint endpoint(
        origin + "/v1/repositories/" + repository->id,
        liquid::sync_endpoint_options{.bearer_token = "model-sync-admin"});
    const auto first_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    const std::filesystem::path packs =
        temporary.path() / "lake" / customer->id / "store" / "packs";
    const std::uint64_t first_unique = unique_pack_raw_bytes(packs);

    {
        std::fstream file(weights, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file);
        const std::string zeros(block.size(), '\0');
        file.seekp(64 * 1024 * 1024);
        file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        file.seekp(68 * 1024 * 1024);
        file.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    ASSERT_TRUE(liquid::runtime::block_on(source->write_file("weights.bin", weights)));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("model revision two")));
    const auto second_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second_push) << second_push.error().message;
    const std::uint64_t second_unique = unique_pack_raw_bytes(packs);
    const std::uint64_t logical_revisions = logical_size * 2;
    const double dedup_ratio =
        1.0 - static_cast<double>(second_unique) / static_cast<double>(logical_revisions);
    std::cout << "multi-gigabyte model dedup ratio: " << dedup_ratio << '\n';
    EXPECT_GT(dedup_ratio, 0.99);
    EXPECT_LT(second_unique - first_unique, 16U * 1024U * 1024U);
    EXPECT_LT(second_push->sent.bytes, first_push->sent.bytes);

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "destination-large"));
    ASSERT_TRUE(destination);
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto resumed = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_EQ(resumed->received.bytes, 0U);
    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree);
    ASSERT_TRUE(destination_tree);
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));

    const std::string info = http_request(server.port(),
                                          "GET",
                                          "/api/models/" + repository->id,
                                          "",
                                          "application/json",
                                          {},
                                          "Bearer model-sync-admin");
    EXPECT_NE(info.find(" 200 "), std::string::npos) << info;
    const nlohmann::json storage = response_json(info).at("storage");
    EXPECT_EQ(storage["logical_bytes"], logical_revisions);
    EXPECT_GT(storage["dedup_ratio"].get<double>(), 0.99);
    EXPECT_EQ(storage["revisions"].size(), 2U);
    server.stop();
}

// lake#805: a push records the id liquid gave the repository, the one `liquid deploy` from a
// checkout of it sends, so the platform sync finds the repository a deployment row names.
TEST(SyncHttp, PushRecordsTheRepositorysLiquidId) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:liquid-id-owner", "liquid-id-owner@example.test", "Liquid Id Owner"});
    ASSERT_TRUE(user);
    const auto customer = control->create_customer({"individual", user->id, "", "liquid-ids"});
    ASSERT_TRUE(customer);
    const auto repository =
        control->create_repository({customer->id, "deployed", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository);
    EXPECT_TRUE(repository->liquid_id.empty());

    const std::filesystem::path file = temporary.path() / "app.txt";
    {
        std::ofstream output(file);
        output << "hello";
    }
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "checkout"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write_file("app.txt", file)));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "liquid-id-test";
    options.admin_token = "liquid-id-admin";
    options.instance_kek = "liquid-id-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    liquid::sync_endpoint endpoint(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id,
        liquid::sync_endpoint_options{.bearer_token = "liquid-id-admin"});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;
    // A new checkout takes the identity of the repository it pulls from, as `liquid clone` does.
    auto clone = liquid::runtime::block_on(liquid::repository::create(temporary.path() / "clone"));
    ASSERT_TRUE(clone);
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*clone));
    ASSERT_TRUE(pulled) << pulled.error().message;
    server.stop();

    // A repository lake created empty took the identity of its first push: the checkout's id.
    const auto id = source->id();
    ASSERT_TRUE(id);
    const auto cloned = clone->id();
    ASSERT_TRUE(cloned);
    EXPECT_EQ(*cloned, *id) << "liquid deploy from the clone sends the same id";
    std::string hex;
    for (const std::byte octet : *id) {
        hex += std::format("{:02x}", std::to_integer<unsigned char>(octet));
    }
    const auto recorded = control->get_repository(repository->id);
    ASSERT_TRUE(recorded);
    EXPECT_EQ(recorded->liquid_id, hex);
    const auto named = control->repositories_by_liquid_id(hex);
    ASSERT_TRUE(named);
    ASSERT_EQ(named->size(), 1U);
    EXPECT_EQ(named->front().id, repository->id);
}

TEST(SyncHttp, PushPullShareCustomerStoreAndTransferOnlyMissingChunks) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:sync-owner", "sync-owner@example.test", "Sync Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "sync-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "project", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto bad_repository =
        control->create_repository({customer->id, "bad-signature", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(bad_repository) << bad_repository.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "source"));
    ASSERT_TRUE(source) << source.error().message;
    const std::string first_contents(128 * 1024, 'a');
    ASSERT_TRUE(liquid::runtime::block_on(source->write("first.txt", sync_bytes(first_contents))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "sync-test";
    options.admin_token = "sync-admin";
    options.instance_kek = "sync-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string direct_url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    RecordingProxy proxy(server.port());
    ASSERT_NE(proxy.port(), 0);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(proxy.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(url,
                                   liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto first_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    EXPECT_GT(first_push->sent.bytes, 0U);

    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("second.txt", sync_bytes("second version"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("second")));
    const auto second_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second_push) << second_push.error().message;
    EXPECT_GT(second_push->sent.bytes, 0U);
    EXPECT_LT(second_push->sent.bytes, first_push->sent.bytes);
    const auto unchanged_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(unchanged_push) << unchanged_push.error().message;
    EXPECT_EQ(unchanged_push->sent.bytes, 0U);
    proxy.stop();

    auto corrupt_push = proxy.push_body();
    ASSERT_FALSE(corrupt_push.empty());
    auto page = std::vector<liquid::sync_item>(256);
    const auto exported =
        liquid::runtime::block_on(source->export_drips(liquid::journal_cursor{}, page));
    ASSERT_TRUE(exported) << exported.error().message;
    const auto signed_drip = std::ranges::find_if(
        page.begin(), page.begin() + exported->count, [](const liquid::sync_item& item) {
            return item.kind == liquid::sync_item_kind::drip && !item.signature.empty();
        });
    ASSERT_NE(signed_drip, page.begin() + exported->count);
    const auto signature = std::search(corrupt_push.begin(),
                                       corrupt_push.end(),
                                       signed_drip->signature.begin(),
                                       signed_drip->signature.end());
    ASSERT_NE(signature, corrupt_push.end());
    *signature ^= std::byte{1};
    const std::string corrupt_body(reinterpret_cast<const char*>(corrupt_push.data()),
                                   corrupt_push.size());
    const std::string rejected =
        http_request(server.port(),
                     "POST",
                     "/v1/repositories/" + bad_repository->id + "/sync/push",
                     corrupt_body,
                     "application/vnd.liquid.sync-v1",
                     {},
                     "Bearer sync-admin");
    EXPECT_NE(rejected.find(" 400 "), std::string::npos) << rejected;

    auto destination =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    liquid::sync_endpoint pull_endpoint(
        direct_url, liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto pulled = liquid::runtime::block_on(pull_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));
    const auto source_first = liquid::runtime::block_on(source->read("first.txt"));
    const auto destination_first = liquid::runtime::block_on(destination->read("first.txt"));
    ASSERT_TRUE(source_first) << source_first.error().message;
    ASSERT_TRUE(destination_first) << destination_first.error().message;
    EXPECT_EQ(*destination_first, *source_first);
    const auto source_second = liquid::runtime::block_on(source->read("second.txt"));
    const auto destination_second = liquid::runtime::block_on(destination->read("second.txt"));
    ASSERT_TRUE(source_second) << source_second.error().message;
    ASSERT_TRUE(destination_second) << destination_second.error().message;
    EXPECT_EQ(*destination_second, *source_second);
    auto rejected_destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "rejected-destination"));
    ASSERT_TRUE(rejected_destination) << rejected_destination.error().message;
    const std::string rejected_url = "http://127.0.0.1:" + std::to_string(server.port()) +
                                     "/v1/repositories/" + bad_repository->id;
    liquid::sync_endpoint rejected_endpoint(
        rejected_url, liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto rejected_pull =
        liquid::runtime::block_on(rejected_endpoint.pull(*rejected_destination));
    ASSERT_TRUE(rejected_pull) << rejected_pull.error().message;
    const auto rejected_tree = liquid::runtime::block_on(rejected_destination->root());
    ASSERT_TRUE(rejected_tree) << rejected_tree.error().message;
    EXPECT_TRUE(rejected_tree->entries.empty());
    EXPECT_FALSE(liquid::runtime::block_on(rejected_destination->head()));
    const json references = served_refs(server.port(), repository->id, "Bearer sync-admin");
    ASSERT_TRUE(references.is_array()) << references.dump();
    EXPECT_FALSE(references.empty());
    for (const json& reference : references) {
        EXPECT_EQ(reference.at("repository_id"), repository->id);
        EXPECT_FALSE(reference.at("head").get<std::string>().empty());
        EXPECT_FALSE(reference.contains("updated_at")) << "lake keeps no copy of refs (lake#319)";
    }
    server.stop();

    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "store"));
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "store" /
                                        "repositories"));
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "repositories" /
                                         repository->id / ".liquid-store"));
    const auto events = lake::test::audit_events(*control, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push_change" && event.draft.outcome == "success";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push" && event.draft.outcome == "failure";
    }));
}

TEST(SyncHttp, TwoLakeInstancesReplicateRepositoryAndResumeFromCursor) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    auto control = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = control->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const std::string unique = lake::control::new_uuid_v4();
    const auto user = control->create_user(
        {"taap:1:ed25519:" + unique, unique + "@example.test", "Replication Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        control->create_customer({"individual", user->id, "", "replication-owner-" + unique});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "replicated", "", "private",
                                                        lake::test::repository_workspace(
                                                            *control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    auto local_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "local-source"));
    ASSERT_TRUE(local_source) << local_source.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(local_source->write("entry.txt", sync_bytes("region data"))));
    ASSERT_TRUE(liquid::runtime::block_on(local_source->cut("initial")));

    const unsigned short source_port = unused_loopback_port();
    const unsigned short replica_port = unused_loopback_port();
    ASSERT_NE(source_port, 0);
    ASSERT_NE(replica_port, 0);
    ASSERT_NE(source_port, replica_port);
    LakeProcess source(source_port, "region-a", database_uri, temporary.path() / "region-a");
    ASSERT_TRUE(source.ready());
    const std::string source_repository_url =
        "http://127.0.0.1:" + std::to_string(source_port) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint source_endpoint(
        source_repository_url, liquid::sync_endpoint_options{.bearer_token = "replication-admin"});
    const auto pushed = liquid::runtime::block_on(source_endpoint.push(*local_source));
    ASSERT_TRUE(pushed) << pushed.error().message;

    // The replica sends its replication credential only to a configured peer (lake#915); plain
    // http to loopback is a test-only allowance.
    const std::string source_origin = "http://127.0.0.1:" + std::to_string(source_port);
    LakeProcess replica(replica_port,
                        "region-b",
                        database_uri,
                        temporary.path() / "region-b",
                        {"LAKE_REPLICATION_TOKEN=replication-admin",
                         "LAKE_REPLICATION_PEERS=" + source_origin,
                         "LAKE_REPLICATION_ALLOW_PRIVATE_ADDRESSES=1"});
    ASSERT_TRUE(replica.ready());

    const auto replicate = [&] {
        return http_request(replica_port,
                            "POST",
                            "/v1/replicate_repository",
                            json{{"repository_id", repository->id},
                                 {"source_instance", "region-a"},
                                 {"source_url", source_origin}}
                                .dump(),
                            "application/json",
                            {},
                            "Bearer replication-admin");
    };
    const std::string first_response = replicate();
    EXPECT_NE(first_response.find(" 200 "), std::string::npos) << first_response;
    EXPECT_GT(response_json(first_response).at("value").at("received_objects").get<std::uint64_t>(),
              0U);

    const std::string replica_repository_url =
        "http://127.0.0.1:" + std::to_string(replica_port) + "/v1/repositories/" + repository->id;
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    liquid::sync_endpoint replica_endpoint(
        replica_repository_url, liquid::sync_endpoint_options{.bearer_token = "replication-admin"});
    const auto pulled = liquid::runtime::block_on(replica_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto source_tree = liquid::runtime::block_on(local_source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));

    const std::string resumed_response = replicate();
    EXPECT_NE(resumed_response.find(" 200 "), std::string::npos) << resumed_response;
    EXPECT_EQ(response_json(resumed_response).at("value").at("received_bytes"), 0);
    // The cursor is liquid repository state (liquid#326), kept in the store that holds the
    // repository, so it moves with it (lake#380).
    bool cursor_in_store = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             temporary.path() / "region-b" / customer->id / "store" / "repositories")) {
        cursor_in_store = cursor_in_store || (entry.path().filename() == "region-a.cursor" &&
                                              entry.path().parent_path().filename() == "replication");
    }
    EXPECT_TRUE(cursor_in_store);
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "region-b" / customer->id /
                                         "repositories" / repository->id / "replication" /
                                         "region-a.cursor"));
}

// Runs the lake executable with `environment` (NAME=value, replacing the parent's value; a bare
// NAME removes it) until it is ready or exits, and answers its exit status (-1 while it was still
// running, then stopped) and what it wrote to stdout.
std::pair<int, std::string> run_lake_until_ready(const std::vector<std::string>& arguments,
                                                 const std::vector<std::string>& environment,
                                                 unsigned short port,
                                                 const std::filesystem::path& output) {
    std::vector<std::string> variables;
    for (const std::string& given : environment) {
        if (given.find('=') != std::string::npos) {
            variables.push_back(given);
        }
    }
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view variable(*entry);
        const std::string name(variable.substr(0, variable.find('=')));
        if (std::ranges::none_of(environment, [&name](const std::string& given) {
                return given == name || given.starts_with(name + "=");
            })) {
            variables.emplace_back(variable);
        }
    }
    std::vector<std::string> argv{LAKE_TEST_SERVER_PATH};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    std::vector<char*> raw_argv;
    for (std::string& argument : argv) {
        raw_argv.push_back(argument.data());
    }
    raw_argv.push_back(nullptr);
    std::vector<char*> raw_environment;
    for (std::string& variable : variables) {
        raw_environment.push_back(variable.data());
    }
    raw_environment.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = -1;
    const int spawned = ::posix_spawn(
        &pid, raw_argv.front(), &actions, nullptr, raw_argv.data(), raw_environment.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return {-2, {}};
    }
    int status = 0;
    int exit_status = -1;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -3;
            break;
        }
        const std::string ready = http_request(port, "GET", "/readyz", "", "application/json");
        if (ready.find(" 200 ") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (exit_status == -1) {
        ::kill(pid, SIGTERM);
        static_cast<void>(::waitpid(pid, nullptr, 0));
    }
    std::ifstream written(output);
    return {exit_status, std::string(std::istreambuf_iterator<char>(written), {})};
}

// lake#1168: with no control store (no --memory-store, no control DB) lake used to dereference a
// null store in its owner loop and die with SIGSEGV a moment after start. It now stays live (`/healthz`
// 200, `/readyz` 503: readiness is the control store's), reports `store: none`, keeps serving past
// the owner loop's first pass, and exits 0 on SIGTERM.
TEST(StorelessStartup, LakeWithNoControlStoreServesAndStopsCleanly) {
    TemporarySyncRoot temporary;
    const unsigned short port = unused_loopback_port();
    ASSERT_NE(port, 0);
    const std::vector<std::string> removed{"LAKE_CONTROL_DB_URI", "LAKE_STORE_ROOT"};
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view variable(*entry);
        const std::string name(variable.substr(0, variable.find('=')));
        if (std::ranges::find(removed, name) == removed.end()) {
            variables.emplace_back(variable);
        }
    }
    std::vector<std::string> argv{
        LAKE_TEST_SERVER_PATH, "--listen", "127.0.0.1:" + std::to_string(port)};
    std::vector<char*> raw_argv;
    for (std::string& argument : argv) {
        raw_argv.push_back(argument.data());
    }
    raw_argv.push_back(nullptr);
    std::vector<char*> raw_environment;
    for (std::string& variable : variables) {
        raw_environment.push_back(variable.data());
    }
    raw_environment.push_back(nullptr);
    const std::filesystem::path output = temporary.path() / "stdout.log";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = -1;
    const int spawned = ::posix_spawn(
        &pid, raw_argv.front(), &actions, nullptr, raw_argv.data(), raw_environment.data());
    posix_spawn_file_actions_destroy(&actions);
    ASSERT_EQ(spawned, 0);
    int status = 0;
    bool exited = false;
    bool ready = false;
    for (int attempt = 0; attempt < 400 && !exited && !ready; ++attempt) {
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
        }
        // Live, not ready: /readyz is control-store readiness and answers 503 without a store.
        ready = http_request(port, "GET", "/healthz", "", "application/json").find(" 200 ") !=
                std::string::npos;
        if (!ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    // The owner loop's first pass runs right after the startup line; give it time to run, then
    // check lake is still alive and serving.
    for (int attempt = 0; attempt < 10 && !exited; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
        }
    }
    const std::string healthz =
        exited ? std::string{} : http_request(port, "GET", "/healthz", "", "application/json");
    const std::string readyz =
        exited ? std::string{} : http_request(port, "GET", "/readyz", "", "application/json");
    if (!exited) {
        ::kill(pid, SIGTERM);
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    }
    std::ifstream written(output);
    const std::string stdout_text(std::istreambuf_iterator<char>(written), {});
    ASSERT_FALSE(exited) << "lake exited on its own (signal "
                         << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ", status "
                         << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << "):\n"
                         << stdout_text;
    EXPECT_TRUE(ready) << stdout_text;
    EXPECT_NE(healthz.find(" 200 "), std::string::npos) << healthz;
    EXPECT_NE(readyz.find(" 503 "), std::string::npos) << readyz;
    ASSERT_TRUE(WIFEXITED(status)) << "lake was killed by signal "
                                   << (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    EXPECT_EQ(WEXITSTATUS(status), 0) << stdout_text;
    std::optional<json> startup;
    std::istringstream lines(stdout_text);
    for (std::string line; std::getline(lines, line);) {
        const json parsed = json::parse(line, nullptr, false);
        if (parsed.is_object() && parsed.value("event", "") == "startup") {
            startup = parsed;
        }
    }
    ASSERT_TRUE(startup) << stdout_text;
    EXPECT_EQ(startup->at("store"), "none");
}

// lake#915: the replication credential never defaults to the admin token, and a peer list
// that is not a list of origins stops startup.
TEST(ReplicationStartup,TheReplicationCredentialNeverDefaultsToTheAdminToken) {
    TemporarySyncRoot temporary;
    const unsigned short port = unused_loopback_port();
    ASSERT_NE(port, 0);
    const std::vector<std::string> arguments{
        "--memory-store",
        "--listen",
        "127.0.0.1:" + std::to_string(port),
        "--store-root",
        (temporary.path() / "root").string(),
        "--instance-kek",
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"};
    const auto [status, output] = run_lake_until_ready(
        arguments,
        {"LAKE_ADMIN_TOKEN=startup-admin", "LAKE_REPLICATION_TOKEN", "LAKE_REPLICATION_PEERS"},
        port,
        temporary.path() / "stdout.log");
    ASSERT_EQ(status, -1) << "lake exited during startup:\n" << output;
    std::optional<json> startup;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        const json parsed = json::parse(line, nullptr, false);
        if (parsed.is_object() && parsed.value("event", "") == "startup") {
            startup = parsed;
        }
    }
    ASSERT_TRUE(startup) << output;
    EXPECT_EQ(startup->at("admin_token_set"), "yes");
    EXPECT_EQ(startup->at("replication_token_set"), "no") << startup->dump();
    EXPECT_EQ(startup->at("replication_peers"), json::array());

    const unsigned short refused_port = unused_loopback_port();
    std::vector<std::string> refused_arguments = arguments;
    refused_arguments[2] = "127.0.0.1:" + std::to_string(refused_port);
    const auto [refused_status, refused_output] = run_lake_until_ready(
        refused_arguments,
        {"LAKE_ADMIN_TOKEN=startup-admin",
         "LAKE_REPLICATION_PEERS=https://region-a.lake.example/v1/repositories"},
        refused_port,
        temporary.path() / "refused.log");
    EXPECT_EQ(refused_status, 2) << refused_output;
}

// lake#1135, lake#1137: the lake binary reads the brand at start. An invalid brand stops it with
// exit status 2; a custom brand is served by the running process on the console's first paint,
// llms.txt, the skill under the brand's short name, MCP initialize and server_info.
TEST(BrandStartup, AnInvalidBrandStopsLakeAndACustomOneIsServed) {
    TemporarySyncRoot temporary;
    const auto arguments_for = [&](unsigned short port) {
        return std::vector<std::string>{
            "--memory-store",
            "--listen",
            "127.0.0.1:" + std::to_string(port),
            "--store-root",
            (temporary.path() / "root").string(),
            "--instance-kek",
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"};
    };
    const std::vector<std::string> unset{"LAKE_BRAND_FILE",
                                         "LAKE_BRAND_SHORT_NAME",
                                         "LAKE_BRAND_COMPANY",
                                         "LAKE_BRAND_ASSISTANT_NAME",
                                         "LAKE_BRAND_CONSOLE_TITLE",
                                         "LAKE_BRAND_BY_ORAMA",
                                         "LAKE_BRAND_LOGO",
                                         "LAKE_BRAND_LOGO_DARK",
                                         "LAKE_BRAND_FAVICON",
                                         "LAKE_BRAND_ACCENT_DARK",
                                         "LAKE_BRAND_LEGAL_NAME",
                                         "LAKE_BRAND_TERMS_URL",
                                         "LAKE_BRAND_PRIVACY_URL",
                                         "LAKE_RESEND_FROM",
                                         "LAKE_SUPPORT_EMAIL",
                                         "LAKE_PUBLIC_HOST",
                                         "LAKE_EXTERNAL_ORIGIN"};
    for (const std::vector<std::string>& invalid :
         {std::vector<std::string>{"LAKE_BRAND_ACCENT=#123456"},
          std::vector<std::string>{"LAKE_BRAND_NAME=Acme Data",
                                   "LAKE_BRAND_SUPPORT_EMAIL=help@acme.example"},
          std::vector<std::string>{"LAKE_BRAND_NAME=Acme Data",
                                   "LAKE_BRAND_SUPPORT_EMAIL=help@acme.example",
                                   "LAKE_BRAND_EMAIL_FROM=hello@acme.example",
                                   "LAKE_BRAND_ACCENT=blue"}}) {
        const unsigned short port = unused_loopback_port();
        ASSERT_NE(port, 0);
        std::vector<std::string> environment = unset;
        for (const char* name : {"LAKE_BRAND_NAME",
                                 "LAKE_BRAND_SUPPORT_EMAIL",
                                 "LAKE_BRAND_EMAIL_FROM",
                                 "LAKE_BRAND_ACCENT"}) {
            environment.emplace_back(name);
        }
        environment.insert(environment.end(), invalid.begin(), invalid.end());
        const auto [status, output] = run_lake_until_ready(
            arguments_for(port), environment, port, temporary.path() / "invalid.log");
        EXPECT_EQ(status, 2) << invalid.back() << "\n" << output;
    }

    const unsigned short port = unused_loopback_port();
    ASSERT_NE(port, 0);
    std::vector<std::string> environment = unset;
    environment.insert(environment.end(),
                       {"LAKE_BRAND_NAME=Acme Data",
                        "LAKE_BRAND_SUPPORT_EMAIL=help@acme.example",
                        "LAKE_BRAND_EMAIL_FROM=Acme Data <hello@acme.example>",
                        "LAKE_BRAND_ACCENT=#aa3300",
                        "LAKE_PUBLIC_HOST=data.acme.example"});
    // Started by hand, not by run_lake_until_ready (which stops it once ready), to read its
    // surfaces while it runs.
    std::vector<std::string> argv{LAKE_TEST_SERVER_PATH};
    for (const std::string& argument : arguments_for(port)) {
        argv.push_back(argument);
    }
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view variable(*entry);
        const std::string name(variable.substr(0, variable.find('=')));
        if (std::ranges::none_of(environment, [&name](const std::string& given) {
                return given == name || given.starts_with(name + "=");
            })) {
            variables.emplace_back(variable);
        }
    }
    for (const std::string& given : environment) {
        if (given.find('=') != std::string::npos) {
            variables.push_back(given);
        }
    }
    std::vector<char*> raw_argv;
    for (std::string& argument : argv) {
        raw_argv.push_back(argument.data());
    }
    raw_argv.push_back(nullptr);
    std::vector<char*> raw_environment;
    for (std::string& variable : variables) {
        raw_environment.push_back(variable.data());
    }
    raw_environment.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const std::string log = (temporary.path() / "served.log").string();
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = -1;
    ASSERT_EQ(
        ::posix_spawn(
            &pid, raw_argv.front(), &actions, nullptr, raw_argv.data(), raw_environment.data()),
        0);
    posix_spawn_file_actions_destroy(&actions);
    bool ready = false;
    for (int attempt = 0; attempt < 200 && !ready; ++attempt) {
        ready = http_request(port, "GET", "/readyz", "", "application/json").find(" 200 ") !=
                std::string::npos;
        if (!ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ASSERT_TRUE(ready);
    const std::string llms = http_request(port, "GET", "/llms.txt", "", "text/plain");
    const std::string skill =
        http_request(port, "GET", "/skills/acme-data/SKILL.md", "", "text/plain");
    const std::string console = http_request(
        port, "GET", "/", "", "text/plain", {}, {}, "localhost", {{"Accept", "text/html"}});
    const std::string initialize =
        http_request(port,
                     "POST",
                     "/mcp",
                     R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
                     "application/json");
    const std::string info = http_request(port, "GET", "/v1/server_info", "", "application/json");
    ::kill(pid, SIGTERM);
    static_cast<void>(::waitpid(pid, nullptr, 0));

    EXPECT_NE(llms.find(" 200 "), std::string::npos) << llms.substr(0, 300);
    EXPECT_NE(llms.find("# Acme Data\n"), std::string::npos);
    EXPECT_NE(llms.find("https://data.acme.example/skills/acme-data/SKILL.md"), std::string::npos);
    EXPECT_EQ(llms.find("Orama"), std::string::npos);
    EXPECT_EQ(llms.find("lake.live"), std::string::npos);
    EXPECT_NE(skill.find(" 200 "), std::string::npos) << skill.substr(0, 300);
    EXPECT_NE(skill.find("name: acme-data\n"), std::string::npos);
    EXPECT_NE(console.find("<title>Acme Data Console</title>"), std::string::npos)
        << console.substr(0, 600);
    EXPECT_NE(console.find("--orama-action-default:#aa3300"), std::string::npos);
    EXPECT_NE(console.find(R"("console_title":"Acme Data Console")"), std::string::npos);
    EXPECT_NE(initialize.find(R"("serverInfo":{"name":"acme-data","title":"Acme Data")"),
              std::string::npos)
        << initialize;
    EXPECT_NE(info.find(R"("name":"acme-data")"), std::string::npos) << info;
}

TEST(SyncHttp, StorageQuotaThrottlesNewWritesAndKeepsExistingContentReadable) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:quota-owner", "quota-owner@example.test", "Quota Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "quota-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "quota-project", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "quota-source"));
    ASSERT_TRUE(source) << source.error().message;
    const std::string original(32 * 1024, 'a');
    ASSERT_TRUE(liquid::runtime::block_on(source->write("original.txt", sync_bytes(original))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("original")));

    ServerOptions options;
    options.port = 0;
    options.admin_token = "quota-admin";
    options.instance_kek = "quota-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer initial(options);
    ASSERT_TRUE(initial.start()) << initial.reason();
    const std::string initial_url =
        "http://127.0.0.1:" + std::to_string(initial.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint initial_endpoint(
        initial_url, liquid::sync_endpoint_options{.bearer_token = "quota-admin"});
    const auto first_push = liquid::runtime::block_on(initial_endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    initial.stop();

    ASSERT_TRUE(liquid::runtime::block_on(
        source->write("refused.txt", sync_bytes(std::string(32 * 1024, 'b')))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("refused")));
    options.metering.storage_quota_bytes = 1;
    ApiServer limited(options);
    ASSERT_TRUE(limited.start()) << limited.reason();
    const std::string limited_url =
        "http://127.0.0.1:" + std::to_string(limited.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint limited_endpoint(
        limited_url, liquid::sync_endpoint_options{.bearer_token = "quota-admin"});
    const auto refused = liquid::runtime::block_on(limited_endpoint.push(*source));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, liquid::errc::busy);

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "quota-destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    const auto pulled = liquid::runtime::block_on(limited_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto retained = liquid::runtime::block_on(destination->read("original.txt"));
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(retained->data()), retained->size()),
              original);
    EXPECT_FALSE(liquid::runtime::block_on(destination->read("refused.txt")));
    limited.stop();
}

TEST(SyncHttp, PushWithoutWriteAccessIsRejectedAndAudited) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = control->create_user(
        {"taap:1:ed25519:sync-owner-denial", "owner-denial@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        control->create_customer({"individual", owner->id, "", "sync-denial-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "private-project", "", "private",
                                    lake::test::repository_workspace(*control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto reader = control->sign_up_password(
        {"lake", "reader@example.test", "correct horse battery staple", "Reader", "test", "{}"});
    ASSERT_TRUE(reader) << reader.error().message;
    const auto principal = control->context_for(reader->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto object = control->find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    const auto grant = control->create_grant({principal->taap_id,
                                              object->id,
                                              "read",
                                              "attestation:sync-reader",
                                              owner->taap_id,
                                              "sync-denial"});
    ASSERT_TRUE(grant) << grant.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "denied-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("private.txt", sync_bytes("not authorized"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("denied")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "sync-denial";
    options.instance_kek = "sync-denial-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(url,
                                   liquid::sync_endpoint_options{.bearer_token = reader->secret});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    EXPECT_FALSE(pushed);
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code, liquid::errc::unauthorized);
    const json references =
        served_refs(server.port(), repository->id, "Bearer " + reader->secret);
    EXPECT_TRUE(references.is_array() && references.empty()) << references.dump();
    server.stop();

    const auto events = lake::test::audit_events(*control, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.outcome == "denied" &&
               (event.draft.action == "upload_chunks" || event.draft.action == "push");
    }));
}

TEST(PostgresSync, PushAndPullUsePostgresControlPlane) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    std::shared_ptr<lake::control::Store> control = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = control->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const std::string suffix = lake::control::new_uuid_v4();
    const auto user = control->create_user({"taap:1:ed25519:postgres-sync-" + suffix,
                                            "postgres-sync-" + suffix + "@example.test",
                                            "Postgres Sync"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        control->create_customer({"individual", user->id, "", "postgres-sync-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    // The repository's workspace Zetta keeps its transfer records (lake#348).
    lake::test::RecordWorkspaces workspaces(control, database_uri);
    const std::string workspace_id = workspaces.create(customer->id, "home");
    const auto repository =
        control->create_repository({customer->id, "project", "", "private", workspace_id});
    ASSERT_TRUE(repository) << repository.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "postgres-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        source->write("postgres.txt", sync_bytes("postgres-backed sync"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("postgres")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "postgres-sync";
    options.admin_token = "postgres-sync-admin";
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    options.workspaces = workspaces.service;
    options.metering_flush_interval = std::chrono::milliseconds(0);
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = "postgres-sync-admin"});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "postgres-destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const std::string metering_response = http_get(server.port(),
                                                   "/v1/metering_usage?customer_id=" + customer->id,
                                                   "Bearer postgres-sync-admin");
    EXPECT_NE(metering_response.find(" 200 "), std::string::npos) << metering_response;
    const json metering = response_json(metering_response);
    ASSERT_TRUE(metering.value("ok", false)) << metering.dump();
    ASSERT_EQ(metering.at("value").at("items").size(), 1U);
    const std::uint64_t accounted =
        unique_pack_raw_bytes(temporary.path() / "lake" / customer->id / "store" / "packs");
    EXPECT_GT(accounted, 0U);
    // Storage is measured live, never recorded per hour.
    EXPECT_EQ(metering.at("value")
                  .at("summary")
                  .at("storage")
                  .at("repositories_bytes")
                  .get<std::uint64_t>(),
              accounted);
    EXPECT_FALSE(metering.at("value").at("items").back().contains("stored_bytes"));
    // Not yet written: the report includes this process's pending transfer.
    const std::uint64_t uploaded =
        metering.at("value").at("items").back().at("transfer_in_bytes").get<std::uint64_t>();
    const std::uint64_t downloaded =
        metering.at("value").at("items").back().at("transfer_out_bytes").get<std::uint64_t>();
    EXPECT_GT(uploaded, 0U);
    EXPECT_GT(downloaded, 0U);
    EXPECT_TRUE(workspaces.records(workspace_id).rows("metering_usage").empty())
        << "a sync request writes nothing to the workspace";

    // The owner loop writes it to the workspace's hourly record; lake's PostgreSQL holds none.
    server.drain();
    EXPECT_EQ(workspaces.records(workspace_id).rows("metering_usage").size(), 1U);
    {
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        EXPECT_TRUE(tx.exec("SELECT to_regclass('metering_usage')")[0][0].is_null())
            << "migration 0070 dropped the retired table";
    }
    const json written = response_json(http_get(server.port(),
                                                "/v1/metering_usage?customer_id=" + customer->id,
                                                "Bearer postgres-sync-admin"));
    ASSERT_EQ(written.at("value").at("items").size(), 1U) << written.dump();
    EXPECT_EQ(written.at("value").at("items").back().at("transfer_in_bytes"), uploaded)
        << "flushed once, counted once";
    EXPECT_EQ(written.at("value").at("items").back().at("transfer_out_bytes"), downloaded);
    // A push wrote no copy of the refs into PostgreSQL (lake#319): they come from liquid.
    const json references =
        served_refs(server.port(), repository->id, "Bearer postgres-sync-admin");
    ASSERT_TRUE(references.is_array()) << references.dump();
    EXPECT_FALSE(references.empty());

    // A second pull counted after the flush is written when the server stops (the drain).
    auto again =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "postgres-again"));
    ASSERT_TRUE(again) << again.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(endpoint.pull(*again)));
    server.stop();
    {
        auto context = lake::api::ApiContext{};
        context.store = control.get();
        context.workspaces = workspaces.service.get();
        context.instance_kek = options.instance_kek;
        auto records = lake::api::workspace_records(context, customer->id, workspace_id);
        ASSERT_TRUE(records);
        const auto transfer =
            liquid::runtime::block_on(lake::control::list_metered_transfer(*records, customer->id));
        ASSERT_TRUE(transfer && *transfer);
        ASSERT_EQ((*transfer)->size(), 1U);
        EXPECT_GT((*transfer)->front().transfer_out_bytes, downloaded)
            << "the stop flushed the second pull";
    }

    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));
    const auto contents = liquid::runtime::block_on(destination->read("postgres.txt"));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ(*contents, sync_bytes("postgres-backed sync"));

    // A push and a pull are the repository's data, so their events are in the
    // audit of the repository's workspace, written by the server's flush (here, its stop), and
    // not in the central log.
    const auto events = lake::test::audit_events(*control, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_FALSE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push" || event.draft.action == "pull";
    }));
    auto context = lake::api::ApiContext{};
    context.store = control.get();
    context.workspaces = workspaces.service.get();
    context.instance_kek = options.instance_kek;
    auto records = lake::api::workspace_records(context, customer->id, workspace_id);
    ASSERT_TRUE(records);
    const auto workspace_events =
        liquid::runtime::block_on(lake::control::list_workspace_audit(*records, {}, 0));
    ASSERT_TRUE(workspace_events && *workspace_events);
    const auto& written_events = **workspace_events;
    EXPECT_TRUE(std::ranges::any_of(written_events, [](const auto& found) {
        return found.event.draft.action == "push" && found.event.draft.outcome == "success" &&
               found.sequence > 0;
    }));
    EXPECT_TRUE(std::ranges::any_of(written_events, [](const auto& found) {
        return found.event.draft.action == "pull" && found.event.draft.outcome == "success";
    }));
    const auto verified =
        liquid::runtime::block_on(lake::control::verify_workspace_audit(*records, *control));
    ASSERT_TRUE(verified && *verified);
    EXPECT_TRUE((*verified)->verified) << (*verified)->error_message;
    EXPECT_EQ((*verified)->chained, written_events.size());
}

TEST(PostgresSync, EnforcesBranchProtectionAtReferenceUpdates) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    std::shared_ptr<lake::control::Store> control =
        lake::control::make_postgres_store(database_uri, instance_kek);
    ASSERT_TRUE(control->ping());
    const std::string suffix = lake::control::new_uuid_v4();
    const auto owner = control->create_user({"taap:1:ed25519:branch-owner-" + suffix,
                                             "branch-owner-" + suffix + "@example.test",
                                             "Branch Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        control->create_customer({"individual", owner->id, "", "branch-owner-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "protected", "", "private",
                                                        lake::test::repository_workspace(
                                                            *control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;

    auto source = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "branch-protection-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", sync_bytes("base"))));
    const auto base = liquid::runtime::block_on(source->cut("base"));
    ASSERT_TRUE(base) << base.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "postgres-branch-protection";
    options.admin_token = "postgres-branch-protection-admin";
    options.instance_kek = instance_kek;
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    // Branch protection, reviews and releases are forge records in the repository's workspace
    // Zetta (lake#341).
    lake::test::RecordWorkspaces workspaces(control, database_uri);
    workspaces.ready(repository->workspace_id);
    options.workspaces = workspaces.service;
    // Branch protection decisions go to the repository's workspace audit; each
    // drain() below writes what the pushes queued.
    options.audit_flush_interval = std::chrono::milliseconds(0);
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto workspace_audit = [&] {
        server.drain();
        auto context = lake::api::ApiContext{};
        context.store = control.get();
        context.workspaces = workspaces.service.get();
        context.instance_kek = options.instance_kek;
        auto records =
            lake::api::workspace_records(context, customer->id, repository->workspace_id);
        EXPECT_TRUE(records);
        std::vector<lake::audit::Event> events;
        if (!records) {
            return events;
        }
        auto listed =
            liquid::runtime::block_on(lake::control::list_workspace_audit(*records, {}, 0));
        EXPECT_TRUE(listed && *listed);
        if (listed && *listed) {
            for (auto& found : **listed) {
                events.push_back(std::move(found.event));
            }
        }
        return events;
    };
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint owner_endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = options.admin_token});
    ASSERT_TRUE(liquid::runtime::block_on(owner_endpoint.push(*source)));

    ASSERT_TRUE(liquid::runtime::block_on(source->write("reviewed.txt", sync_bytes("reviewed"))));
    const auto reviewed = liquid::runtime::block_on(source->cut("reviewed"));
    ASSERT_TRUE(reviewed) << reviewed.error().message;
    const std::string reviewed_id = liquid::to_string(reviewed->value);
    workspaces.forge(*repository, "branch_protection",
                     {{"ref", "main"},
                      {"required_review", true},
                      {"fast_forward_only", true},
                      {"allowed_pushers", json::array({"admin"})}},
                     instance_kek);

    const auto review_push = liquid::runtime::block_on(owner_endpoint.push(*source));
    ASSERT_TRUE(review_push) << review_push.error().message;
    const auto* review_refusal = reference_outcome(*review_push, "main");
    ASSERT_NE(review_refusal, nullptr);
    EXPECT_EQ(review_refusal->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(review_refusal->reason,
              "branch protection rejects main: an approved review is required");

    // An approval lake recorded, by a reviewer who is not the pusher.
    const auto repository_object = control->find_policy_object("repository", repository->id);
    ASSERT_TRUE(repository_object) << repository_object.error().message;
    const std::string reviewer = "taap:1:ed25519:branch-reviewer-" + suffix;
    ASSERT_TRUE(control->create_grant({reviewer,
                                       repository_object->id,
                                       "review",
                                       "attestation-" + reviewer,
                                       "admin",
                                       "request-" + suffix}));
    workspaces.forge(*repository,
                     "review",
                     {{"change_id", reviewed_id},
                      {"state", "approved"},
                      {"approval", {{"by", reviewer}, {"change_id", reviewed_id}}}},
                     instance_kek);
    ASSERT_TRUE(liquid::runtime::block_on(owner_endpoint.push(*source)));

    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("main", *base)));
    const auto fast_forward_push = liquid::runtime::block_on(owner_endpoint.push(*source));
    ASSERT_TRUE(fast_forward_push) << fast_forward_push.error().message;
    const auto* fast_forward_refusal = reference_outcome(*fast_forward_push, "main");
    ASSERT_NE(fast_forward_refusal, nullptr);
    EXPECT_EQ(fast_forward_refusal->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(fast_forward_refusal->reason,
              "branch protection rejects a non-fast-forward update to main");

    const auto central_events = lake::test::audit_events(*control, {.limit = 100});
    ASSERT_TRUE(central_events) << central_events.error().message;
    EXPECT_FALSE(std::ranges::any_of(*central_events, [](const auto& event) {
        return event.draft.kind.starts_with("forge.branch_protection.");
    })) << "branch protection decisions are the repository's workspace audit";
    const auto events_before_unauthorized = workspace_audit();
    const auto branch_events_before =
        std::ranges::count_if(events_before_unauthorized, [](const auto& event) {
            return event.draft.kind.starts_with("forge.branch_protection.");
        });
    EXPECT_TRUE(std::ranges::any_of(events_before_unauthorized, [](const auto& event) {
        return event.draft.kind == "forge.branch_protection.refused" &&
               event.draft.details.value("reference", "") == "main" &&
               event.draft.details.value("caller", "") == "admin" &&
               event.draft.details.value("rule", "") == "required_review";
    }));
    EXPECT_TRUE(std::ranges::any_of(events_before_unauthorized, [](const auto& event) {
        return event.draft.kind == "forge.branch_protection.allowed" &&
               event.draft.details.value("reference", "") == "main" &&
               event.draft.details.value("caller", "") == "admin";
    }));

    auto anonymous_source = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "branch-protection-anonymous"));
    ASSERT_TRUE(anonymous_source) << anonymous_source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        anonymous_source->write("anonymous.txt", sync_bytes("anonymous"))));
    ASSERT_TRUE(liquid::runtime::block_on(anonymous_source->cut("anonymous")));
    liquid::sync_endpoint anonymous_endpoint(url);
    const auto anonymous = liquid::runtime::block_on(anonymous_endpoint.push(*anonymous_source));
    ASSERT_FALSE(anonymous);
    EXPECT_EQ(anonymous.error().code, liquid::errc::unauthorized);
    const auto events_after_anonymous = workspace_audit();
    const auto branch_events_after =
        std::ranges::count_if(events_after_anonymous, [](const auto& event) {
            return event.draft.kind.starts_with("forge.branch_protection.");
        });
    EXPECT_EQ(branch_events_after, branch_events_before);
    server.stop();
}

TEST(SyncHttp, ProtectedRefusalDoesNotBlockAnUnprotectedReference) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner =
        control->create_user({"taap:1:ed25519:mixed-branch-owner", "mixed@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer = control->create_customer({"individual", owner->id, "", "mixed-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "mixed", "", "private",
                                                        lake::test::repository_workspace(
                                                            *control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "mixed-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", sync_bytes("base"))));
    const auto base = liquid::runtime::block_on(source->cut("base"));
    ASSERT_TRUE(base) << base.error().message;

    ServerOptions options;
    options.port = 0;
    options.admin_token = "mixed-admin";
    options.instance_kek = "6d697865642d696e7374616e63652d6b656b2d666f722d666f7267652d746573";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    lake::test::RecordWorkspaces workspaces(control);
    workspaces.ready(repository->workspace_id);
    options.workspaces = workspaces.service;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = options.admin_token});
    ASSERT_TRUE(liquid::runtime::block_on(endpoint.push(*source)));
    ASSERT_TRUE(liquid::runtime::block_on(source->write("candidate.txt", sync_bytes("candidate"))));
    const auto candidate = liquid::runtime::block_on(source->cut("candidate"));
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("release", *candidate)));
    workspaces.forge(*repository, "branch_protection", {{"ref", "main"}, {"required_review", true}},
                     options.instance_kek);

    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;
    ASSERT_EQ(pushed->references.size(), 2U);
    const auto* main_outcome = reference_outcome(*pushed, "main");
    const auto* release_outcome = reference_outcome(*pushed, "release");
    ASSERT_NE(main_outcome, nullptr);
    ASSERT_NE(release_outcome, nullptr);
    EXPECT_EQ(main_outcome->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(main_outcome->reason,
              "branch protection rejects main: an approved review is required");
    EXPECT_EQ(release_outcome->outcome, liquid::sync_ref_outcome_kind::applied);
    EXPECT_TRUE(release_outcome->reason.empty());
    const json references = served_refs(server.port(), repository->id, "Bearer mixed-admin");
    ASSERT_TRUE(references.is_array()) << references.dump();
    EXPECT_TRUE(std::ranges::any_of(references, [&base](const json& reference) {
        return reference.at("name") == "main" &&
               reference.at("head") == liquid::to_string(base->value);
    }));
    EXPECT_TRUE(std::ranges::any_of(references, [&candidate](const json& reference) {
        return reference.at("name") == "release" &&
               reference.at("head") == liquid::to_string(candidate->value);
    }));

    // When the repository's workspace cannot be read, its rules are unknown: every reference
    // update is refused with the reason rather than decided without them (lake#341).
    workspaces.status(repository->workspace_id, "Provisioning");
    ASSERT_TRUE(liquid::runtime::block_on(source->write("later.txt", sync_bytes("later"))));
    const auto later = liquid::runtime::block_on(source->cut("later"));
    ASSERT_TRUE(later) << later.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("release", *later)));
    const auto unreadable = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(unreadable) << unreadable.error().message;
    const auto* release_refused = reference_outcome(*unreadable, "release");
    ASSERT_NE(release_refused, nullptr);
    EXPECT_EQ(release_refused->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_NE(release_refused->reason.find("branch protection is unavailable"), std::string::npos)
        << release_refused->reason;
    const json unchanged = served_refs(server.port(), repository->id, "Bearer mixed-admin");
    ASSERT_TRUE(unchanged.is_array()) << unchanged.dump();
    EXPECT_TRUE(std::ranges::any_of(unchanged, [&candidate](const json& reference) {
        return reference.at("name") == "release" &&
               reference.at("head") == liquid::to_string(candidate->value);
    }));
    server.stop();
}

TEST(SyncHttp, AtomicProtectedRefusalReportsEveryReferenceAndLandsNothing) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = control->create_user(
        {"taap:1:ed25519:atomic-branch-owner", "atomic@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer = control->create_customer({"individual", owner->id, "", "atomic-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "atomic", "", "private",
                                                        lake::test::repository_workspace(
                                                            *control, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "atomic-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", sync_bytes("base"))));
    const auto base = liquid::runtime::block_on(source->cut("base"));
    ASSERT_TRUE(base) << base.error().message;

    ServerOptions options;
    options.port = 0;
    options.admin_token = "atomic-admin";
    options.instance_kek = "61746f6d69632d696e7374616e63652d6b656b2d666f722d666f7267652d7465";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    lake::test::RecordWorkspaces workspaces(control);
    workspaces.ready(repository->workspace_id);
    options.workspaces = workspaces.service;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = options.admin_token, .atomic = true});
    ASSERT_TRUE(liquid::runtime::block_on(endpoint.push(*source)));
    ASSERT_TRUE(liquid::runtime::block_on(source->write("candidate.txt", sync_bytes("candidate"))));
    const auto candidate = liquid::runtime::block_on(source->cut("candidate"));
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("release", *candidate)));
    workspaces.forge(*repository, "branch_protection", {{"ref", "main"}, {"required_review", true}},
                     options.instance_kek);

    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;
    ASSERT_EQ(pushed->references.size(), 2U);
    const auto* main_outcome = reference_outcome(*pushed, "main");
    const auto* release_outcome = reference_outcome(*pushed, "release");
    ASSERT_NE(main_outcome, nullptr);
    ASSERT_NE(release_outcome, nullptr);
    EXPECT_EQ(main_outcome->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(main_outcome->reason,
              "branch protection rejects main: an approved review is required");
    EXPECT_EQ(release_outcome->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(release_outcome->reason, "atomic push refused because another reference was refused");
    const json references = served_refs(server.port(), repository->id, "Bearer atomic-admin");
    ASSERT_TRUE(references.is_array()) << references.dump();
    ASSERT_EQ(references.size(), 1U);
    EXPECT_EQ(references.front().at("name"), "main");
    EXPECT_EQ(references.front().at("head"), liquid::to_string(base->value));
    server.stop();
}

// The six-digit code an authenticator app shows for a base32 secret now (RFC 6238: HMAC-SHA1,
// 30-second steps), as a person would type it (lake#773).
std::string authenticator_code(const std::string& base32_secret) {
    std::vector<unsigned char> key;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : base32_secret) {
        const auto at = std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567").find(c);
        buffer = (buffer << 5) | static_cast<std::uint32_t>(at);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            key.push_back(static_cast<unsigned char>((buffer >> bits) & 0xff));
        }
    }
    std::uint64_t counter = static_cast<std::uint64_t>(std::time(nullptr)) / 30;
    unsigned char message[8];
    for (int i = 7; i >= 0; --i) {
        message[i] = static_cast<unsigned char>(counter & 0xff);
        counter >>= 8;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha1(),
         key.data(),
         static_cast<int>(key.size()),
         message,
         sizeof(message),
         digest,
         &size);
    const int offset = digest[size - 1] & 0x0f;
    const std::uint32_t binary = (static_cast<std::uint32_t>(digest[offset] & 0x7f) << 24) |
                                 (static_cast<std::uint32_t>(digest[offset + 1]) << 16) |
                                 (static_cast<std::uint32_t>(digest[offset + 2]) << 8) |
                                 static_cast<std::uint32_t>(digest[offset + 3]);
    return std::format("{:06}", binary % 1000000);
}

TEST(Identity, PasswordSessionsTokensAndRevocation) {
    Surface s;
    const ApiResult sign_up = s.call("sign_up_password",
                                     {{"email", "ada@example.com"},
                                      {"password", "correct horse battery"},
                                      {"identity_document", "{}"}});
    ASSERT_TRUE(sign_up.ok) << sign_up.error.message;
    const std::string browser_secret = sign_up.value["secret"];
    const std::string user_id = sign_up.value["session"]["user_id"];

    auto principal = s.store.context_for(browser_secret);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->user_id, user_id);

    const ApiResult provider = s.call("register_oauth_provider",
                                      {{"provider", "stub"},
                                       {"issuer", "https://stub.example.test"},
                                       {"client_id", "lake-test"},
                                       {"client_secret", "test-client-secret"},
                                       {"scopes", json::array({"openid", "email"})}});
    ASSERT_TRUE(provider.ok) << provider.error.message;
    ApiContext user_context;
    user_context.store = &s.store;
    user_context.authorized = true;
    user_context.acting_user_id = principal->user_id;
    user_context.acting_identity = principal->taap_id;
    user_context.instance_kek = s.ctx.instance_kek;
    user_context.session_credential = true;
    user_context.scopes = principal->scopes;
    user_context.credential_session_id = principal->session_id;
    user_context.credential_kind = principal->kind;
    user_context.credential_expires_at_unix = principal->expires_at_unix;

    const ApiResult enrollment = *liquid::runtime::block_on(
        s.registry.call_async("enroll_mfa", {{"password", "correct horse battery"}}, user_context));
    ASSERT_TRUE(enrollment.ok) << enrollment.error.message;
    const std::string uri = enrollment.value["otpauth_uri"];
    EXPECT_TRUE(uri.starts_with("otpauth://totp/lake:"));
    // lake#773: the URI's secret is base32, and the code an authenticator computes from it signs
    // in.
    const std::size_t secret_at = uri.find("secret=") + 7;
    const std::string secret = uri.substr(secret_at, uri.find('&', secret_at) - secret_at);
    ASSERT_FALSE(secret.empty());
    EXPECT_EQ(secret.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"), std::string::npos)
        << secret;
    const auto refused = *liquid::runtime::block_on(
        s.registry.call_async("enroll_mfa", json::object(), user_context));
    EXPECT_FALSE(refused.ok);
    const auto wrong_password = *liquid::runtime::block_on(
        s.registry.call_async("enroll_mfa", {{"password", "wrong password"}}, user_context));
    EXPECT_FALSE(wrong_password.ok);
    // Refused replacements leave the original authenticator's seed usable.
    const ApiResult with_authenticator = s.call("verify_mfa",
                                                {{"email", "ada@example.com"},
                                                 {"password", "correct horse battery"},
                                                 {"code", authenticator_code(secret)}});
    ASSERT_TRUE(with_authenticator.ok) << with_authenticator.error.message;
    ASSERT_EQ(enrollment.value["recovery_codes"].size(), 8);
    EXPECT_FALSE(enrollment.value.contains("secret"));
    const std::string first_recovery = enrollment.value["recovery_codes"][0];
    const std::string second_recovery = enrollment.value["recovery_codes"][1];

    const json sign_in_input = {{"email", "ada@example.com"},
                                {"password", "correct horse battery"}};
    const ApiResult mfa_required = s.call("sign_in_password", sign_in_input);
    EXPECT_EQ(mfa_required.error.code, "mfa_required");
    const ApiResult verified = s.call("verify_mfa",
                                      {{"email", "ada@example.com"},
                                       {"password", "correct horse battery"},
                                       {"code", first_recovery}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_TRUE(s.store.context_for(verified.value["secret"]));
    const ApiResult reused = s.call("verify_mfa",
                                    {{"email", "ada@example.com"},
                                     {"password", "correct horse battery"},
                                     {"code", first_recovery}});
    EXPECT_FALSE(reused.ok);

    const ApiResult stale_disable = *liquid::runtime::block_on(
        s.registry.call_async("disable_mfa", {{"code", first_recovery}}, user_context));
    // Credential failures share the generic 401 (lake#966).
    EXPECT_EQ(stale_disable.error.code, "unauthenticated");
    const ApiResult disabled = *liquid::runtime::block_on(
        s.registry.call_async("disable_mfa", {{"code", second_recovery}}, user_context));
    ASSERT_TRUE(disabled.ok) << disabled.error.message;
    EXPECT_TRUE(s.call("sign_in_password", sign_in_input).ok);

    const ApiResult token = *liquid::runtime::block_on(s.registry.call_async(
        "create_token",
        {{"name", "read sessions"}, {"scopes", json::array({"list_sessions"})}},
        user_context));
    ASSERT_TRUE(token.ok) << token.error.message;
    EXPECT_FALSE(token.value["session"]["expires_at"].get<std::string>().empty());

    auto token_principal = s.store.context_for(token.value["secret"]);
    ASSERT_TRUE(token_principal);
    ApiContext token_context;
    token_context.store = &s.store;
    token_context.authorized = true;
    token_context.acting_user_id = token_principal->user_id;
    token_context.acting_identity = token_principal->taap_id;
    token_context.instance_kek = s.ctx.instance_kek;
    token_context.session_credential = true;
    token_context.scopes = token_principal->scopes;
    EXPECT_TRUE(s.registry.call("list_sessions", json::object(), token_context).ok);
    EXPECT_EQ(liquid::runtime::block_on(
                  s.registry.call_async(
                      "create_token", {{"name", "no"}, {"scopes", json::array()}}, token_context))
                  ->error.code,
              "unauthorized");

    const std::string session_id = sign_up.value["session"]["id"];
    ASSERT_TRUE(s.store.revoke_session(user_id, session_id));
    EXPECT_FALSE(s.store.context_for(browser_secret));
    const ApiResult static_without_reason = *liquid::runtime::block_on(s.registry.call_async(
        "create_token",
        {{"name", "legacy"}, {"scopes", json::array({"list_sessions"})}, {"static", true}},
        user_context));
    EXPECT_EQ(static_without_reason.error.code, "invalid_input");
    const ApiResult static_token = *liquid::runtime::block_on(
        s.registry.call_async("create_token",
                              {{"name", "legacy"},
                               {"scopes", json::array({"list_sessions"})},
                               {"static", true},
                               {"reason", "migration"}},
                              user_context));
    EXPECT_FALSE(static_token.ok);
    EXPECT_EQ(static_token.error.code, "unauthorized");
    // Signing out never ends a key made from that sign-in (lake#1163).
    EXPECT_TRUE(s.store.context_for(token.value["secret"]));
    auto audit_events = lake::test::audit_events(s.store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(audit_events);
    EXPECT_TRUE(std::ranges::any_of(*audit_events, [](const lake::audit::Event& event) {
        return event.draft.action == "create_token" && event.draft.outcome == "success";
    }));
}

// --- API consistency (lake#450, docs/api/errors.md, docs/api/naming.md) -------------------------

TEST(ApiErrors, EveryReturnedCodeIsDocumented) {
    // Every error code literal in the source is in error_codes(), or is a spelling
    // ApiResult::failure maps onto one, or never leaves the store (`expired` becomes
    // ApiContext::credential_rejection). The table is also the doc's.
    const std::set<std::string> mapped = {
        "invalid_argument", "unsupported", "unavailable", "internal", "expired"};
    const std::regex literal(R"re((?:failure|ApiError\s*\{|StoreError\s*\{|SyncStoreError\s*\{|)re"
                             R"re(\bError\s*\{|ZettaControlError\s*\{|error_body)\s*\(?\s*\{?\s*)re"
                             R"re("([a-z][a-z_]*)"\s*,)re");
    std::set<std::string> found;
    const std::filesystem::path root(LAKE_TEST_SOURCE_DIR);
    for (const auto* directory : {"src", "include"}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / directory)) {
            const auto extension = entry.path().extension();
            if (extension != ".cpp" && extension != ".hpp") {
                continue;
            }
            std::ifstream file(entry.path());
            const std::string text((std::istreambuf_iterator<char>(file)), {});
            for (auto match = std::sregex_iterator(text.begin(), text.end(), literal);
                 match != std::sregex_iterator();
                 ++match) {
                found.insert((*match)[1].str());
            }
        }
    }
    EXPECT_TRUE(found.contains("invalid_input")) << "the scan finds the codes it is meant to";
    for (const std::string& code : found) {
        EXPECT_TRUE(lake::api::find_error_code(code) != nullptr || mapped.contains(code))
            << code << " is returned but is not in error_codes() (docs/api/errors.md)";
    }
    std::ifstream doc(root / "docs/api/errors.md");
    const std::string errors((std::istreambuf_iterator<char>(doc)), {});
    for (const lake::api::ErrorCode& code : lake::api::error_codes()) {
        EXPECT_NE(errors.find("| `" + std::string(code.code) + "` | " +
                              std::to_string(code.status) + " |"),
                  std::string::npos)
            << code.code << " is missing from docs/api/errors.md";
    }
}

TEST(ApiErrors, OtherSpellingsAndUnknownCodesBecomeDocumentedOnes) {
    EXPECT_EQ(ApiResult::failure("invalid_argument", "x").error.code, "invalid_input");
    EXPECT_EQ(ApiResult::failure("unsupported", "x").error.code, "not_supported");
    EXPECT_EQ(ApiResult::failure("unavailable", "x").error.code, "store_unavailable");
    const ApiResult odd = ApiResult::failure("made_up", "something broke", "/a");
    EXPECT_EQ(odd.error.code, "store_error");
    EXPECT_EQ(odd.error.message, "made_up: something broke");
    EXPECT_EQ(rest_status(ApiResult::failure("unauthenticated", "x")), 401);
    EXPECT_EQ(rest_status(ApiResult::failure("unauthorized", "x")), 403);
    EXPECT_EQ(rest_status(ApiResult::failure("method_not_allowed", "x")), 405);
    EXPECT_EQ(rest_status(ApiResult::failure("not_approved", "x")), 409);
    EXPECT_EQ(rest_status(ApiResult::failure("engine_error", "x")), 422);
    EXPECT_EQ(rest_status(ApiResult::failure("workspace_not_ready", "x")), 503);
    // The OpenAPI error envelope enumerates exactly the documented codes.
    const json schema = lake::api::error_envelope_schema();
    EXPECT_EQ(schema["properties"]["error"]["properties"]["code"]["enum"].size(),
              lake::api::error_codes().size());
}

// Cloudflare answers a 502 or 504 from lake with its own page, so a REST caller would never see
// lake's error (lake#572). No documented code may answer either.
TEST(ApiErrors, NoCodeAnswersAStatusCloudflareReplaces) {
    for (const auto& code : lake::api::error_codes()) {
        EXPECT_NE(code.status, 502) << code.code;
        EXPECT_NE(code.status, 504) << code.code;
    }
    const ApiResult refused =
        ApiResult::failure("engine_error", "table \"no_such_table\" does not exist");
    EXPECT_EQ(rest_status(refused), 422);
    const json body = lake::api::rest_envelope(refused);
    EXPECT_EQ(body["error"]["code"], "engine_error");
    EXPECT_EQ(body["error"]["message"], "table \"no_such_table\" does not exist");
}

// A database constraint the input broke is said as the rule, at the field (lake#592): never "the
// request contains invalid data" with an empty path, and never the constraint's name.
TEST(ApiErrors, ABrokenConstraintIsSaidAsItsRuleAtItsField) {
    const ApiResult named = ApiResult::failure(
        "invalid_input",
        "ERROR:  new row for relation \"repositories\" violates check constraint "
        "\"repository_name_format\"",
        "/repository_name");
    EXPECT_EQ(named.error.path, "/repository_name");
    EXPECT_EQ(named.error.message,
              "repository_name is 1 to 256 lowercase letters, digits and hyphens, starting and "
              "ending with a letter or digit");
    const ApiResult unnamed =
        ApiResult::failure("invalid_input",
                           "ERROR:  new row for relation \"grants\" violates check constraint "
                           "\"grants_attestation_check\"");
    EXPECT_EQ(unnamed.error.path, "/attestation_digest");
    EXPECT_TRUE(unnamed.error.message.starts_with("attestation_digest is required"));
    const ApiResult taken = ApiResult::failure(
        "conflict",
        "ERROR:  duplicate key value violates unique constraint "
        "\"grants_principal_object_role_unique\"\nDETAIL:  Key (…) already exists.");
    EXPECT_EQ(taken.error.path, "/principal_taap_id");
    EXPECT_NE(taken.error.message.find("already shared with that person"), std::string::npos);
    // The in-memory store names the same constraints without a diagnostic.
    const ApiResult memory =
        ApiResult::failure("invalid_input", "name must match repository_name_format");
    EXPECT_EQ(memory.error.path, "/name");
    EXPECT_TRUE(memory.error.message.starts_with("name is 1 to 256 lowercase"));
    // An unknown diagnostic still never leaks.
    const ApiResult other =
        ApiResult::failure("invalid_input", "ERROR:  something \"secret_table\" DETAIL: x");
    EXPECT_EQ(other.error.message, "the request contains invalid data");
}

TEST(ApiErrors, RestSaysWhetherACredentialIsMissingInvalidOrExpired) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user =
        store->create_user({"taap:1:ed25519:expiring", "expiring@example.test", "Expiring"});
    ASSERT_TRUE(user) << user.error().message;
    const auto token =
        store->create_token({user->id, "lake", "short", "test", {"list_customers"}, 1, false});
    ASSERT_TRUE(token) << token.error().message;
    const auto live =
        store->create_token({user->id, "lake", "live", "test", {"list_customers"}, 600, false});
    ASSERT_TRUE(live) << live.error().message;
    ServerOptions options;
    options.port = 0;
    options.node_name = "credential-test";
    options.admin_token = "credential-admin";
    options.store = store;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto body = [](const std::string& response) {
        return json::parse(response.substr(response.find("\r\n\r\n") + 4));
    };

    const std::string missing = http_get(server.port(), "/v1/list_customers");
    EXPECT_NE(missing.find(" 401 "), std::string::npos) << missing;
    EXPECT_EQ(body(missing)["error"]["code"], "unauthenticated");
    EXPECT_EQ(body(missing)["error"]["reason"], "missing");
    EXPECT_EQ(body(missing)["error"]["help"], "http://localhost/developer/mcp");
    EXPECT_EQ(response_header(missing, "WWW-Authenticate"),
              "Bearer realm=\"lake\", error_uri=\"http://localhost/developer/mcp\"");

    const std::string invalid =
        http_get(server.port(), "/v1/list_customers", "Bearer not-a-lake-credential");
    EXPECT_NE(invalid.find(" 401 "), std::string::npos) << invalid;
    EXPECT_EQ(body(invalid)["error"]["reason"], "invalid");
    EXPECT_NE(response_header(invalid, "WWW-Authenticate").find("error=\"invalid_token\""),
              std::string::npos);

    // viewer needs no key, but a rejected one is refused there too (lake#594); a visitor with
    // none is answered signed_in: false.
    const std::string bad_viewer =
        http_get(server.port(), "/v1/viewer", "Bearer not-a-lake-credential");
    EXPECT_NE(bad_viewer.find(" 401 "), std::string::npos) << bad_viewer;
    EXPECT_EQ(body(bad_viewer)["error"]["reason"], "invalid");
    const std::string visitor = http_get(server.port(), "/v1/viewer");
    EXPECT_NE(visitor.find(" 200 "), std::string::npos) << visitor;
    // lake#775: the same on a capability a visitor may call: a rejected key is 401, not a
    // visitor's empty answer; no key at all is still a visitor.
    for (const char* bearer : {"Bearer not-a-lake-credential",
                               "Bearer ls1.00000000-0000-4000-8000-000000000000.not-a-secret"}) {
        const std::string listed = http_get(server.port(), "/v1/list_repositories", bearer);
        EXPECT_NE(listed.find(" 401 "), std::string::npos) << bearer << "\n" << listed;
        EXPECT_EQ(body(listed)["error"]["reason"], "invalid");
    }
    const std::string public_list = http_get(server.port(), "/v1/list_repositories");
    EXPECT_NE(public_list.find(" 200 "), std::string::npos) << public_list;
    EXPECT_EQ(body(visitor)["value"]["signed_in"], false);
    EXPECT_FALSE(body(visitor)["value"]["config"].contains("base_agent_model"));

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const std::string expired =
        http_get(server.port(), "/v1/list_customers", "Bearer " + token->secret);
    EXPECT_NE(expired.find(" 401 "), std::string::npos) << expired;
    EXPECT_EQ(body(expired)["error"]["reason"], "expired");
    EXPECT_NE(body(expired)["error"]["message"].get<std::string>().find("expired"),
              std::string::npos);

    // A valid credential that does not grant the call is still 403.
    const std::string refused = http_get(server.port(), "/v1/list_users", "Bearer " + live->secret);
    EXPECT_NE(refused.find(" 403 "), std::string::npos) << refused;
    EXPECT_EQ(body(refused)["error"]["code"], "unauthorized");

    // A public capability is unaffected, and an empty query key names no field.
    for (const char* path : {"/v1/server_info?", "/v1/server_info?&", "/v1/server_info?="}) {
        const std::string info = http_get(server.port(), path);
        EXPECT_NE(info.find(" 200 "), std::string::npos) << path << "\n" << info;
    }
    server.stop();
}

TEST(ApiErrors, InputRefusalsPointAtTheField) {
    Surface s;
    const ApiResult token =
        s.call("create_token", {{"name", "k"}, {"scopes", json::array({"*"})}, {"static", true}});
    ASSERT_FALSE(token.ok);
    EXPECT_EQ(token.error.path, "/reason");
    // A handler that names the field first but gives no path gets it from the registry.
    Capability capability;
    capability.name = "check_confirm";
    capability.description = "A test capability that refuses its confirmation.";
    capability.input_schema = {{"type", "object"},
                               {"properties", {{"confirm", {{"type", "string"}}}}},
                               {"additionalProperties", false}};
    capability.requires_auth = false;
    capability.handler = [](const json&, ApiContext&) {
        return ApiResult::failure("invalid_input", "confirm must be the service name");
    };
    Registry registry;
    ASSERT_TRUE(registry.add(capability));
    ApiContext context;
    const ApiResult refused = registry.call("check_confirm", {{"confirm", "x"}}, context);
    EXPECT_EQ(refused.error.path, "/confirm");
}

TEST(ApiNaming, TokenScopesAreCapabilityNamesStarOrRegistryPull) {
    Surface s;
    const ApiResult nonsense =
        s.call("create_token",
               {{"name", "k"}, {"scopes", json::array({"list_customers", "nonsense:scope"})}});
    ASSERT_FALSE(nonsense.ok);
    EXPECT_EQ(nonsense.error.code, "invalid_input");
    EXPECT_EQ(nonsense.error.path, "/scopes/1");
    EXPECT_NE(nonsense.error.message.find("nonsense:scope"), std::string::npos);
    const ApiResult named =
        s.call("create_token",
               {{"name", "k"}, {"scopes", json::array({"list_customers", "registry_pull"})}});
    // The operator issues no token of its own, but the scopes pass validation first.
    if (!named.ok) {
        EXPECT_NE(named.error.path, "/scopes/0");
        EXPECT_NE(named.error.path, "/scopes/1");
    }
}

TEST(ApiNaming, GetCustomerNamesTheSpaceCustomerIdAndKeepsIdAsADeprecatedAlias) {
    Surface s;
    const ApiResult user =
        s.call("create_user", {{"email", "naming@example.test"}, {"display_name", "Naming"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user.value["id"]}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string id = customer.value["id"];
    EXPECT_TRUE(s.call("get_customer", {{"customer_id", id}}).ok);
    EXPECT_TRUE(s.call("get_customer", {{"id", id}}).ok) << "the deprecated alias still works";
    const ApiResult both = s.call("get_customer", {{"id", id}, {"customer_id", "other"}});
    EXPECT_EQ(both.error.code, "invalid_input");
    EXPECT_EQ(both.error.path, "/id");
    const ApiResult missing = s.call("get_customer", {{"customer_id", "missing"}});
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_EQ(missing.error.path, "/customer_id");
    // The schema shows the alias as deprecated, so OpenAPI and MCP document it.
    const json properties = s.registry.find("get_customer")->input_schema["properties"];
    EXPECT_TRUE(properties["id"].value("deprecated", false));
    EXPECT_EQ(s.registry.find("get_customer")->input_schema["required"],
              json::array({"customer_id"}));
}

TEST(ApiNaming, OneNamePerConceptAcrossCapabilities) {
    Surface s;
    const auto has = [&](const std::string& capability, const std::string& field) {
        const auto found = s.registry.find(capability);
        return found && found->input_schema["properties"].contains(field) &&
               !found->input_schema["properties"][field].value("deprecated", false);
    };
    for (const auto& [capability, field] :
         std::vector<std::pair<std::string, std::string>>{{"predict", "model_id"},
                                                          {"search_index", "index_id"},
                                                          {"get_run_metrics", "run_id"},
                                                          {"stop_experiment_trial", "run_id"},
                                                          {"promote_trial", "run_id"},
                                                          {"promote_trial", "experiment_id"},
                                                          {"get_experiment", "experiment_id"},
                                                          {"stop_experiment", "experiment_id"},
                                                          {"get_customer", "customer_id"}}) {
        EXPECT_TRUE(has(capability, field)) << capability << " names it " << field;
    }
    // No capability takes a concept under a non-canonical name except as a deprecated alias.
    for (const auto& capability : s.registry.list()) {
        for (const auto& [field, schema] :
             capability->input_schema.value("properties", json::object()).items()) {
            if ((field == "run" || field == "model" || field == "index") &&
                schema.value("type", json()) == "string") {
                EXPECT_TRUE(schema.value("deprecated", false))
                    << capability->name << " takes `" << field << "`; use `" << field << "_id`";
            }
            // An experiment is the engine's study; lake never names it so (lake#561).
            EXPECT_EQ(field.find("study"), std::string::npos)
                << capability->name << " takes `" << field << "`; use `experiment_id`";
        }
    }
}

TEST(ApiDescriptions, EveryCapabilitySaysWhatItDoesForAPerson) {
    Surface s;
    std::map<std::string, std::string> seen;
    for (const auto& capability : s.registry.list()) {
        const std::string& text = capability->description;
        EXPECT_GE(text.size(), 40u) << capability->name << ": \"" << text << "\" is too thin";
        EXPECT_FALSE(text.starts_with("Manage")) << capability->name << ": say what it does";
        for (const char* vague : {"durable forge record",
                                  "desired lifecycle state",
                                  "after explicit confirmation in this space"}) {
            EXPECT_EQ(text.find(vague), std::string::npos) << capability->name << ": " << text;
        }
        // A person runs experiments; "study" is only the engine's name, in a code span such as
        // `validate_study` (lake#561).
        static const std::regex study(R"((^|[^_`.-])\b[Ss]tud(y|ies)\b(?![_-]))");
        EXPECT_FALSE(std::regex_search(text, study))
            << capability->name << " says study: \"" << text << "\"";
        EXPECT_FALSE(std::regex_search(capability->title, study)) << capability->name;
        const auto [previous, inserted] = seen.emplace(text, capability->name);
        EXPECT_TRUE(inserted) << capability->name << " shares its description with "
                              << previous->second << ": \"" << text << "\"";
    }
}

namespace {

// lake#561: the reads the web app needs that the server-rendered pages computed themselves.
TEST(WebApp, PermissionsDocsMcpAndPackagesAreCapabilities) {
    Surface s;
    const ApiResult owner =
        s.call("create_user", {{"taap_id", "taap:1:ed25519:web-owner"}, {"display_name", "Owner"}});
    ASSERT_TRUE(owner.ok);
    const ApiResult customer = s.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "web-owner"}});
    ASSERT_TRUE(customer.ok);
    const std::string customer_id = customer.value["id"];
    const json every{"read", "write", "admin", "promote"};
    const ApiResult operator_may =
        s.call("my_permissions",
               {{"object_type", "customer"}, {"object_id", customer_id}, {"actions", every}});
    ASSERT_TRUE(operator_may.ok) << operator_may.error.message;
    for (const std::string action : {"read", "write", "admin", "promote"}) {
        EXPECT_EQ(operator_may.value["allowed"][action], true) << action;
    }
    EXPECT_EQ(s.call("my_permissions",
                     {{"object_type", "customer"}, {"object_id", "nope"}, {"actions", every}})
                  .error.code,
              "not_found");
    EXPECT_EQ(
        s.call("my_permissions",
               {{"object_type", "customer"}, {"object_id", customer_id}, {"actions", {"fly"}}})
            .error.code,
        "invalid_input");
    // A workspace answers its space's permissions; a drive says to ask about its workspace;
    // an unknown type lists the ones it takes (lake#594).
    const auto workspace =
        s.store.create_workspace({customer_id, "analytics", "dev", "taap:1:ed25519:web-owner"});
    ASSERT_TRUE(workspace);
    const ApiResult on_workspace =
        s.call("my_permissions",
               {{"object_type", "workspace"}, {"object_id", workspace->id}, {"actions", every}});
    ASSERT_TRUE(on_workspace.ok) << on_workspace.error.message;
    EXPECT_EQ(on_workspace.value["allowed"]["write"], true);
    const ApiResult on_drive =
        s.call("my_permissions",
               {{"object_type", "drive"}, {"object_id", workspace->id}, {"actions", every}});
    EXPECT_EQ(on_drive.error.code, "invalid_input");
    EXPECT_EQ(on_drive.error.path, "/object_type");
    const ApiResult odd = s.call(
        "my_permissions", {{"object_type", "planet"}, {"object_id", "x"}, {"actions", every}});
    EXPECT_EQ(odd.error.path, "/object_type");
    EXPECT_NE(odd.error.message.find("\"workspace\""), std::string::npos);

    const auto writer = s.store.create_user({"taap:1:ed25519:web-writer", "", "Writer"});
    ASSERT_TRUE(writer);
    const auto object = s.store.find_policy_object("customer", customer_id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(s.store.create_grant(
        {writer->taap_id, object->id, "write", "web-writer-attestation", "admin", "web-writer"}));
    s.ctx.acting_identity = writer->taap_id;
    s.ctx.acting_user_id = writer->id;
    s.ctx.operator_credential = false;
    s.ctx.session_credential = true;
    s.ctx.scopes = {"*"};
    const ApiResult writer_may =
        s.call("my_permissions",
               {{"object_type", "customer"}, {"object_id", customer_id}, {"actions", every}});
    ASSERT_TRUE(writer_may.ok) << writer_may.error.message;
    EXPECT_EQ(writer_may.value["allowed"],
              (json{{"read", true}, {"write", true}, {"admin", false}, {"promote", false}}));

    // Docs are public, in reading order.
    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    const ApiResult pages = s.call("list_docs");
    ASSERT_TRUE(pages.ok) << pages.error.message;
    // The Billing guide is absent where billing is off, as in this registry (lake#564).
    std::vector<WebUiGuidePage> guide;
    for (const WebUiGuidePage& page : web_ui_guide()) {
        if (page.slug != "billing") {
            guide.push_back(page);
        }
    }
    ASSERT_EQ(pages.value["pages"].size(), guide.size());
    ASSERT_GE(guide.size(), 2U);
    EXPECT_EQ(pages.value["pages"][0]["slug"], guide.front().slug);
    const ApiResult first = s.call("get_doc");
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(first.value["slug"], guide.front().slug);
    EXPECT_EQ(first.value["markdown"], guide.front().markdown);
    EXPECT_EQ(first.value["next"]["slug"], guide.at(1).slug);
    EXPECT_EQ(first.value["section"], guide.front().section);
    EXPECT_FALSE(first.value.contains("previous"));
    // get_doc takes the `slug` list_docs answers; `page` is its deprecated name (lake#591).
    const ApiResult by_slug = s.call("get_doc", {{"slug", guide.at(1).slug}});
    ASSERT_TRUE(by_slug.ok) << by_slug.error.message;
    EXPECT_EQ(by_slug.value["slug"], guide.at(1).slug);
    EXPECT_EQ(s.call("get_doc", {{"slug", "no-such-guide"}}).error.path, "/slug");
    const ApiResult last = s.call("get_doc", {{"page", guide.back().slug}});
    ASSERT_TRUE(last.ok);
    EXPECT_FALSE(last.value.contains("next"));
    EXPECT_EQ(last.value["previous"]["slug"], guide.at(guide.size() - 2).slug);
    EXPECT_EQ(s.call("get_doc", {{"page", "no-such-guide"}}).error.code, "not_found");

    // lake#561: sections in reading order, each page under its section with its headings.
    ASSERT_TRUE(pages.value["sections"].is_array());
    EXPECT_EQ(pages.value["sections"][0], guide.front().section);
    std::vector<std::string> sections;
    for (const auto& page : guide) {
        if (sections.empty() || sections.back() != page.section) {
            sections.emplace_back(page.section);
        }
    }
    EXPECT_EQ(pages.value["sections"], json(sections));
    for (std::size_t i = 0; i < guide.size(); ++i) {
        EXPECT_EQ(pages.value["pages"][i]["slug"], guide[i].slug);
        EXPECT_EQ(pages.value["pages"][i]["section"], guide[i].section);
        EXPECT_TRUE(pages.value["pages"][i]["headings"].is_array());
    }
    // A heading of a page anchors as the renderer does; `q` finds it and says where.
    const json train = *std::ranges::find_if(pages.value["pages"], [](const json& page) {
        return page["slug"] == "train";
    });
    ASSERT_FALSE(train["headings"].empty());
    const json heading = train["headings"][0];
    EXPECT_NE(markdown_to_html(std::string(std::ranges::find(guide, "train", &WebUiGuidePage::slug)
                                               ->markdown))
                  .find("id=\"" + heading["anchor"].get<std::string>() + "\""),
              std::string::npos);
    const ApiResult found = s.call("list_docs", {{"q", heading["text"]}});
    ASSERT_TRUE(found.ok) << found.error.message;
    ASSERT_FALSE(found.value["pages"].empty());
    const json hit = *std::ranges::find_if(found.value["pages"], [](const json& page) {
        return page["slug"] == "train";
    });
    EXPECT_NE(hit["match"], "text");
    const ApiResult text = s.call("list_docs", {{"q", "HELD_OUT_AUC"}});
    ASSERT_TRUE(text.ok);
    ASSERT_FALSE(text.value["pages"].empty());
    EXPECT_TRUE(text.value["pages"][0].contains("excerpt"));
    EXPECT_NE(text.value["pages"][0]["excerpt"].get<std::string>().find("held_out_auc"),
              std::string::npos);
    EXPECT_TRUE(s.call("list_docs", {{"q", "no guide says zqxwv"}}).value["pages"].empty());
    // Every word, in any order (lake#594): the words of a heading, reversed, still find it.
    const ApiResult words = s.call("list_docs", {{"q", "csv upload"}});
    ASSERT_TRUE(words.ok) << words.error.message;
    EXPECT_FALSE(words.value["pages"].empty());
    for (const json& page : words.value["pages"]) {
        std::string body(
            std::ranges::find(guide, page["slug"].get<std::string>(), &WebUiGuidePage::slug)
                ->markdown);
        std::ranges::transform(
            body, body.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        EXPECT_NE(body.find("csv"), std::string::npos) << page["slug"];
        EXPECT_NE(body.find("upload"), std::string::npos) << page["slug"];
    }
    EXPECT_TRUE(s.call("list_docs", {{"q", "upload zqxwv"}}).value["pages"].empty());
    EXPECT_EQ(s.call("list_docs", {{"q", std::string(201, 'a')}}).error.path, "/q");
    EXPECT_EQ(s.call("mcp_setup").error.code, "unauthenticated");

    s.ctx.authorized = true;
    s.ctx.acting_identity = writer->taap_id;
    s.ctx.external_origin = "https://lake.example.test";
    const ApiResult setup = s.call("mcp_setup");
    ASSERT_TRUE(setup.ok) << setup.error.message;
    EXPECT_EQ(setup.value["mcp_url"], "https://lake.example.test/mcp");
    EXPECT_FALSE(setup.value["tool_groups"].empty());
    EXPECT_FALSE(setup.value["flows"].empty());
    EXPECT_EQ(setup.value["scopes"]["all"].size(), s.registry.list().size());
    const json& read_only = setup.value["scopes"]["read_only"];
    EXPECT_NE(std::ranges::find(read_only, "list_customers"), read_only.end());
    EXPECT_EQ(std::ranges::find(read_only, "create_customer"), read_only.end());
    std::size_t grouped = 0;
    for (const json& group : setup.value["tool_groups"]) {
        grouped += group["tools"].size();
    }
    EXPECT_GT(grouped, 0U);

    const ApiResult none = s.call("list_packages");
    ASSERT_TRUE(none.ok) << none.error.message;
    EXPECT_EQ(none.value["available"], false);
    EXPECT_EQ(none.value["origin"], "https://lake.example.test");
    s.ctx.artifact_catalog = [] {
        return json{{"available", true},
                    {"npm", {{{"name", "left-pad"}, {"versions", {"1.0.0"}}}}},
                    {"python", json::array()},
                    {"oci", {{{"name", "base"}, {"versions", {"latest"}}}}}};
    };
    const ApiResult packages = s.call("list_packages");
    ASSERT_TRUE(packages.ok) << packages.error.message;
    EXPECT_EQ(packages.value["available"], true);
    EXPECT_EQ(packages.value["npm"][0]["name"], "left-pad");
    EXPECT_EQ(packages.value["oci"][0]["versions"][0], "latest");
}

// lake#1098: under Clerk, Clerk is the only way into the Lake pool. The password entries refuse by
// the same rule that makes viewer say password_sign_up false, and work again without Clerk.
TEST(Identity, ClerkTurnsOffLakePoolPasswordSignUpAndSignIn) {
    Surface s;
    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    s.ctx.operator_credential = false;
    const json ada = {{"email", "ada@example.com"}, {"password", "correct horse battery"}};
    const json outsider = {{"email", "outsider@example.com"},
                           {"password", "correct horse battery"}};

    // Without Clerk: Lake's own sign-up and sign-in.
    EXPECT_EQ(s.call("viewer").value["password_sign_up"], true);
    const ApiResult signed_up = s.call("sign_up_password", ada);
    ASSERT_TRUE(signed_up.ok) << signed_up.error.message;
    ASSERT_TRUE(s.call("sign_in_password", ada).ok);

    s.ctx.clerk_publishable_key = "pk_test_ZXhhbXBsZS0xMjMuY2xlcmsuYWNjb3VudHMuZGV2JA==";
    EXPECT_EQ(s.call("viewer").value["password_sign_up"], false);
    const auto refused = [&](const std::string& capability, json input) {
        const ApiResult result = s.call(capability, std::move(input));
        EXPECT_FALSE(result.ok) << capability;
        EXPECT_EQ(result.error.code, "forbidden") << capability << ": " << result.error.message;
        EXPECT_FALSE(result.value.contains("secret")) << capability;
    };
    refused("sign_up_password", outsider);
    refused("sign_up_password",
            json{{"pool_id", "lake"},
                 {"email", "x@example.com"},
                 {"password", "correct horse battery"}});
    refused("sign_in_password", ada);
    json mfa = ada;
    mfa["code"] = "000000";
    refused("verify_mfa", mfa);
    refused("recover_with_code", json{{"email", "ada@example.com"}, {"code", "abcd-efgh"}});
    // Application pools keep their own password sign-in: the rule is the Lake pool's only.
    const ApiResult application = s.call("sign_in_password",
                                         json{{"pool_id", "app-unknown"},
                                              {"email", "ada@example.com"},
                                              {"password", "correct horse battery"}});
    EXPECT_FALSE(application.ok);
    EXPECT_NE(application.error.code, "forbidden") << application.error.message;

    // The refusal changed nothing: the existing account signs in again once Clerk is off, and the
    // refused sign-up created no account.
    s.ctx.clerk_publishable_key.clear();
    EXPECT_EQ(s.call("viewer").value["password_sign_up"], true);
    EXPECT_TRUE(s.call("sign_in_password", ada).ok);
    EXPECT_EQ(s.call("sign_in_password", outsider).error.code, "unauthenticated");
}

// Sign-up and sign-in link to the instance's terms and privacy policy: viewer
// reports them to anyone, signed in or not, and an empty URL means no link.
TEST(WebApp, ViewerReportsTheTermsAndPrivacyLinks) {
    Surface s;
    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    s.ctx.operator_credential = false;
    s.ctx.terms_url = "https://example.test/terms";
    s.ctx.privacy_url = "https://example.test/privacy";
    const ApiResult visitor = s.call("viewer");
    ASSERT_TRUE(visitor.ok) << visitor.error.message;
    EXPECT_EQ(visitor.value["legal"]["terms_url"], "https://example.test/terms");
    EXPECT_EQ(visitor.value["legal"]["privacy_url"], "https://example.test/privacy");

    s.ctx.terms_url.clear();
    s.ctx.privacy_url.clear();
    const ApiResult none = s.call("viewer");
    ASSERT_TRUE(none.ok) << none.error.message;
    EXPECT_EQ(none.value["legal"], (json{{"terms_url", ""}, {"privacy_url", ""}}));
}

TEST(WebApp, LegalUrlSettingsKeepTheDefaultTurnOffOrRefuse) {
    using lake::api::legal_url_setting;
    // No legal link of lake's own: an install names its own pages, or shows none.
    EXPECT_EQ(lake::api::default_terms_url, "");
    EXPECT_EQ(lake::api::default_privacy_url, "");
    const std::string_view fallback = "https://example.com/legal/terms";
    EXPECT_EQ(legal_url_setting("LAKE_TERMS_URL", nullptr, fallback).value(), fallback);
    EXPECT_EQ(legal_url_setting("LAKE_TERMS_URL", "", fallback).value(), fallback);
    EXPECT_EQ(legal_url_setting("LAKE_TERMS_URL", "off", fallback).value(), "");
    EXPECT_EQ(legal_url_setting("LAKE_TERMS_URL", "https://acme.example/legal/terms?v=2#top", fallback).value(),
              "https://acme.example/legal/terms?v=2#top");
    EXPECT_EQ(legal_url_setting("LAKE_PRIVACY_URL", "http://localhost:3000/privacy", fallback).value(),
              "http://localhost:3000/privacy");
    for (const char* bad : {"javascript:alert(1)", "/legal/terms", "www.example.com/legal/terms", "https://",
                            "https://a.example/x y", "https://a.example/\"x", "https://a.example/<b>",
                            "ftp://a.example/terms"}) {
        const auto refused = legal_url_setting("LAKE_TERMS_URL", bad, fallback);
        ASSERT_FALSE(refused.has_value()) << bad;
        EXPECT_TRUE(refused.error().starts_with("LAKE_TERMS_URL ")) << refused.error();
    }
    const std::string long_url = "https://a.example/" + std::string(2048, 'a');
    EXPECT_FALSE(legal_url_setting("LAKE_PRIVACY_URL", long_url.c_str(), fallback).has_value());
}

TEST(WebApp, ViewerNamesClerkAndAnOrganizationNamesItsSpace) {
    Surface s;
    s.ctx.clerk_publishable_key = "pk_test_ZXhhbXBsZS0xMjMuY2xlcmsuYWNjb3VudHMuZGV2JA==";
    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    s.ctx.operator_credential = false;
    const ApiResult visitor = s.call("viewer");
    ASSERT_TRUE(visitor.ok) << visitor.error.message;
    EXPECT_EQ(visitor.value["signed_in"], false);
    EXPECT_EQ(visitor.value["clerk"]["enabled"], true);
    EXPECT_EQ(visitor.value["clerk"]["frontend_api"], "example-123.clerk.accounts.dev");
    EXPECT_EQ(visitor.value["password_sign_up"], false);
    EXPECT_EQ(visitor.value["version"], "0.0.1");

    s.ctx.authorized = true;
    s.ctx.acting_identity = "admin";
    s.ctx.operator_credential = true;
    const ApiResult organization = s.call("create_organization", {{"display_name", "Acme"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const ApiResult before = s.call("get_organization", {{"id", organization.value["id"]}});
    ASSERT_TRUE(before.ok) << before.error.message;
    EXPECT_TRUE(before.value["customer_id"].is_null());
    const ApiResult space = s.call("create_customer",
                                   {{"customer_type", "organization"},
                                    {"organization_id", organization.value["id"]},
                                    {"name", "acme"}});
    ASSERT_TRUE(space.ok) << space.error.message;
    const ApiResult after = s.call("get_organization", {{"id", organization.value["id"]}});
    ASSERT_TRUE(after.ok);
    EXPECT_EQ(after.value["customer_id"], space.value["id"]);
    const ApiResult administrator = s.call("viewer");
    ASSERT_TRUE(administrator.ok);
    EXPECT_EQ(administrator.value["administrator"], true);
    EXPECT_FALSE(administrator.value.contains("user_id"));
    ASSERT_EQ(administrator.value["spaces"].size(), 1U);
    EXPECT_EQ(administrator.value["spaces"][0]["kind"], "organization");
    EXPECT_EQ(administrator.value["spaces"][0]["label"], "acme");
    EXPECT_EQ(administrator.value["customer_id"], space.value["id"]);
}

} // namespace
