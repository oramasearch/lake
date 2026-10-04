// The per-request log line and the per-workspace request series (decision 54 S9, lake#384,
// docs/serving/telemetry.md).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/request_log.hpp"
#include "lake/api/request_recorder.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_control.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"
#include "workspace_fixture.hpp"

namespace {

using nlohmann::json;
using lake::api::RequestRecorder;

std::string space(lake::control::Store& store, const std::string& handle) {
    const auto user = store.create_user(
        {"taap:1:ed25519:request-series-" + handle, handle + "@example.test", handle});
    EXPECT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", handle});
    EXPECT_TRUE(customer) << customer.error().message;
    return customer->id;
}

void flush(RequestRecorder& recorder) {
    static_cast<void>(liquid::runtime::block_on(recorder.flush()));
}

const std::chrono::system_clock::time_point minute{std::chrono::milliseconds(1'800'000'000'000)};

TEST(RequestLog, NamesTheRouteAndCapturedIdsNeverTheRawPath) {
    const std::string repository = "0f1e2d3c-4b5a-4968-8776-655443322110";
    const std::vector<std::pair<std::string, std::string>> params = {
        {"repository_id", repository},
        {"package", "secret-package"},          // a registry name: left out
        {"customer_id", "not-a-uuid"},          // not a UUID: left out
        {"digest", "sha256:0123456789abcdef"},  // left out
        {"token", "acme-challenge-token"},      // left out
    };
    lake::api::RequestLogEntry entry{.ts = "2026-10-03T00:00:00Z",
                                     .method = "POST",
                                     .route = "/v1/repositories/:repository_id/sync/pull",
                                     .status = 200,
                                     .duration_ms = 1.5,
                                     .node = "lake-0",
                                     .request_id = "request-384",
                                     .ids = lake::api::request_log_ids(params)};
    const json line = json::parse(lake::api::request_log_line(entry));
    EXPECT_FALSE(line.contains("path"));
    EXPECT_EQ(line["route"], "/v1/repositories/:repository_id/sync/pull");
    EXPECT_EQ(line["repository_id"], repository);
    EXPECT_EQ(line["request_id"], "request-384");
    EXPECT_EQ(line["status"], 200);
    EXPECT_EQ(line["node"], "lake-0");
    EXPECT_FALSE(line.contains("package"));
    EXPECT_FALSE(line.contains("customer_id"));
    EXPECT_FALSE(line.contains("digest"));
    EXPECT_FALSE(line.contains("token"));
    EXPECT_EQ(lake::api::request_log_line(entry).find("secret-package"), std::string::npos);

    // A caller's request id is logged only as a short token.
    entry.request_id = "my repo/secret name";
    EXPECT_FALSE(json::parse(lake::api::request_log_line(entry)).contains("request_id"));
    EXPECT_TRUE(lake::api::request_log_capability_route("/v1/get_repository"));
    EXPECT_TRUE(lake::api::request_log_capability_route("/w/:workspace_id/mcp"));
    EXPECT_FALSE(lake::api::request_log_capability_route("/repository"));
}

TEST(RequestRecorder, AggregatesPerWorkspaceAndFlushesOnlyOnFlush) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string mine = space(*store, "series-mine");
    const std::string theirs = space(*store, "series-theirs");
    const std::string home = workspaces.create(mine, "home");
    const std::string lab = workspaces.create(mine, "lab");
    const std::string other = workspaces.create(theirs, "home");

    RequestRecorder recorder(store.get(), workspaces.service.get());
    for (int index = 0; index < 3; ++index) {
        recorder.add(home, "/v1/get_repository", "get_repository", 200, minute);
    }
    recorder.add(home, "/v1/get_repository", "get_repository", 404, minute);
    recorder.add(home, "/repository", "", 200, minute + std::chrono::seconds(30));
    recorder.add(lab, "/v1/get_repository", "get_repository", 200, minute);
    recorder.add(other, "/mcp", "drive_browse", 503, minute);
    recorder.add("", "/v1/server_info", "server_info", 200, minute); // no workspace: ignored

    // Three counts for home (two status classes, one page route), one each for lab and other.
    EXPECT_EQ(recorder.pending(), 5U);
    EXPECT_TRUE(workspaces.records(home).metrics.empty()) << "nothing is written on add";

    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    auto& home_series = workspaces.records(home);
    ASSERT_TRUE(home_series.metrics.contains("lake_requests"));
    const json& definition = home_series.metrics.at("lake_requests").definition;
    EXPECT_EQ(definition["kind"], "counter");
    EXPECT_EQ(definition["resolution_ms"], 60000);
    EXPECT_EQ(definition["retention_ms"], 30LL * 24 * 60 * 60 * 1000);
    EXPECT_EQ(home_series.metric_total("lake_requests"), 5);
    EXPECT_EQ(home_series.metric_total("lake_requests", {{"status_class", "2xx"}}), 4);
    EXPECT_EQ(home_series.metric_total("lake_requests", {{"status_class", "4xx"}}), 1);
    EXPECT_EQ(home_series.metric_total("lake_requests",
                                       {{"route", "/v1/get_repository"},
                                        {"capability", "get_repository"}}),
              4);
    for (const auto& point : home_series.metrics.at("lake_requests").points) {
        EXPECT_EQ(point["at_ms"].get<std::int64_t>() % 60000, 0) << "minute buckets";
        if (point["labels"]["route"] == "/repository") {
            EXPECT_FALSE(point["labels"].contains("capability")) << "a page is no capability call";
        }
    }
    // Two workspaces of one space, and another space, are separate series.
    EXPECT_EQ(workspaces.records(lab).metric_total("lake_requests"), 1);
    EXPECT_EQ(workspaces.records(other).metric_total("lake_requests"), 1);
    EXPECT_EQ(workspaces.records(other).metric_total("lake_requests", {{"status_class", "5xx"}}), 1);

    const auto totals = recorder.totals();
    EXPECT_EQ(totals.recorded, 7U);
    EXPECT_EQ(totals.points, 5U);
    EXPECT_EQ(totals.flush_errors, 0U);

    // The metric is defined once per workspace in this process; a second flush adds only new
    // counts.
    recorder.add(home, "/v1/get_repository", "get_repository", 200, minute);
    flush(recorder);
    flush(recorder);
    EXPECT_EQ(home_series.metric_total("lake_requests"), 6);
    EXPECT_EQ(std::ranges::count(home_series.calls, std::string("create_metric")), 1);
    EXPECT_EQ(std::ranges::count(home_series.calls, std::string("record_metric")), 2);
}

TEST(RequestRecorder, IsBoundedAndCountsWhatItDrops) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "series-bound");
    const std::string home = workspaces.create(customer, "home");

    RequestRecorder recorder(store.get(), workspaces.service.get(), 2);
    recorder.add(home, "/a", "", 200, minute);
    recorder.add(home, "/b", "", 200, minute);
    recorder.add(home, "/c", "", 200, minute); // a third count: dropped
    recorder.add(home, "/a", "", 200, minute); // a held count still merges
    EXPECT_EQ(recorder.pending(), 2U);
    EXPECT_EQ(recorder.totals().dropped_bound, 1U);

    flush(recorder);
    EXPECT_EQ(workspaces.records(home).metric_total("lake_requests", {{"route", "/a"}}), 2);
    EXPECT_EQ(workspaces.records(home).metric_total("lake_requests", {{"route", "/c"}}), 0);
}

TEST(RequestRecorder, KeepsAFailedWriteAndDropsADeletedWorkspace) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "series-failures");
    const std::string home = workspaces.create(customer, "home");
    const std::string gone = workspaces.create(customer, "gone");

    RequestRecorder recorder(store.get(), workspaces.service.get());
    // The workspace does not answer: the count is kept for the next flush, never written
    // anywhere else.
    workspaces.records(home).metric_unavailable_next = 1;
    recorder.add(home, "/a", "", 200, minute);
    flush(recorder);
    EXPECT_EQ(recorder.totals().flush_errors, 1U);
    EXPECT_EQ(recorder.pending(), 1U);
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(workspaces.records(home).metric_total("lake_requests"), 1);

    // Not ready: kept as well.
    workspaces.status(home, "Provisioning");
    recorder.add(home, "/a", "", 200, minute);
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 1U);
    workspaces.ready(home);
    flush(recorder);
    EXPECT_EQ(workspaces.records(home).metric_total("lake_requests"), 2);

    // A deleted workspace's counts are dropped and counted, not retried.
    recorder.add(gone, "/a", "", 200, minute);
    lake::control::WorkspacePatch deleted;
    deleted.desired = "deleted";
    ASSERT_TRUE(store->update_workspace(gone, deleted));
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().dropped_workspace_deleted, 1U);
    EXPECT_TRUE(workspaces.records(gone).metrics.empty());

    // An unknown workspace is dropped the same way.
    recorder.add(lake::control::new_uuid_v4(), "/a", "", 200, minute);
    flush(recorder);
    EXPECT_EQ(recorder.totals().dropped_workspace_deleted, 2U);

    // A workspace that already defines lake_requests differently refuses for good.
    const std::string conflicting = workspaces.create(customer, "conflicting");
    workspaces.records(conflicting).metrics["lake_requests"] =
        lake::test::RecordTables::Metric{{{"metric", "lake_requests"}, {"kind", "gauge"}}, {}};
    recorder.add(conflicting, "/a", "", 200, minute);
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().dropped_refused, 1U);
}

// Against a real Zetta when LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN name one (a local
// `zetta --headless` built from zetta dev with #1231): the time series' real contract.
TEST(RequestRecorderZetta, QueryMetricReturnsTheFlushedCounts) {
    const char* url = std::getenv("LAKE_TEST_ZETTA_URL");
    const char* token = std::getenv("LAKE_TEST_ZETTA_TOKEN");
    if (url == nullptr || token == nullptr || *url == '\0') {
        GTEST_SKIP() << "LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN are not set";
    }
    lake::control::install_zetta_control_transport();
    struct Shutdown {
        ~Shutdown() {
            lake::control::shutdown_zetta_control_transport();
        }
    } shutdown;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::filesystem::path dir =
        lake::test::fixture_path("request-series-zetta-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(dir);
    auto service = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{dir.string(), 0}, store,
        std::make_shared<lake::control::ZettaWorkspaceCaller>());
    const std::string customer = space(*store, "series-zetta");
    const auto workspace = store->create_workspace({customer, "home", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto zetta_service = store->get_service(workspace->zetta_service_id);
    ASSERT_TRUE(zetta_service);
    ASSERT_TRUE(store->set_service_status(zetta_service->id, "Ready", "", url));
    std::ofstream(dir / (zetta_service->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", token}}.dump();

    // A route of its own, so a Zetta reused across runs answers only this run's counts.
    const std::string route = "/test/" + lake::control::new_uuid_v4();
    const auto now = std::chrono::system_clock::now();
    RequestRecorder recorder(store.get(), service.get());
    for (int index = 0; index < 3; ++index) {
        recorder.add(workspace->id, route, "get_repository", 200, now);
    }
    recorder.add(workspace->id, route, "get_repository", 404, now);
    flush(recorder);
    EXPECT_EQ(recorder.totals().flush_errors, 0U);
    EXPECT_EQ(recorder.totals().recorded, 4U);

    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto queried = lake::control::settled(liquid::runtime::block_on(service->call(
        customer, workspace->id, "query_metric", false,
        json{{"metric", "lake_requests"},
             {"from_ms", now_ms - 120'000},
             {"to_ms", now_ms + 120'000},
             {"labels", {{"route", route}}},
             {"group_by", json::array({"status_class"})}})));
    ASSERT_TRUE(queried) << queried.error().code << ": " << queried.error().message;
    double ok = 0, missing = 0;
    for (const auto& series : queried->at("series")) {
        const std::string status_class = series.at("labels").value("status_class", "");
        (status_class == "2xx" ? ok : missing) += series.at("total").get<double>();
    }
    EXPECT_EQ(ok, 3);
    EXPECT_EQ(missing, 1);
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

} // namespace
