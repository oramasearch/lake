// Usage over time (lake#564, docs/api/metering.md#usage-over-time): the bounded recorder that
// writes each workspace's `lake_usage` and `lake_storage` series, the owner loop's sample,
// usage_over_time across workspaces, usage limits and record_model_usage.

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/transcription.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::UsageRecorder;
using nlohmann::json;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";

std::string space(lake::control::Store& store, const std::string& handle) {
    const auto user =
        store.create_user({"taap:1:ed25519:usage-" + handle, handle + "@example.test", handle});
    EXPECT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", handle});
    EXPECT_TRUE(customer) << customer.error().message;
    return customer->id;
}

void flush(UsageRecorder& recorder) {
    static_cast<void>(liquid::runtime::block_on(recorder.flush()));
}

// 2027-01-15T08:00:00Z, an hour boundary.
const std::chrono::system_clock::time_point hour{
    std::chrono::milliseconds(1'800'000'000'000 - 1'800'000'000'000 % 3'600'000)};

std::int64_t ms(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::string utc(std::chrono::system_clock::time_point at) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(at));
}

TEST(UsageRecorder, AddsPerWorkspaceAndHourAndWritesOnlyOnFlush) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string mine = space(*store, "usage-mine");
    const std::string theirs = space(*store, "usage-theirs");
    const std::string home = workspaces.create(mine, "home");
    const std::string lab = workspaces.create(mine, "lab");
    const std::string other = workspaces.create(theirs, "home");

    UsageRecorder recorder(store.get(), workspaces.service.get());
    recorder.add(home, "calls", 1, {{"kind", "api"}}, hour);
    recorder.add(home, "calls", 1, {{"kind", "api"}}, hour + std::chrono::minutes(59));
    recorder.add(home, "calls", 1, {{"kind", "mcp"}}, hour);
    recorder.add(home, "calls", 1, {{"kind", "api"}}, hour + std::chrono::hours(1));
    recorder.add(lab, "speech", 62, {{"kind", "transcript"}}, hour);
    recorder.add(other, "calls", 1, {{"kind", "api"}}, hour);
    recorder.add(home, "calls", 0, {{"kind", "api"}}, hour); // nothing: ignored
    recorder.add("", "calls", 1, {}, hour);                  // no workspace: ignored
    recorder.sample(home, "workspace_data", 4096, hour + std::chrono::minutes(5));

    // home: api@h, mcp@h, api@h+1, and one reading; lab and other one value each.
    EXPECT_EQ(recorder.pending(), 6U);
    EXPECT_TRUE(workspaces.records(home).metrics.empty()) << "nothing is written on add";

    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    auto& series = workspaces.records(home);
    ASSERT_TRUE(series.metrics.contains("lake_usage"));
    ASSERT_TRUE(series.metrics.contains("lake_storage"));
    const json& usage = series.metrics.at("lake_usage").definition;
    EXPECT_EQ(usage["kind"], "counter");
    EXPECT_EQ(usage["resolution_ms"], 3'600'000);
    EXPECT_EQ(usage["retention_ms"], 400LL * 24 * 60 * 60 * 1000);
    EXPECT_EQ(series.metrics.at("lake_storage").definition["kind"], "gauge");
    EXPECT_EQ(series.metric_total("lake_usage", {{"dimension", "calls"}}), 4);
    EXPECT_EQ(series.metric_total("lake_usage", {{"dimension", "calls"}, {"kind", "api"}}), 3);
    for (const auto& point : series.metrics.at("lake_usage").points) {
        EXPECT_EQ(point["at_ms"].get<std::int64_t>() % 3'600'000, 0) << "hour buckets";
    }
    const auto& reading = series.metrics.at("lake_storage").points.at(0);
    EXPECT_EQ(reading["at_ms"], ms(hour + std::chrono::minutes(5))) << "a reading keeps its time";
    EXPECT_EQ(reading["labels"]["kind"], "workspace_data");
    EXPECT_EQ(workspaces.records(lab).metric_total("lake_usage", {{"dimension", "speech"}}), 62);
    EXPECT_EQ(workspaces.records(other).metric_total("lake_usage"), 1);

    const auto totals = recorder.totals();
    EXPECT_EQ(totals.increments, 5U);
    EXPECT_EQ(totals.readings, 1U);
    EXPECT_EQ(totals.points, 6U);
    EXPECT_EQ(totals.flush_errors, 0U);

    // Each metric is defined once per workspace in this process.
    recorder.add(home, "calls", 1, {{"kind", "api"}}, hour);
    flush(recorder);
    std::size_t defines = 0;
    for (const auto& call : series.calls) {
        defines += call == "create_metric" ? 1 : 0;
    }
    EXPECT_EQ(defines, 2U);
}

TEST(UsageRecorder, AReadingKeepsTheLatestOfItsHour) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "usage-reading");
    const std::string home = workspaces.create(customer, "home");

    UsageRecorder recorder(store.get(), workspaces.service.get());
    recorder.sample(home, "repositories", 100, hour + std::chrono::minutes(30));
    recorder.sample(home, "repositories", 50, hour + std::chrono::minutes(10)); // older: ignored
    recorder.sample(home, "repositories", 300, hour + std::chrono::minutes(45));
    recorder.sample(home, "images", 7, hour + std::chrono::minutes(45));
    EXPECT_EQ(recorder.pending(), 2U) << "one reading per kind and hour";
    flush(recorder);
    const auto& points = workspaces.records(home).metrics.at("lake_storage").points;
    ASSERT_EQ(points.size(), 2U);
    for (const auto& point : points) {
        if (point["labels"]["kind"] == "repositories") {
            EXPECT_EQ(point["value"], 300);
        }
    }
}

TEST(UsageRecorder, IsBoundedAndKeepsValuesAddedTogetherTogether) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "usage-bound");
    const std::string home = workspaces.create(customer, "home");

    UsageRecorder recorder(store.get(), workspaces.service.get(), 3);
    recorder.add(home, "calls", 1, {{"kind", "api"}}, hour);
    std::vector<UsageRecorder::Increment> first{
        {"model_tokens", 10, {{"kind", "input"}, {"agent", "a"}}},
        {"model_tokens", 4, {{"kind", "output"}, {"agent", "a"}}}};
    recorder.add_together(home, first, hour); // two more: three held
    std::vector<UsageRecorder::Increment> second{
        {"model_tokens", 10, {{"kind", "input"}, {"agent", "b"}}},
        {"model_tokens", 4, {{"kind", "output"}, {"agent", "b"}}}};
    recorder.add_together(home, second, hour); // no room for either: dropped whole
    recorder.add_together(home, first, hour);  // held values still merge
    recorder.sample(home, "images", 9, hour);  // a new reading at the bound: dropped
    EXPECT_EQ(recorder.pending(), 3U);
    EXPECT_EQ(recorder.totals().dropped_bound, 2U) << "an addition and a reading, not values";

    flush(recorder);
    auto& series = workspaces.records(home);
    EXPECT_EQ(series.metric_total("lake_usage", {{"agent", "a"}, {"kind", "input"}}), 20);
    EXPECT_EQ(series.metric_total("lake_usage", {{"agent", "a"}, {"kind", "output"}}), 8);
    EXPECT_EQ(series.metric_total("lake_usage", {{"agent", "b"}}), 0);
    EXPECT_FALSE(series.metrics.contains("lake_storage"));
}

TEST(UsageRecorder, KeepsAFailedWriteAndDropsGoneOrRefusingWorkspaces) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "usage-failures");
    const std::string home = workspaces.create(customer, "home");
    const std::string gone = workspaces.create(customer, "gone");
    const std::string conflicting = workspaces.create(customer, "conflicting");

    UsageRecorder recorder(store.get(), workspaces.service.get());
    // The workspace does not answer: kept, and written by the next flush.
    workspaces.records(home).metric_unavailable_next = 1;
    recorder.add(home, "calls", 2, {{"kind", "api"}}, hour);
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 1U);
    EXPECT_EQ(recorder.totals().flush_errors, 1U);
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(workspaces.records(home).metric_total("lake_usage"), 2);

    // A deleted workspace's values go with it; an unknown id is dropped the same way.
    lake::control::WorkspacePatch deleted;
    deleted.desired = "deleted";
    ASSERT_TRUE(store->update_workspace(gone, deleted));
    recorder.add(gone, "calls", 1, {{"kind", "api"}}, hour);
    recorder.add("00000000-0000-4000-8000-000000000000", "calls", 1, {{"kind", "api"}}, hour);
    // A workspace that defines lake_usage differently refuses it for good.
    workspaces.records(conflicting).metrics["lake_usage"] =
        lake::test::RecordTables::Metric{{{"metric", "lake_usage"}, {"kind", "gauge"}}, {}};
    recorder.add(conflicting, "calls", 1, {{"kind", "api"}}, hour);
    recorder.sample(conflicting, "images", 5, hour);
    flush(recorder);
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.totals().dropped_workspace_deleted, 2U);
    EXPECT_EQ(recorder.totals().dropped_refused, 1U);
    EXPECT_EQ(workspaces.records(conflicting).metric_total("lake_storage"), 5)
        << "only the conflicting metric is refused";
}

// A registry over ready workspaces, with the recorder in its context.
struct UsageRig {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::string customer = space(*store, "usage-rig");
    std::string home = workspaces.create(customer, "home");
    std::string lab = workspaces.create(customer, "lab");
    UsageRecorder recorder{store.get(), workspaces.service.get()};
    lake::api::Registry registry;

    UsageRig() {
        lake::api::register_core_capabilities(registry, "test");
    }

    lake::api::ApiContext context() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.usage_recorder = &recorder;
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        return context;
    }

    lake::api::ApiResult call(const std::string& name, json input) {
        auto context = this->context();
        return call(name, std::move(input), context);
    }

    lake::api::ApiResult call(const std::string& name, json input, lake::api::ApiContext& context) {
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }
};

const json* dimension(const json& reading, const std::string& name) {
    for (const json& entry : reading["dimensions"]) {
        if (entry["dimension"] == name) {
            return &entry;
        }
    }
    return nullptr;
}

TEST(UsageOverTime, AddsUpTheChosenWorkspacesByHourDayAndMonth) {
    UsageRig rig;
    const auto day = std::chrono::hours(24);
    rig.recorder.add(rig.home, "calls", 3, {{"kind", "api"}}, hour);
    rig.recorder.add(rig.home, "calls", 2, {{"kind", "mcp"}}, hour + std::chrono::hours(1));
    rig.recorder.add(rig.lab, "calls", 5, {{"kind", "api"}}, hour + day);
    rig.recorder.add(rig.home, "speech", 90, {{"kind", "transcript"}}, hour + day);
    // Storage readings: the latest of each period counts, and workspaces add up.
    rig.recorder.sample(rig.home, "workspace_data", 100, hour);
    rig.recorder.sample(rig.home, "workspace_data", 150, hour + std::chrono::hours(2));
    rig.recorder.sample(rig.lab, "workspace_data", 1000, hour + std::chrono::hours(2));
    rig.recorder.sample(rig.home, "workspace_data", 175, hour + day);
    flush(rig.recorder);

    const std::string from = utc(std::chrono::floor<std::chrono::days>(hour));
    const std::string to = utc(std::chrono::floor<std::chrono::days>(hour) + 2 * day);
    const auto days = rig.call("usage_over_time",
                               {{"customer_id", rig.customer},
                                {"from", from},
                                {"to", to},
                                {"granularity", "day"},
                                {"dimensions", json::array({"calls", "storage", "speech"})}});
    ASSERT_TRUE(days.ok) << days.error.message;
    ASSERT_EQ(days.value["periods"].size(), 2U);
    EXPECT_EQ(days.value["periods"][0], from);
    EXPECT_EQ(days.value["workspaces"].size(), 2U);
    const json* calls = dimension(days.value, "calls");
    ASSERT_NE(calls, nullptr);
    EXPECT_EQ((*calls)["unit"], "calls");
    EXPECT_EQ((*calls)["total"], 10);
    EXPECT_EQ((*calls)["points"], json::array({5, 5}));
    ASSERT_EQ((*calls)["by_workspace"].size(), 2U);
    EXPECT_EQ((*calls)["by_workspace"][0]["total"], 5);
    EXPECT_EQ((*calls)["by_kind"][0]["key"], "api");
    EXPECT_EQ((*calls)["by_kind"][0]["total"], 8);
    const json* storage = dimension(days.value, "storage");
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ((*storage)["aggregation"], "last");
    // Day one: home's latest 150 and lab's 1000; day two: home's 175 alone.
    EXPECT_EQ((*storage)["points"], json::array({1150, 175}));
    EXPECT_EQ((*storage)["total"], 175) << "a reading's total is its latest period";
    EXPECT_EQ(dimension(days.value, "speech")->at("total"), 90);
    EXPECT_EQ(dimension(days.value, "transfer"), nullptr) << "only the dimensions asked for";

    // One workspace only.
    const auto lab_only = rig.call("usage_over_time",
                                   {{"customer_id", rig.customer},
                                    {"workspace_ids", json::array({rig.lab})},
                                    {"from", from},
                                    {"to", to},
                                    {"granularity", "day"},
                                    {"dimensions", json::array({"calls"})}});
    ASSERT_TRUE(lab_only.ok) << lab_only.error.message;
    EXPECT_EQ(dimension(lab_only.value, "calls")->at("total"), 5);
    EXPECT_EQ(lab_only.value["workspaces"].size(), 1U);

    // Hours: `from` and `to` round out to whole hours.
    const auto hours = rig.call("usage_over_time",
                                {{"customer_id", rig.customer},
                                 {"workspace_ids", json::array({rig.home})},
                                 {"from", utc(hour + std::chrono::minutes(20))},
                                 {"to", utc(hour + std::chrono::minutes(90))},
                                 {"granularity", "hour"},
                                 {"dimensions", json::array({"calls"})}});
    ASSERT_TRUE(hours.ok) << hours.error.message;
    EXPECT_EQ(hours.value["periods"].size(), 2U);
    EXPECT_EQ(hours.value["from"], utc(hour));
    EXPECT_EQ(dimension(hours.value, "calls")->at("points"), json::array({3, 2}));

    // Months add up the days of each month.
    const auto months = rig.call("usage_over_time",
                                 {{"customer_id", rig.customer},
                                  {"from", from},
                                  {"to", to},
                                  {"granularity", "month"},
                                  {"dimensions", json::array({"calls", "storage"})}});
    ASSERT_TRUE(months.ok) << months.error.message;
    ASSERT_EQ(months.value["periods"].size(), 1U);
    EXPECT_EQ(months.value["periods"][0].get<std::string>().substr(8, 2), "01");
    EXPECT_EQ(dimension(months.value, "calls")->at("total"), 10);
    EXPECT_EQ(dimension(months.value, "storage")->at("points"), json::array({1175}))
        << "each workspace's latest reading of the month";
}

TEST(UsageOverTime, RefusesAnotherAccountsWorkspaceAndBadPeriods) {
    UsageRig rig;
    const std::string other = space(*rig.store, "usage-other");
    const std::string theirs = rig.workspaces.create(other, "theirs");
    const json base{{"customer_id", rig.customer},
                    {"from", "2026-10-01T00:00:00Z"},
                    {"to", "2026-10-02T00:00:00Z"},
                    {"granularity", "day"}};

    json crossing = base;
    crossing["workspace_ids"] = json::array({rig.home, theirs});
    const auto refused = rig.call("usage_over_time", crossing);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/workspace_ids/1");

    json unknown = base;
    unknown["dimensions"] = json::array({"calls", "invoices"});
    EXPECT_FALSE(rig.call("usage_over_time", unknown).ok);

    json backwards = base;
    backwards["to"] = "2026-09-30T00:00:00Z";
    EXPECT_EQ(rig.call("usage_over_time", backwards).error.path, "/to");

    json too_many = base;
    too_many["granularity"] = "hour";
    too_many["to"] = "2027-10-01T00:00:00Z";
    EXPECT_EQ(rig.call("usage_over_time", too_many).error.path, "/to");

    // A person of another account may not read this account's usage.
    const auto other_customer = rig.store->get_customer(other);
    ASSERT_TRUE(other_customer);
    const auto outsider = rig.store->get_user(other_customer->user_id);
    ASSERT_TRUE(outsider);
    auto context = rig.context();
    context.acting_identity = outsider->taap_id;
    context.acting_user_id = outsider->id;
    context.operator_credential = false;
    const auto denied = rig.call("usage_over_time", base, context);
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
}

TEST(UsageLimits, WarnFromTheirPercentageAndStorageIsTheEnforcedQuota) {
    UsageRig rig;
    // No limits: nothing to report.
    const auto none = rig.call("usage_limits", {{"customer_id", rig.customer}});
    ASSERT_TRUE(none.ok) << none.error.message;
    EXPECT_TRUE(none.value["limits"].empty());

    ASSERT_TRUE(rig.call("set_usage_limit",
                         {{"customer_id", rig.customer},
                          {"dimension", "calls"},
                          {"limit", 10},
                          {"warn_percent", 80}})
                    .ok);
    // This month's calls, across both workspaces.
    const auto now = std::chrono::system_clock::now();
    rig.recorder.add(rig.home, "calls", 5, {{"kind", "api"}}, now);
    rig.recorder.add(rig.lab, "calls", 3, {{"kind", "mcp"}}, now);
    flush(rig.recorder);
    const auto warned = rig.call("usage_limits", {{"customer_id", rig.customer}});
    ASSERT_TRUE(warned.ok) << warned.error.message;
    ASSERT_EQ(warned.value["limits"].size(), 1U);
    const json& calls = warned.value["limits"][0];
    EXPECT_EQ(calls["dimension"], "calls");
    EXPECT_EQ(calls["used"], 8);
    EXPECT_EQ(calls["state"], "warning");
    EXPECT_EQ(calls["enforced"], false);
    EXPECT_EQ(warned.value["warnings"].size(), 1U);

    rig.recorder.add(rig.home, "calls", 4, {{"kind", "api"}}, now);
    flush(rig.recorder);
    const auto over = rig.call("usage_limits", {{"customer_id", rig.customer}});
    EXPECT_EQ(over.value["limits"][0]["state"], "over");

    // Storage's limit is the repository storage quota that set_storage_quota enforces.
    ASSERT_TRUE(rig.call("set_usage_limit",
                         {{"customer_id", rig.customer}, {"dimension", "storage"}, {"limit", 4096}})
                    .ok);
    const auto quota = rig.store->storage_quota(rig.customer);
    ASSERT_TRUE(quota && *quota);
    EXPECT_EQ(**quota, 4096U);
    const auto both = rig.call("usage_limits", {{"customer_id", rig.customer}});
    ASSERT_EQ(both.value["limits"].size(), 2U);
    EXPECT_EQ(both.value["limits"][0]["dimension"], "storage");
    EXPECT_EQ(both.value["limits"][0]["enforced"], true);
    EXPECT_EQ(both.value["limits"][0]["warn_percent"], 80);

    // 0 removes a limit; an unknown dimension is refused.
    ASSERT_TRUE(rig.call("set_usage_limit",
                         {{"customer_id", rig.customer}, {"dimension", "calls"}, {"limit", 0}})
                    .ok);
    const auto removed = rig.call("usage_limits", {{"customer_id", rig.customer}});
    EXPECT_EQ(removed.value["limits"].size(), 1U);
    EXPECT_FALSE(rig.call("set_usage_limit",
                          {{"customer_id", rig.customer}, {"dimension", "dollars"}, {"limit", 5}})
                     .ok);
}

TEST(RecordModelUsage, CountsAnAgentRunsTokensInItsWorkspace) {
    UsageRig rig;
    const auto owner = rig.store->get_customer(rig.customer);
    ASSERT_TRUE(owner);
    const auto launch = rig.store->create_agent_launch({.customer_id = rig.customer,
                                                        .agent_id = "agent-1",
                                                        .agent_name = "Churn Watcher",
                                                        .mode = "job",
                                                        .trigger = "manual",
                                                        .scopes = {"workspace_query"},
                                                        .created_by_user_id = owner->user_id,
                                                        .workspace_id = rig.lab});
    ASSERT_TRUE(launch) << launch.error().message;

    auto context = rig.context();
    context.acting_identity.clear();
    context.operator_credential = false;
    context.credential_kind = "agent_launch";
    context.credential_session_id = "launch:" + launch->id;
    const json tokens{{"model_name", "gpt-5"}, {"input_tokens", 1200}, {"output_tokens", 300}};
    const auto recorded = rig.call("record_model_usage", tokens, context);
    ASSERT_TRUE(recorded.ok) << recorded.error.message;
    EXPECT_EQ(recorded.value["workspace_id"], rig.lab);
    EXPECT_EQ(recorded.value["agent"], "Churn Watcher");
    flush(rig.recorder);
    auto& series = rig.workspaces.records(rig.lab);
    EXPECT_EQ(series.metric_total("lake_usage",
                                  {{"dimension", "model_tokens"},
                                   {"kind", "input"},
                                   {"launch_id", launch->id},
                                   {"model", "gpt-5"}}),
              1200);
    EXPECT_EQ(series.metric_total("lake_usage", {{"kind", "output"}}), 300);
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage"), 0);

    const auto read = rig.call(
        "usage_over_time",
        {{"customer_id", rig.customer},
         {"from", utc(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()))},
         {"to", utc(std::chrono::system_clock::now() + std::chrono::hours(1))},
         {"granularity", "day"},
         {"dimensions", json::array({"model_tokens"})}});
    ASSERT_TRUE(read.ok) << read.error.message;
    const json* model_tokens = dimension(read.value, "model_tokens");
    ASSERT_NE(model_tokens, nullptr);
    EXPECT_EQ((*model_tokens)["total"], 1500);
    EXPECT_EQ((*model_tokens)["by_agent"][0]["key"], "Churn Watcher");
    EXPECT_EQ((*model_tokens)["by_launch"][0]["key"], launch->id);
    EXPECT_EQ((*model_tokens)["by_model"][0]["key"], "gpt-5");

    // A person's session, or no credential, may not report an agent's tokens.
    auto person = rig.context();
    person.credential_kind = "session";
    EXPECT_EQ(rig.call("record_model_usage", tokens, person).error.code, "unauthorized");
}

TEST(UsageSample, ReadsEachWorkspacesStoredBytesAndRunningServices) {
    UsageRig rig;
    rig.workspaces.zetta->answers["list_datasets"] = {
        {"datasets",
         json::array(
             {{{"name", "a"}, {"size_bytes", 1000}}, {{"name", "b"}, {"size_bytes", 24}}})}};
    const auto service = rig.store->create_service({.customer_id = rig.customer,
                                                    .kind = "postgres",
                                                    .name = "orders",
                                                    .instances = 2,
                                                    .storage_gi = 5,
                                                    .workspace_id = rig.home});
    ASSERT_TRUE(service) << service.error().message;
    ASSERT_TRUE(rig.store->set_service_status(service->id, "Ready", "", "postgres://orders"));
    const auto pending = rig.store->create_service({.customer_id = rig.customer,
                                                    .kind = "postgres",
                                                    .name = "waiting",
                                                    .workspace_id = rig.home});
    ASSERT_TRUE(pending);

    auto context = rig.context();
    const auto report = liquid::runtime::block_on(
        lake::api::sample_usage(context, std::chrono::seconds(900), hour));
    ASSERT_TRUE(report);
    EXPECT_EQ(report->workspaces, 2U);
    flush(rig.recorder);
    auto& series = rig.workspaces.records(rig.home);
    EXPECT_EQ(series.metric_total("lake_storage", {{"kind", "workspace_data"}}), 1024);
    EXPECT_EQ(series.metric_total("lake_storage", {{"kind", "transcripts"}}), 0);
    EXPECT_EQ(series.metric_total("lake_storage", {{"kind", "drive_files"}}), 0);
    EXPECT_EQ(series.metric_total("lake_storage", {{"kind", "images"}}), 0);
    // Two instances for 900 seconds, with 5 GiB each; the service still pending counts nothing.
    EXPECT_EQ(
        series.metric_total("lake_usage", {{"dimension", "instances"}, {"object_id", service->id}}),
        1800);
    EXPECT_EQ(
        series.metric_total("lake_usage", {{"dimension", "volumes"}, {"object_id", service->id}}),
        9000);
    EXPECT_EQ(series.metric_total("lake_usage", {{"object_id", pending->id}}), 0);
    // The workspace's own engine is a running service too.
    EXPECT_EQ(series.metric_total("lake_usage", {{"dimension", "instances"}, {"kind", "zetta"}}),
              900);
}

TEST(UsageSample, CountsTheSecondsOfATranscriptMadeFromAudio) {
    UsageRig rig;
    auto context = rig.context();
    context.transcriber = [](const lake::api::TranscriptionRequest&) {
        return std::expected<lake::api::Transcription, lake::api::ApiError>(
            lake::api::Transcription{"hello from the call", 61.2, "gpt-4o-transcribe"});
    };
    const auto made = rig.call("create_transcript",
                               {{"customer_id", rig.customer},
                                {"workspace_id", rig.home},
                                {"title", "Call"},
                                {"source", "recording"},
                                {"audio_base64", "AAAA"},
                                {"audio_name", "call.webm"}},
                               context);
    ASSERT_TRUE(made.ok) << made.error.message;
    flush(rig.recorder);
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total(
                  "lake_usage", {{"dimension", "speech"}, {"kind", "transcript"}}),
              62)
        << "61.2 seconds, rounded up";

    // gpt-4o-transcribe reports tokens, not seconds: they count as its model tokens.
    context.transcriber = [](const lake::api::TranscriptionRequest&) {
        lake::api::Transcription heard{"another call", std::nullopt, "gpt-4o-transcribe"};
        heard.input_tokens = 900;
        heard.output_tokens = 40;
        return std::expected<lake::api::Transcription, lake::api::ApiError>(heard);
    };
    ASSERT_TRUE(rig.call("create_transcript",
                         {{"customer_id", rig.customer},
                          {"workspace_id", rig.home},
                          {"title", "Second call"},
                          {"source", "recording"},
                          {"audio_base64", "AAAA"},
                          {"audio_name", "call.webm"}},
                         context)
                    .ok);
    flush(rig.recorder);
    auto& series = rig.workspaces.records(rig.home);
    EXPECT_EQ(
        series.metric_total(
            "lake_usage",
            {{"dimension", "model_tokens"}, {"model", "gpt-4o-transcribe"}, {"kind", "input"}}),
        900);
    EXPECT_EQ(series.metric_total("lake_usage", {{"dimension", "speech"}}), 62);
}

TEST(UsageOverTime, CountsEachModelsAnsweredPredictions) {
    UsageRig rig;
    rig.workspaces.zetta->answers["get_model"] = {{"id", "m-1"}, {"exposed", true}};
    rig.workspaces.zetta->answers["score"] = {
        {"ranked", json::array({{{"entity", 0}, {"risk", 0.9}}})}};
    const json input{{"customer_id", rig.customer},
                     {"workspace_id", rig.home},
                     {"model_id", "m-1"},
                     {"rows", json::array({{{"entity", "acct-a"}, {"tenure", 4}}})}};
    ASSERT_TRUE(rig.call("predict", input).ok);
    ASSERT_TRUE(rig.call("predict", input).ok);
    // A refused call is not an answered prediction.
    rig.workspaces.zetta->answers["get_model"] = {{"id", "m-1"}, {"exposed", false}};
    EXPECT_FALSE(rig.call("predict", input).ok);
    flush(rig.recorder);
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total(
                  "lake_usage", {{"dimension", "predictions"}, {"object_id", "m-1"}}),
              2);
}

TEST(UsageSample, ReadsTheTokensATranscriptionModelReports) {
    const auto tokens = lake::api::parse_openai_transcription(
        200,
        R"({"text":"hi","usage":{"type":"tokens","input_tokens":120,"output_tokens":7,"total_tokens":127}})",
        "gpt-4o-transcribe");
    ASSERT_TRUE(tokens);
    EXPECT_FALSE(tokens->seconds);
    EXPECT_EQ(tokens->input_tokens, 120U);
    EXPECT_EQ(tokens->output_tokens, 7U);
    const auto seconds = lake::api::parse_openai_transcription(
        200, R"({"text":"hi","usage":{"type":"duration","seconds":3.5}})", "whisper-1");
    ASSERT_TRUE(seconds);
    EXPECT_EQ(seconds->seconds, 3.5);
    EXPECT_FALSE(seconds->input_tokens);
}

} // namespace

// lake#643: compute is metered by the resources each workload is scheduled with, as the Orama
// operator reports them in workload_resources: CPU in mCPU-seconds, RAM and GPU memory in
// MiB-seconds, GPU seconds by class. A running workload counts the seconds since the last sample;
// a build or an agent job counts once, from its request to its end, in the sample after it ends.
TEST(UsageSample, MetersCpuRamGpuAndGpuMemoryFromScheduledResources) {
    constexpr std::uint64_t mib = 1024 * 1024;
    constexpr std::uint64_t gib = 1024 * mib;
    UsageRig rig;
    auto& store = *rig.store;
    // The home workspace's engine, a Postgres of two instances and a GPU app run in home.
    ASSERT_TRUE(store.put_workload_resources({"workspace", rig.home, 1, 250, 512 * mib}));
    const auto service = store.create_service({.customer_id = rig.customer,
                                               .kind = "postgres",
                                               .name = "orders",
                                               .instances = 2,
                                               .workspace_id = rig.home});
    ASSERT_TRUE(service) << service.error().message;
    ASSERT_TRUE(store.put_workload_resources({"service", service->id, 2, 1000, 2 * gib}));
    const auto unreported = store.create_service({.customer_id = rig.customer,
                                                  .kind = "postgres",
                                                  .name = "unreported",
                                                  .workspace_id = rig.home});
    ASSERT_TRUE(unreported);
    const auto app = store.create_web_app({.customer_id = rig.customer,
                                           .workspace_id = rig.home,
                                           .name = "vision",
                                           .mode = "image",
                                           .image = "lake.example.test/usage-rig/vision:v1",
                                           .framework = "container",
                                           .port = 8080,
                                           .host = "vision.apps.example.test"});
    ASSERT_TRUE(app) << app.error().message;
    ASSERT_TRUE(
        store.put_workload_resources({"app", app->id, 1, 4000, 16 * gib, 1, "l4", 24 * gib}));
    // An image build that ran ten minutes and an agent job that ran five, both finished now; their
    // pods are gone and their rows keep what they ran with.
    const auto repository =
        store.create_repository({rig.customer, "models", "", "private", rig.lab});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto build = store.create_build({.customer_id = rig.customer,
                                           .workspace_id = rig.lab,
                                           .repository = repository->id,
                                           .revision = "abc",
                                           .target = "lake.example.test/usage-rig/models:v1"});
    ASSERT_TRUE(build) << build.error().message;
    ASSERT_TRUE(store.age_build(build->id, 600));
    ASSERT_TRUE(store.set_build_status(build->id, {.phase = "Succeeded"}));
    ASSERT_TRUE(store.put_workload_resources({"build", build->id, 0, 2000, 4 * gib}));
    const auto owner = store.get_customer(rig.customer);
    ASSERT_TRUE(owner);
    const auto job = store.create_agent_launch({.customer_id = rig.customer,
                                                .agent_id = "agent-1",
                                                .agent_name = "Churn Watcher",
                                                .mode = "job",
                                                .trigger = "manual",
                                                .scopes = {"workspace_query"},
                                                .created_by_user_id = owner->user_id,
                                                .workspace_id = rig.lab});
    ASSERT_TRUE(job) << job.error().message;
    ASSERT_TRUE(store.age_agent_launch(job->id, 300));
    ASSERT_TRUE(store.set_agent_launch_status(job->id, "Succeeded", "", "", ""));
    ASSERT_TRUE(store.put_workload_resources({"agent_launch", job->id, 0, 500, gib}));

    auto context = rig.context();
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds(1);
    const auto report =
        liquid::runtime::block_on(lake::api::sample_usage(context, std::chrono::seconds(900), now));
    ASSERT_TRUE(report);
    flush(rig.recorder);
    auto& home = rig.workspaces.records(rig.home);
    const auto total = [](auto& series, const json& labels) {
        return series.metric_total("lake_usage", labels);
    };
    // 900 seconds of each running workload.
    EXPECT_EQ(total(home, {{"dimension", "cpu"}, {"kind", "workspace"}}), 250 * 900);
    EXPECT_EQ(total(home, {{"dimension", "ram"}, {"kind", "workspace"}}), 512 * 900);
    EXPECT_EQ(total(home, {{"dimension", "cpu"}, {"object_id", service->id}}), 1000 * 900);
    EXPECT_EQ(total(home, {{"dimension", "ram"}, {"kind", "postgres"}}), 2048 * 900);
    EXPECT_EQ(total(home, {{"dimension", "cpu"}, {"kind", "app"}}), 4000 * 900);
    EXPECT_EQ(total(home, {{"dimension", "gpu"}, {"class", "l4"}, {"object_id", app->id}}), 900);
    EXPECT_EQ(total(home, {{"dimension", "vram"}, {"class", "l4"}}), 24 * 1024 * 900);
    EXPECT_EQ(total(home, {{"object_id", unreported->id}}), 0)
        << "nothing reported, nothing counted";
    EXPECT_EQ(total(home, {{"dimension", "gpu"}, {"kind", "postgres"}}), 0)
        << "no GPU, no GPU line";
    // The build and the job once, for how long each ran, in their workspace.
    auto& lab = rig.workspaces.records(rig.lab);
    const double build_cpu = total(lab, {{"dimension", "cpu"}, {"kind", "build"}});
    EXPECT_GE(build_cpu, 2000.0 * 600);
    EXPECT_LE(build_cpu, 2000.0 * 602);
    const double job_ram = total(lab, {{"dimension", "ram"}, {"kind", "agent"}});
    EXPECT_GE(job_ram, 1024.0 * 300);
    EXPECT_LE(job_ram, 1024.0 * 302);
    EXPECT_EQ(total(lab, {{"dimension", "cpu"}, {"kind", "workspace"}}), 0)
        << "lab's engine is not reported";

    // A later sample counts the running workloads again, never the finished build or job.
    const auto later = liquid::runtime::block_on(
        lake::api::sample_usage(context, std::chrono::seconds(900), now + std::chrono::hours(2)));
    ASSERT_TRUE(later);
    flush(rig.recorder);
    EXPECT_EQ(total(home, {{"dimension", "cpu"}, {"kind", "workspace"}}), 250 * 1800);
    EXPECT_EQ(total(lab, {{"dimension", "cpu"}, {"kind", "build"}}), build_cpu);

    // usage_over_time answers them by object and GPU class, and names what the operator has not
    // reported.
    const auto read = rig.call("usage_over_time",
                               {{"customer_id", rig.customer},
                                {"from", utc(now - std::chrono::hours(1))},
                                {"to", utc(now + std::chrono::hours(3))},
                                {"granularity", "hour"},
                                {"dimensions", json::array({"cpu", "ram", "gpu", "vram"})}});
    ASSERT_TRUE(read.ok) << read.error.message;
    const json* cpu = dimension(read.value, "cpu");
    ASSERT_NE(cpu, nullptr);
    EXPECT_EQ((*cpu)["unit"], "mcpu_seconds");
    EXPECT_EQ((*cpu)["by_object"][0]["key"], app->id) << "the largest first";
    const json* gpu = dimension(read.value, "gpu");
    ASSERT_NE(gpu, nullptr);
    EXPECT_EQ((*gpu)["unit"], "gpu_seconds");
    EXPECT_EQ((*gpu)["total"], 1800);
    ASSERT_EQ((*gpu)["by_class"].size(), 1U);
    EXPECT_EQ((*gpu)["by_class"][0]["key"], "l4");
    std::string not_measured = read.value["not_measured"].dump();
    EXPECT_NE(not_measured.find("service unreported"), std::string::npos) << not_measured;
    EXPECT_NE(not_measured.find("the engine of workspace lab"), std::string::npos) << not_measured;
    EXPECT_EQ(not_measured.find("service orders"), std::string::npos) << not_measured;
    EXPECT_EQ(not_measured.find("lake#612"), std::string::npos) << "CPU and memory are measured";
}
