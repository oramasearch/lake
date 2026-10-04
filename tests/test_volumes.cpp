// The operator's volumes (migration 0069, operator#259, operator#260, docs/api/volumes.md):
// set_workspace_storage, request_volume_snapshot and volume_snapshots, against the memory store and
// against PostgreSQL, where the trigger refuses a smaller size on its own.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "audit_test_support.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"

namespace {

using json = nlohmann::json;

class Volumes : public testing::TestWithParam<bool> {
protected:
    static constexpr const char* kek =
        "766f6c756d65732d696e7374616e63652d6b656b2d746573742d303030303030";

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
        lake::api::register_core_capabilities(registry, "test");
        const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
        const auto owner = store->create_user(
            {"taap:1:ed25519:volumes-" + suffix, "volumes-" + suffix + "@example.test", "Owner"});
        ASSERT_TRUE(owner) << owner.error().message;
        owner_id = owner->id;
        owner_taap = owner->taap_id;
        const auto customer =
            store->create_customer({"individual", owner->id, "", "volumes-" + suffix});
        ASSERT_TRUE(customer) << customer.error().message;
        customer_id = customer->id;
        // The owner administers the space, as a real owner's grant does.
        const auto object = store->find_policy_object("customer", customer_id);
        ASSERT_TRUE(object) << object.error().message;
        ASSERT_TRUE(store->create_grant({owner_taap, object->id, "admin", "owner-attestation",
                                         "admin", "owner-grant"}));
        const auto workspace = store->create_workspace({customer_id, "main", "prod", ""});
        ASSERT_TRUE(workspace) << workspace.error().message;
        workspace_id = workspace->id;
    }

    // As the operator credential (the instance administrator) unless `identity` names another.
    lake::api::ApiResult call(const std::string& name, const json& input,
                              const std::string& identity = "admin") {
        lake::api::ApiContext context;
        context.store = store.get();
        context.authorized = true;
        context.acting_identity = identity;
        context.operator_credential = identity == "admin";
        context.instance_kek = kek;
        return registry.call(name, input, context);
    }

    // What the operator writes to volume_snapshot_status.
    void report_snapshot(const std::string& target, const std::string& name, bool ready) {
        if (database_uri.empty()) {
            ASSERT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_volume_snapshot_status(
                target, {.snapshot_name = name,
                         .ready_to_use = ready,
                         .snapshot_at = "2026-10-03T12:00:00.000000Z"}));
            return;
        }
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO volume_snapshot_status (target, snapshot_name, ready_to_use, "
                       "snapshot_at, message, updated_at) VALUES ($1, $2, $3, "
                       "'2026-10-03T12:00:00Z', '', now()) ON CONFLICT (target) DO UPDATE SET "
                       "snapshot_name = EXCLUDED.snapshot_name, ready_to_use = "
                       "EXCLUDED.ready_to_use, snapshot_at = EXCLUDED.snapshot_at",
                       target, name, ready);
        tx.commit();
    }

    std::shared_ptr<lake::control::Store> store;
    std::string database_uri;
    lake::api::Registry registry;
    std::string owner_id, owner_taap, customer_id, workspace_id;
};

// A workspace starts at 5 GiB; the operator grows it, which advances the generation the operator
// acknowledges; the same size changes nothing; a smaller size, an unknown or deleted workspace and
// a caller who is not the operator are refused; and each call is audited centrally.
TEST_P(Volumes, WorkspaceStorageOnlyGrowsAndOnlyTheOperatorSetsIt) {
    const auto created = store->get_workspace(workspace_id);
    ASSERT_TRUE(created);
    EXPECT_EQ(created->storage_gi, 5);
    EXPECT_EQ(created->storage_gi_observed, 0);
    EXPECT_EQ(created->storage_phase, "");

    const auto grown = call("set_workspace_storage", {{"workspace_id", workspace_id}, {"storage_gi", 20}});
    ASSERT_TRUE(grown.ok) << grown.error.message;
    EXPECT_EQ(grown.value.at("storage_gi"), 20);
    EXPECT_EQ(grown.value.at("generation"), created->generation + 1);
    EXPECT_EQ(grown.value.at("storage_gi_observed"), 0);
    EXPECT_TRUE(grown.value.contains("storage_phase"));
    EXPECT_TRUE(grown.value.contains("storage_message"));

    const auto same = call("set_workspace_storage", {{"workspace_id", workspace_id}, {"storage_gi", 20}});
    ASSERT_TRUE(same.ok) << same.error.message;
    EXPECT_EQ(same.value.at("generation"), created->generation + 1) << "nothing changed";

    const auto smaller = call("set_workspace_storage", {{"workspace_id", workspace_id}, {"storage_gi", 10}});
    ASSERT_FALSE(smaller.ok);
    EXPECT_EQ(smaller.error.code, "invalid_input");
    EXPECT_EQ(smaller.error.path, "/storage_gi");
    EXPECT_EQ(store->get_workspace(workspace_id)->storage_gi, 20);
    // The store refuses it too (PostgreSQL: enforce_workspace_storage_growth).
    const auto direct = store->set_workspace_storage(workspace_id, 10);
    ASSERT_FALSE(direct);
    EXPECT_EQ(direct.error().code, "invalid_input");

    for (const json& input : {json{{"workspace_id", lake::control::new_uuid_v4()}, {"storage_gi", 30}},
                              json{{"workspace_id", "not-a-uuid"}, {"storage_gi", 30}},
                              json{{"workspace_id", workspace_id}, {"storage_gi", 0}},
                              json{{"workspace_id", workspace_id}},
                              json{{"storage_gi", 30}}}) {
        const auto refused = call("set_workspace_storage", input);
        ASSERT_FALSE(refused.ok) << input;
        EXPECT_EQ(refused.error.code, "invalid_input") << input;
    }
    const auto person = call("set_workspace_storage",
                             {{"workspace_id", workspace_id}, {"storage_gi", 30}}, owner_taap);
    ASSERT_FALSE(person.ok);
    EXPECT_EQ(person.error.code, "unauthorized") << "even the space's owner";
    EXPECT_EQ(store->get_workspace(workspace_id)->storage_gi, 20);

    ASSERT_TRUE(store->update_workspace(workspace_id, {.desired = "deleted"}));
    const auto deleted = call("set_workspace_storage", {{"workspace_id", workspace_id}, {"storage_gi", 40}});
    ASSERT_FALSE(deleted.ok);
    EXPECT_EQ(deleted.error.code, "invalid_input");

    const auto events = lake::test::audit_events(*store, {.limit = 200});
    ASSERT_TRUE(events) << events.error().message;
    int successes = 0;
    int refusals = 0;
    for (const auto& event : *events) {
        if (event.draft.action != "set_workspace_storage") {
            continue;
        }
        successes += event.draft.outcome == "success";
        refusals += event.draft.outcome != "success";
        if (event.draft.outcome == "success") {
            EXPECT_EQ(event.draft.object, "workspace/" + workspace_id + "/storage");
        }
    }
    EXPECT_EQ(successes, 2);
    EXPECT_GE(refusals, 3);
}

// Lake's store and every workspace have a policy (daily, keep 7, nothing requested); a request
// stamps requested_at and may change the schedule and keep; the operator's status is read back;
// unknown targets and bad schedules are invalid_input; only the operator reads or requests.
TEST_P(Volumes, SnapshotPoliciesAreContentFreeAndReadBackWithTheOperatorsStatus) {
    const auto listed = call("volume_snapshots", json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    const json& policies = listed.value.at("policies");
    ASSERT_GE(policies.size(), 2U) << policies;
    EXPECT_EQ(policies[0]["target"], "store");
    bool found = false;
    for (const json& policy : policies) {
        EXPECT_EQ(policy.at("schedule"), "0 3 * * *");
        EXPECT_EQ(policy.at("keep"), 7);
        EXPECT_EQ(policy.at("requested_at"), "");
        EXPECT_TRUE(policy.at("status").is_null());
        found = found || policy.at("target") == workspace_id;
    }
    EXPECT_TRUE(found) << "a new workspace gets the default policy";

    const auto requested = call("request_volume_snapshot", {{"target", "store"}});
    ASSERT_TRUE(requested.ok) << requested.error.message;
    EXPECT_NE(requested.value.at("requested_at"), "");
    EXPECT_EQ(requested.value.at("keep"), 7);

    const auto changed = call("request_volume_snapshot",
                              {{"target", workspace_id}, {"schedule", "*/30 * * * *"}, {"keep", 3}});
    ASSERT_TRUE(changed.ok) << changed.error.message;
    EXPECT_EQ(changed.value.at("schedule"), "*/30 * * * *");
    EXPECT_EQ(changed.value.at("keep"), 3);
    EXPECT_NE(changed.value.at("requested_at"), "");

    report_snapshot(workspace_id, "data-lake-ws-0-20261003t120000z", true);
    const auto one = call("volume_snapshots", {{"target", workspace_id}});
    ASSERT_TRUE(one.ok) << one.error.message;
    ASSERT_EQ(one.value.at("policies").size(), 1U);
    const json& status = one.value.at("policies")[0].at("status");
    ASSERT_TRUE(status.is_object()) << one.value;
    EXPECT_EQ(status.at("snapshot_name"), "data-lake-ws-0-20261003t120000z");
    EXPECT_EQ(status.at("ready_to_use"), true);
    EXPECT_TRUE(status.at("snapshot_at").get<std::string>().starts_with("2026-10-03T12:00:00"));

    for (const json& input : {json{{"target", lake::control::new_uuid_v4()}},
                              json{{"target", "everything"}},
                              json{{"target", "store"}, {"schedule", "@daily"}},
                              json{{"target", "store"}, {"schedule", "0 3 * *"}},
                              json{{"target", "store"}, {"keep", 0}},
                              json::object()}) {
        const auto refused = call("request_volume_snapshot", input);
        ASSERT_FALSE(refused.ok) << input;
        EXPECT_EQ(refused.error.code, "invalid_input") << input;
    }
    EXPECT_EQ(call("volume_snapshots", {{"target", lake::control::new_uuid_v4()}}).error.code,
              "invalid_input");
    EXPECT_EQ(call("volume_snapshots", json::object(), owner_taap).error.code, "unauthorized");
    EXPECT_EQ(call("request_volume_snapshot", {{"target", "store"}}, owner_taap).error.code,
              "unauthorized");

    // A deleted workspace takes no new requests; its policy and status stay, as its retained
    // snapshots do.
    ASSERT_TRUE(store->update_workspace(workspace_id, {.desired = "deleted"}));
    EXPECT_EQ(call("request_volume_snapshot", {{"target", workspace_id}}).error.code,
              "invalid_input");
    const auto kept = call("volume_snapshots", {{"target", workspace_id}});
    ASSERT_TRUE(kept.ok) << kept.error.message;
    EXPECT_TRUE(kept.value.at("policies")[0].at("status").is_object());
}

INSTANTIATE_TEST_SUITE_P(Stores, Volumes, testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

} // namespace

namespace {

// Review of #525: a schedule is the operator's five standard cron fields, and one that can never
// fire (no selected month has a selected day while the day of week is unrestricted) is refused,
// as the operator would find no next time for it.
TEST(VolumeSchedules, ImpossibleSchedulesAreRefused) {
    using lake::control::valid_snapshot_schedule;
    for (const char* good : {"0 3 * * *", "*/15 * * * *", "0 0 29 2 *", "0 0 31 2 1", "0 0 31 */2 *",
                             "0 0 1-7 * *", "5,35 1-23/2 * 1,6 0-6", "0 0 30 1-12 *"}) {
        EXPECT_TRUE(valid_snapshot_schedule(good)) << good;
    }
    for (const char* bad : {"0 0 31 2 *", "0 0 30 2 *", "0 0 31 4,6,9,11 *", "60 * * * *",
                            "0 24 * * *", "0 3 0 * *", "0 3 * 13 *", "0 3 * * 7", "@daily",
                            "0 3 * *", "0 3 * * * *", "0 3 * * 1,", "0 3 5-1 * *", "*/0 * * * *",
                            "0  3 * * *", ""}) {
        EXPECT_FALSE(valid_snapshot_schedule(bad)) << bad;
    }
}

} // namespace

// Review of #525: request_volume_snapshot refuses a schedule that can never fire.
TEST_P(Volumes, AScheduleThatNeverFiresIsRefused) {
    const auto refused = call("request_volume_snapshot", {{"target", "store"}, {"schedule", "0 0 31 2 *"}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/schedule");
}

// Review of #525: tenants see a short storage reason code, never the operator's message (which can
// carry Kubernetes API errors); the operator and the instance administrator see both.
TEST_P(Volumes, TenantsSeeAStorageReasonAndNotTheOperatorsMessage) {
    const std::string message = "growing claim data-x to 20 GiB was refused: admission webhook denied";
    if (database_uri.empty()) {
        ASSERT_TRUE(static_cast<lake::control::InMemoryStore&>(*store).set_workspace_storage_status(
            workspace_id, 5, "Failed", message));
    } else {
        pqxx::connection connection(database_uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspaces SET storage_gi_observed = 5, storage_phase = 'Failed', "
                       "storage_message = $2 WHERE id = $1::uuid",
                       workspace_id, message);
        tx.commit();
    }
    const json input{{"customer_id", customer_id}, {"workspace_id", workspace_id}};
    const auto tenant = call("get_workspace", input, owner_taap);
    ASSERT_TRUE(tenant.ok) << tenant.error.message;
    EXPECT_EQ(tenant.value.at("storage_reason"), "failed");
    EXPECT_EQ(tenant.value.at("storage_phase"), "Failed");
    EXPECT_FALSE(tenant.value.contains("storage_message")) << tenant.value;
    EXPECT_EQ(tenant.value.dump().find("admission"), std::string::npos);
    const auto listed = call("list_workspaces", {{"customer_id", customer_id}}, owner_taap);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value.dump().find("admission"), std::string::npos);
    const auto admin = call("get_workspace", input);
    ASSERT_TRUE(admin.ok) << admin.error.message;
    EXPECT_EQ(admin.value.at("storage_message"), message);
}

namespace {

// Review of #525: a store that cannot answer is store_unavailable, not the caller's invalid_input.
TEST(VolumeStoreFailures, AStoreThatCannotAnswerIsUnavailable) {
    auto store = lake::control::make_postgres_store(
        "postgresql://lake@127.0.0.1:1/unreachable?connect_timeout=1",
        "766f6c756d65732d696e7374616e63652d6b656b2d746573742d303030303030");
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.acting_identity = "admin";
    context.operator_credential = true;
    const std::string workspace = "00000000-0000-4000-8000-000000000001";
    for (const auto& [name, input] : std::vector<std::pair<std::string, json>>{
             {"set_workspace_storage", {{"workspace_id", workspace}, {"storage_gi", 20}}},
             {"request_volume_snapshot", {{"target", workspace}}},
             {"request_volume_snapshot", {{"target", "store"}}},
             {"volume_snapshots", json::object()}}) {
        const auto entry = registry.find(name);
        ASSERT_TRUE(entry) << name;
        const auto result = entry->handler(input, context);
        ASSERT_FALSE(result.ok) << name;
        EXPECT_EQ(result.error.code, "store_unavailable") << name << " " << input;
    }
}

} // namespace
