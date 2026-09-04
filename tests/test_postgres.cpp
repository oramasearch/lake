#include <gtest/gtest.h>

#include <cstdlib>

#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

TEST(PostgresIntegration, RepositoryLifecycleRoundTripsWithAuditEvents) {
    const char* uri = std::getenv("LAKE_TEST_DATABASE_URI");
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DATABASE_URI is not configured";
    }

    auto store = lake::control::make_postgres_store(uri);
    ASSERT_TRUE(store->ping());

    const auto user = store->create_user({"", "", "Postgres lifecycle"});
    ASSERT_TRUE(user);
    const std::string customer_name = "pg" + lake::control::new_uuid_v4().substr(0, 8);
    const auto source = store->create_customer({"individual", user->id, "", customer_name});
    ASSERT_TRUE(source);

    const auto destination_user = store->create_user({"", "", "Postgres destination"});
    ASSERT_TRUE(destination_user);
    const std::string destination_name = "pg" + lake::control::new_uuid_v4().substr(0, 8);
    const auto destination =
        store->create_customer({"individual", destination_user->id, "", destination_name});
    ASSERT_TRUE(destination);

    const auto created = store->create_repository(
        {source->id, "liquid", "repository lifecycle integration", "private"}, "admin");
    ASSERT_TRUE(created);
    const auto renamed = store->rename_repository(created->id, "liquid-next", "admin");
    ASSERT_TRUE(renamed);
    const auto transferred = store->transfer_repository(renamed->id, destination->id, "admin");
    ASSERT_TRUE(transferred);
    const auto visible =
        store->set_repository_visibility(transferred->id, "organization", "admin");
    ASSERT_TRUE(visible);
    const auto deleted = store->delete_repository(
        visible->id, {"2030-01-01T00:00:00Z", "2030-01-31T00:00:00Z"}, "admin");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->customer_id, destination->id);
    EXPECT_EQ(deleted->name, "liquid-next");
    EXPECT_EQ(deleted->visibility, "organization");
    EXPECT_EQ(deleted->deleted_at, "2030-01-01 00:00:00+00");
    EXPECT_EQ(deleted->retention_until, "2030-01-31 00:00:00+00");

    const auto events = store->list_repository_audit_events(deleted->id, 100);
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 5u);
    EXPECT_EQ(events->front().action, "repository.delete");
    EXPECT_EQ(events->back().action, "repository.create");
    const auto denied = store->rename_repository(deleted->id, "again", "admin");
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "conflict");
}

} // namespace
