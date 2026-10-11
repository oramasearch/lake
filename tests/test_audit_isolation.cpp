#include "audit_test_support.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <set>
#include <string>

namespace {

class AuditIsolation : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            const char* uri = lake::test::postgres_uri();
            if (!uri) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            store = lake::control::make_postgres_store(uri, std::getenv("LAKE_INSTANCE_KEK"));
        } else {
            store = std::make_unique<lake::control::InMemoryStore>();
        }
        context.store = store.get();
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = std::getenv("LAKE_INSTANCE_KEK");
        lake::api::register_core_capabilities(registry, "test");
    }

    std::unique_ptr<lake::control::Store> store;
    lake::api::Registry registry;
    lake::api::ApiContext context;
};

TEST_P(AuditIsolation, ExactRequestAndCursorSurviveEarlierEvents) {
    constexpr int earlier_events = 1105;
    const lake::audit::IdentityOptions options{.instance_kek = context.instance_kek};
    for (int index = 0; index < earlier_events; ++index) {
        const auto event = lake::audit::record(*store,
                                               {.kind = "fixture",
                                                .object = "repository/target",
                                                .action = "earlier",
                                                .outcome = "success"},
                                               options);
        ASSERT_TRUE(event) << event.error().message;
    }
    const std::string request_id = lake::control::new_uuid_v4();
    const auto target = lake::audit::record(*store,
                                            {.kind = "fixture",
                                             .object = "repository/target",
                                             .action = "delete_repository",
                                             .outcome = "success",
                                             .request_id = request_id},
                                            options);
    ASSERT_TRUE(target) << target.error().message;

    // Prove that the old assertion would miss this same-object record.
    const auto first_page =
        store->list_audit_events({.object = "repository/target", .limit = 1000});
    ASSERT_TRUE(first_page);
    ASSERT_EQ(first_page->size(), 1000U);
    for (const auto& event : *first_page) {
        EXPECT_NE(event.id, target->id);
    }

    const auto exact = registry.call("audit", {{"request_id", request_id}, {"limit", 1}}, context);
    ASSERT_TRUE(exact.ok) << exact.error.message;
    ASSERT_EQ(exact.value.at("items").size(), 1U);
    EXPECT_EQ(exact.value.at("items").front().at("id"), target->id);
    EXPECT_EQ(exact.value.at("items").front().at("action"), "delete_repository");

    // Several pages, including an exact page boundary, preserve the signed chain.
    const auto all = lake::test::audit_events(*store, {.limit = 41});
    ASSERT_TRUE(all) << all.error().message;
    ASSERT_EQ(all->size(), earlier_events + 2U);
    const auto verified = lake::audit::verify(*store, *all);
    ASSERT_TRUE(verified) << verified.error().message;
    const auto tail = store->list_audit_events({.after_id = target->id});
    ASSERT_TRUE(tail);
    ASSERT_EQ(tail->size(), 1U);
    EXPECT_EQ(tail->front().draft.action, "audit");
    const auto exhausted = store->list_audit_events({.after_id = all->back().id});
    ASSERT_TRUE(exhausted);
    EXPECT_TRUE(exhausted->empty());
    const auto missing = store->list_audit_events({.request_id = "absent"});
    ASSERT_TRUE(missing);
    EXPECT_TRUE(missing->empty());
    const auto unknown_cursor =
        store->list_audit_events({.after_id = lake::control::new_uuid_v4()});
    ASSERT_TRUE(unknown_cursor);
    EXPECT_TRUE(unknown_cursor->empty());
}

// lake#445: a person's activity is newest first, names who acted (the agent working for them
// included), shows what a workspace call did, and leaves out the reads a browser polls.
TEST_P(AuditIsolation, ActivityIsNewestFirstNamedAndQuiet) {
    const std::string taap_id = "taap:1:ed25519:activity-" + lake::control::new_uuid_v4();
    const auto person = store->create_user(
        {.taap_id = taap_id, .email = taap_id + "@example.test", .display_name = "Ada Lovelace"});
    ASSERT_TRUE(person) << person.error().message;
    const lake::audit::IdentityOptions options{.instance_kek = context.instance_kek};
    for (const lake::audit::EventDraft& draft :
         {lake::audit::EventDraft{.kind = "capability",
                                  .principal = taap_id,
                                  .object = "repository/r",
                                  .action = "create_repository",
                                  .outcome = "success"},
          lake::audit::EventDraft{.kind = "capability",
                                  .principal = taap_id,
                                  .object = "capability/list_users",
                                  .action = "list_users",
                                  .outcome = "success"},
          lake::audit::EventDraft{.kind = "capability",
                                  .principal = taap_id,
                                  .object = "capability/list_users",
                                  .action = "list_users",
                                  .outcome = "denied"},
          lake::audit::EventDraft{.kind = "workspace.act",
                                  .principal = taap_id,
                                  .object = "customer/c",
                                  .action = "workspace_act",
                                  .outcome = "success",
                                  .details = {{"capability", "create_agent"},
                                              {"acting_agent", "agent-project"}}}}) {
        const auto recorded = lake::audit::record(*store, draft, options);
        ASSERT_TRUE(recorded) << recorded.error().message;
    }

    lake::api::ApiContext own;
    own.store = store.get();
    own.authorized = true;
    own.acting_user_id = person->id;
    own.acting_identity = taap_id;
    own.instance_kek = context.instance_kek;
    const auto actions = [](const nlohmann::json& items) {
        std::vector<std::string> out;
        for (const nlohmann::json& item : items) {
            if (item.at("action") != "my_audit") {
                out.push_back(item.at("action").get<std::string>() + "/" +
                              item.at("outcome").get<std::string>());
            }
        }
        return out;
    };

    const auto quiet = registry.call("my_audit", {{"limit", 100}}, own);
    ASSERT_TRUE(quiet.ok) << quiet.error.message;
    const nlohmann::json& items = quiet.value.at("items");
    EXPECT_EQ(actions(items),
              (std::vector<std::string>{
                  "workspace_act/success", "list_users/denied", "create_repository/success"}))
        << "newest first, the successful read left out, the denied read kept";
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(items.front().at("principal_name"), "Project Agent for Ada Lovelace");
    EXPECT_EQ(items.front().at("capability"), "create_agent");
    EXPECT_EQ(items.back().at("principal_name"), "Ada Lovelace");
    EXPECT_FALSE(items.back().contains("capability"));

    const auto everything =
        registry.call("my_audit", {{"limit", 100}, {"include_reads", true}}, own);
    ASSERT_TRUE(everything.ok) << everything.error.message;
    EXPECT_EQ(actions(everything.value.at("items")),
              (std::vector<std::string>{"workspace_act/success",
                                        "list_users/denied",
                                        "list_users/success",
                                        "create_repository/success"}));
    EXPECT_EQ(everything.value.at("items").front().at("action"), "my_audit")
        << "the earlier my_audit read is the newest event once reads are included";

    const auto oldest = registry.call(
        "my_audit", {{"limit", 100}, {"order", "oldest"}, {"include_reads", true}}, own);
    ASSERT_TRUE(oldest.ok) << oldest.error.message;
    EXPECT_EQ(actions(oldest.value.at("items")),
              (std::vector<std::string>{"create_repository/success",
                                        "list_users/success",
                                        "list_users/denied",
                                        "workspace_act/success"}));

    const auto operator_view =
        registry.call("audit", {{"principal", taap_id}, {"limit", 1}}, context);
    ASSERT_TRUE(operator_view.ok) << operator_view.error.message;
    ASSERT_EQ(operator_view.value.at("items").size(), 1U);
    EXPECT_EQ(operator_view.value.at("items").front().at("action"), "my_audit")
        << "the operator's audit query is newest first and keeps reads";
}

TEST_P(AuditIsolation, CustomerLookupFiltersBeforeLimiting) {
    const auto initial = lake::test::audit_events(*store);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(initial->empty());
    const auto owner = store->create_user({"taap:1:ed25519:owner", "owner@example.test", "Owner"});
    ASSERT_TRUE(owner);
    const auto target = store->create_customer({"individual", owner->id, "", "owner"});
    ASSERT_TRUE(target) << target.error().message;
    for (int index = 0; index < 3; ++index) {
        const std::string name = "other" + std::to_string(index);
        const auto user =
            store->create_user({"taap:1:ed25519:" + name, name + "@example.test", name});
        ASSERT_TRUE(user);
        ASSERT_TRUE(store->create_customer({"individual", user->id, "", name}));
    }
    const auto found = store->list_customers(1, owner->id);
    ASSERT_TRUE(found);
    ASSERT_EQ(found->size(), 1U);
    EXPECT_EQ(found->front().id, target->id);
    const auto missing = store->list_customers(1, lake::control::new_uuid_v4());
    ASSERT_TRUE(missing);
    EXPECT_TRUE(missing->empty());
}

INSTANTIATE_TEST_SUITE_P(Stores,
                         AuditIsolation,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

} // namespace

// Decision 52 (lake#317): one function decides which log a capability's events go to, and every
// registered capability is named by it, so a new capability cannot be added without deciding.
TEST(AuditRouting, EveryRegisteredCapabilityIsClassified) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    // Billing is registered only where it is on (lake#564), and classified like the rest.
    lake::api::register_billing_capabilities(registry);
    std::set<std::string> registered;
    for (const auto& capability : registry.list()) {
        registered.insert(capability->name);
        EXPECT_TRUE(lake::api::audit_log_of(capability->name).has_value())
            << capability->name << " is in neither audit table (src/api/audit_routing.cpp)";
    }
    const auto central = lake::api::central_audit_capabilities();
    const auto workspace = lake::api::workspace_audit_capabilities();
    EXPECT_TRUE(std::ranges::is_sorted(central));
    EXPECT_TRUE(std::ranges::is_sorted(workspace));
    for (const std::string_view name : central) {
        EXPECT_FALSE(std::ranges::binary_search(workspace, name)) << name << " is in both";
        EXPECT_TRUE(registered.contains(std::string(name))) << name << " is not registered";
    }
    for (const std::string_view name : workspace) {
        EXPECT_TRUE(registered.contains(std::string(name))) << name << " is not registered";
    }
    // The identity, control-plane and audit capabilities named in decision 52 stay central; the
    // workspace calls and actions on a workspace's data go to it.
    for (const char* name : {"sign_in_password", "create_membership", "create_grant",
                             "create_workspace", "create_token", "create_customer",
                             "create_organization", "create_object", "audit", "my_audit",
                             "set_audit_retention"}) {
        EXPECT_EQ(lake::api::audit_log_of(name), lake::api::AuditLog::Central) << name;
    }
    for (const char* name : {"workspace_act", "workspace_query", "workspace_sql",
                             "create_repository", "register_application", "create_app",
                             "build_image", "put_secret", "upload_drive_files", "create_issue",
                             "create_stream", "create_store"}) {
        EXPECT_EQ(lake::api::audit_log_of(name), lake::api::AuditLog::Workspace) << name;
    }

    // Migration 0063 assigns the central events of workspace actions only.
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0063_audit_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto list = schema.find("e.action = ANY (ARRAY[", start);
    ASSERT_NE(list, std::string::npos);
    const auto end = schema.find("])", list);
    const std::string names = schema.substr(list, end - list);
    const auto actions = lake::api::workspace_audit_actions();
    std::size_t listed = 0;
    const std::regex quoted("'([a-z_0-9]+)'");
    for (auto match = std::sregex_iterator(names.begin(), names.end(), quoted);
         match != std::sregex_iterator(); ++match) {
        EXPECT_TRUE(std::ranges::binary_search(actions, (*match)[1].str())) << (*match)[1];
        ++listed;
    }
    // The copy reapplies the assignment with the current list, so a later workspace capability
    // is covered there without changing the migration.
    EXPECT_GT(listed, 0U);
}
