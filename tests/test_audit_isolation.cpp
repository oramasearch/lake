#include "audit_test_support.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

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
