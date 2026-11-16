// SPDX-License-Identifier: MIT

#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <liquid/liquid.hpp>
#include <pqxx/pqxx>

#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/application_user_records.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/platform_records.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/stream_owners.hpp"
#include "lake/search/index.hpp"
#include "repository_test_support.hpp"
#include "temporary_directory.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

class TemporarySearchStore {
public:
    TemporarySearchStore() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("postgres-search-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporarySearchStore() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> search_bytes(const std::string& value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::string create_search_change(const std::filesystem::path& path, const std::string& content) {
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(path.parent_path().parent_path() / "store"));
    EXPECT_TRUE(customer_store) << customer_store.error().message;
    if (!customer_store) {
        return {};
    }
    liquid::repository_options options;
    options.store = &*customer_store;
    auto opened = liquid::runtime::block_on(liquid::repository::create(path, options));
    EXPECT_TRUE(opened) << opened.error().message;
    if (!opened) {
        return {};
    }
    auto repository = std::move(*opened);
    const auto written =
        liquid::runtime::block_on(repository.write("document.txt", search_bytes(content)));
    EXPECT_TRUE(written) << written.error().message;
    const auto change = liquid::runtime::block_on(repository.cut("search fixture"));
    EXPECT_TRUE(change) << change.error().message;
    return change ? liquid::to_string(change->value) : "";
}

TEST(PostgresWorkspaces, MigrationRecordsAmbiguityAndMakesRoutingImmutable) {
    const char* uri = lake::test::postgres_uri();
    if (!uri) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is absent";
    }
    auto store = lake::control::make_postgres_store(uri, "workspace-migration-test");
    std::vector<std::string> customers;
    std::vector<std::string> workspaces;
    std::vector<std::string> tasks;
    for (int index = 0; index < 3; ++index) {
        const auto user = store->create_user({"taap:migration-" + std::to_string(index), "", "Migration"});
        ASSERT_TRUE(user);
        const auto customer = store->create_customer({"individual", user->id, "", "migration-" + std::to_string(index)});
        ASSERT_TRUE(customer);
        const auto workspace = store->create_workspace({customer->id, "workspace", "dev", ""});
        ASSERT_TRUE(workspace);
        customers.push_back(customer->id);
        workspaces.push_back(workspace->zetta_service_id);
        const auto task = store->create_agent_task({.customer_id = customer->id, .agent = "agent-project",
            .kind = "test", .workspace_id = workspace->id, .scopes = {"workspace_query"},
            .requested_by_user_id = user->id, .requested_by_taap_id = "taap:migration-" + std::to_string(index)});
        ASSERT_TRUE(task) << task.error().message;
        tasks.push_back(task->id);
    }
    ASSERT_TRUE(store->create_service({customers[1], "zetta", "second", ""}));
    ASSERT_TRUE(store->withdraw_service(workspaces[2]));
    const auto repository = store->create_repository({customers[0], "source", "", "private",
                                                      lake::test::repository_workspace(
                                                          *store, customers[0])});
    ASSERT_TRUE(repository);
    const auto original_workspace = store->list_workspaces(customers[0])->front().id;
    const auto app = store->create_web_app({.customer_id = customers[0], .workspace_id = original_workspace,
        .name = "app", .mode = "image", .image = "app:v1", .port = 3000, .host = "migration.test"});
    ASSERT_TRUE(app) << app.error().message;
    const auto build = store->create_build({.customer_id = customers[0], .workspace_id = original_workspace,
        .repository = repository->id, .revision = "main", .target = "app:v1"});
    ASSERT_TRUE(build) << build.error().message;
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("DROP TRIGGER agent_task_workspace_routing ON agent_tasks");
        tx.exec("DROP TRIGGER app_workspace_routing ON apps");
        tx.exec("DROP TRIGGER build_workspace_routing ON builds");
        tx.exec("UPDATE agent_tasks SET workspace_id = NULL");
        tx.exec("UPDATE apps SET workspace_id = NULL");
        tx.exec("UPDATE builds SET workspace_id = NULL");
        tx.exec("UPDATE services SET workspace_id = NULL");
        tx.exec("UPDATE services SET slug='ws-' || left(replace(customer_id::text,'-',''),12) WHERE is_default");
        // Repositories name workspaces too (0051); before 0038 there were none to name.
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec("UPDATE repositories SET workspace_id = NULL WHERE workspace_id IS NOT NULL");
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.exec("DELETE FROM workspaces");
        for (const std::string table : {"agent_tasks", "apps", "builds"}) {
            tx.exec("ALTER TABLE " + table + " DROP CONSTRAINT " + table + "_workspace_id_fkey");
            tx.exec("ALTER TABLE " + table + " ADD CONSTRAINT " + table +
                    "_workspace_id_fkey FOREIGN KEY (workspace_id) REFERENCES services(id)");
        }
        tx.exec("DELETE FROM schema_migrations WHERE version = '0039_workspaces'");
        tx.exec("DELETE FROM schema_migrations WHERE version = '0038_workspace_data_plane'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0038_workspace_data_plane:");
    ASSERT_NE(start, std::string::npos);
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec(schema.substr(start));
        EXPECT_EQ(tx.exec("SELECT count(*) FROM workspace_assignments WHERE outcome = 'sole_default'")[0][0].as<int>(), 3);
        EXPECT_EQ(tx.exec("SELECT count(*) FROM workspace_assignments WHERE outcome = 'needs_selection'")[0][0].as<int>(), 2);
        tx.commit();
    }
    EXPECT_EQ(store->get_agent_task(tasks[0])->workspace_id, customers[0]);
    EXPECT_EQ(store->get_web_app(app->id)->workspace_id, customers[0]);
    EXPECT_EQ(store->get_build(build->id)->workspace_id, customers[0]);
    EXPECT_TRUE(store->get_agent_task(tasks[1])->workspace_id.empty());
    EXPECT_TRUE(store->get_agent_task(tasks[2])->workspace_id.empty());
    for (std::size_t index = 0; index < workspaces.size(); ++index) {
        const auto& id = workspaces[index];
        const auto migrated = store->get_workspace(customers[index]);
        ASSERT_TRUE(migrated);
        EXPECT_EQ(migrated->environment, "prod");
        EXPECT_EQ(migrated->zetta_service_id, id);
        EXPECT_EQ(store->get_service(id)->workspace_id, migrated->id);
        EXPECT_EQ(store->get_service(id)->slug, lake::control::managed_service_slug("ws", migrated->id));
    }
    EXPECT_EQ(store->create_agent_task({.customer_id = customers[0], .agent = "agent-project", .kind = "test"}).error().code,
              "invalid_input");
    EXPECT_THROW(([&] {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE agent_tasks SET workspace_id = $1::uuid WHERE id = $2::uuid", workspaces[1], tasks[0]);
        tx.commit();
    })(), pqxx::check_violation);
}

TEST(PostgresWorkspaces, ContractNotifiesEveryChangeAndKeepsObservedStateOperatorOwned) {
    const char* uri = lake::test::postgres_uri();
    if (!uri) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is absent";
    }
    auto store = lake::control::make_postgres_store(uri, "workspace-contract-test");
    const auto user = store->create_user({"taap:workspace-contract", "", "Workspace"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "contract"});
    ASSERT_TRUE(customer);
    pqxx::connection listener(uri);
    std::vector<std::string> notifications;
    listener.listen("lake_workspaces", [&](pqxx::notification note) {
        notifications.emplace_back(note.payload);
    });
    const auto created = store->create_workspace({customer->id, "Research", "dev", "owner"});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_NE(created->id, created->zetta_service_id);
    EXPECT_EQ(store->get_service(created->zetta_service_id)->workspace_id, created->id);
    listener.await_notification(5, 0);
    ASSERT_EQ(notifications.size(), 1);
    EXPECT_EQ(notifications.back(), created->id);
    const auto changed = store->update_workspace(created->id, {.name = "Renamed"});
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->generation, 2);
    EXPECT_EQ(changed->observed_generation, 0);
    EXPECT_EQ(changed->phase, "Pending");
    listener.await_notification(5, 0);
    EXPECT_EQ(notifications.size(), 2);
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspaces SET phase='Ready', observed_generation=2, namespace=$2 "
                       "WHERE id=$1::uuid", created->id, "dev-ws-test");
        tx.commit();
    }
    listener.await_notification(5, 0);
    EXPECT_EQ(notifications.size(), 3);
    EXPECT_EQ(store->get_workspace(created->id)->namespace_name, "dev-ws-test");
    EXPECT_EQ(store->get_workspace(created->id)->observed_generation, 2);
    const auto deleted = store->update_workspace(created->id, {.desired = "deleted"});
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->phase, "Ready");
    EXPECT_EQ(deleted->generation, 3);
    EXPECT_EQ(store->get_service(created->zetta_service_id)->desired, "absent");
    listener.await_notification(5, 0);
    EXPECT_EQ(notifications.size(), 4);
    {
        pqxx::work tx(connection);
        EXPECT_EQ(tx.exec("SELECT count(*) FROM pg_constraint WHERE confrelid='workspaces'::regclass "
                         "AND conrelid IN ('agent_tasks'::regclass,'apps'::regclass,'builds'::regclass)")[0][0].as<int>(), 3);
        EXPECT_FALSE(tx.exec("SELECT to_regclass('workspaces_guard')")[0][0].is_null());
        tx.exec_params("UPDATE services SET workspace_id=NULL WHERE id=$1::uuid", created->zetta_service_id);
        tx.exec_params("DELETE FROM workspaces WHERE id=$1::uuid", created->id);
        tx.commit();
    }
    listener.await_notification(5, 0);
    EXPECT_EQ(notifications.size(), 5);
}

TEST(PostgresCapabilities, OrganizationAndPolicyRoundTrip) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }

    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.acting_identity = "admin";
    context.request_id = "postgres-integration";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    auto call = [&](const std::string& name, json input = json::object()) {
        context.request_id = lake::control::new_uuid_v4();
        return registry.call(name, input, context);
    };
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);

    ASSERT_TRUE(call("server_info").ok);
    const ApiResult first_user = call("create_user",
                                      {{"taap_id", "taap:1:ed25519:first-" + suffix},
                                       {"email", "first-" + suffix + "@example.com"},
                                       {"display_name", "First"}});
    const ApiResult second_user = call("create_user",
                                       {{"taap_id", "taap:1:ed25519:second-" + suffix},
                                        {"email", "second-" + suffix + "@example.com"},
                                        {"display_name", "Second"}});
    ASSERT_TRUE(first_user.ok) << first_user.error.message;
    ASSERT_TRUE(second_user.ok) << second_user.error.message;
    ASSERT_TRUE(call("list_users").ok);
    ASSERT_TRUE(call("get_user", {{"id", first_user.value.at("id")}}).ok);

    const ApiResult organization = call("create_organization",
                                        {{"taap_id", "taap:1:ed25519:org-" + suffix},
                                         {"display_name", "Integration organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    ASSERT_TRUE(call("list_organizations").ok);
    ASSERT_TRUE(call("get_organization", {{"id", organization.value.at("id")}}).ok);

    const ApiResult customer = call("create_customer",
                                    {{"customer_type", "organization"},
                                     {"organization_id", organization.value.at("id")},
                                     {"name", "org-" + suffix}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    ASSERT_TRUE(call("list_customers").ok);
    ASSERT_TRUE(call("get_customer", {{"id", customer.value.at("id")}}).ok);

    const std::string workspace =
        lake::test::repository_workspace(*store, customer.value.at("id"));
    const ApiResult missing_workspace =
        call("create_repository",
             {{"customer_id", customer.value.at("id")}, {"name", "repo-" + suffix}});
    EXPECT_EQ(missing_workspace.error.code, "invalid_input");
    EXPECT_EQ(missing_workspace.error.path, "/workspace_id");
    const ApiResult repository = call("create_repository",
                                      {{"customer_id", customer.value.at("id")},
                                       {"workspace_id", workspace},
                                       {"name", "repo-" + suffix}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    EXPECT_EQ(repository.value.at("workspace_id"), workspace);
    ASSERT_TRUE(call("list_repositories", {{"customer_id", customer.value.at("id")}}).ok);
    ASSERT_TRUE(call("get_repository", {{"id", repository.value.at("id")}}).ok);
    const ApiResult duplicate = call("create_repository",
                                     {{"customer_id", customer.value.at("id")},
                                      {"workspace_id", workspace},
                                      {"name", "repo-" + suffix}});
    EXPECT_EQ(duplicate.error.code, "conflict");
    EXPECT_EQ(duplicate.error.message,
              "a repository named 'repo-" + suffix + "' already exists");
    EXPECT_EQ(duplicate.error.path, "/name");
    EXPECT_EQ(duplicate.error.message.find("ERROR:"), std::string::npos);
    EXPECT_EQ(duplicate.error.message.find("DETAIL:"), std::string::npos);

    const ApiResult destination_user =
        call("create_user", {{"display_name", "Repository destination"}});
    ASSERT_TRUE(destination_user.ok) << destination_user.error.message;
    const ApiResult destination = call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", destination_user.value.at("id")},
                                        {"name", "destination-" + suffix}});
    ASSERT_TRUE(destination.ok) << destination.error.message;
    ASSERT_TRUE(call("set_repository_visibility",
                     {{"id", repository.value.at("id")}, {"visibility", "organization"}})
                    .ok);
    ASSERT_TRUE(call("rename_repository",
                     {{"id", repository.value.at("id")}, {"name", "renamed-" + suffix}})
                    .ok);
    // A repository stays in its space and workspace (lake#315).
    const ApiResult transferred =
        call("transfer_repository",
             {{"id", repository.value.at("id")}, {"customer_id", destination.value.at("id")}});
    EXPECT_EQ(transferred.error.code, "conflict");
    const ApiResult deleted = call("delete_repository", {{"id", repository.value.at("id")}});
    const std::string deletion_request = context.request_id;
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_FALSE(deleted.value.at("deleted_at").is_null());
    const ApiResult recreated = call("create_repository",
                                     {{"customer_id", customer.value.at("id")},
                                      {"workspace_id", workspace},
                                      {"name", "renamed-" + suffix}});
    ASSERT_TRUE(recreated.ok) << recreated.error.message;
    EXPECT_NE(recreated.value.at("id"), repository.value.at("id"));
    EXPECT_EQ(recreated.value.at("name"), "renamed-" + suffix);
    EXPECT_EQ(
        call("rename_repository", {{"id", repository.value.at("id")}, {"name", "again-" + suffix}})
            .error.code,
        "conflict");
    const ApiResult audit =
        call("audit",
             {{"object", "repository/" + repository.value.at("id").get<std::string>()},
              {"outcome", "success"},
              {"request_id", deletion_request},
              {"limit", 1}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    ASSERT_EQ(audit.value.at("items").size(), 1U);
    EXPECT_EQ(audit.value.at("items").at(0).at("action"), "delete_repository");
    EXPECT_EQ(audit.value.at("items").at(0).at("outcome"), "success");

    const ApiResult team = call("create_team",
                                {{"organization_id", organization.value.at("id")},
                                 {"name", "team-" + suffix},
                                 {"display_name", "Integration team"}});
    ASSERT_TRUE(team.ok) << team.error.message;
    ASSERT_TRUE(call("list_teams", {{"organization_id", organization.value.at("id")}}).ok);
    ASSERT_TRUE(call("get_team", {{"id", team.value.at("id")}}).ok);

    const ApiResult membership = call("create_membership",
                                      {{"organization_id", organization.value.at("id")},
                                       {"principal_type", "user"},
                                       {"principal_id", first_user.value.at("id")},
                                       {"principal_taap_id", first_user.value.at("taap_id")},
                                       {"role", "owner"}});
    ASSERT_TRUE(membership.ok) << membership.error.message;
    ASSERT_TRUE(call("list_memberships", {{"organization_id", organization.value.at("id")}}).ok);

    const ApiResult invitation = call("create_invitation",
                                      {{"organization_id", organization.value.at("id")},
                                       {"team_id", team.value.at("id")},
                                       {"invitee_type", "email"},
                                       {"invitee", "second-" + suffix + "@example.com"},
                                       {"role", "member"},
                                       {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(invitation.ok) << invitation.error.message;
    ASSERT_TRUE(
        call("list_invitations",
             {{"organization_id", organization.value.at("id")}, {"team_id", team.value.at("id")}})
            .ok);
    const ApiResult accepted = call("accept_invitation",
                                    {{"invitation_id", invitation.value.at("id")},
                                     {"principal_type", "user"},
                                     {"principal_id", second_user.value.at("id")},
                                     {"principal_taap_id", second_user.value.at("taap_id")}});
    ASSERT_TRUE(accepted.ok) << accepted.error.message;

    const auto repository_object =
        store->find_policy_object("repository", repository.value.at("id"));
    ASSERT_TRUE(repository_object) << repository_object.error().message;
    // create_object places a path object from the repository's tree (lake#1030), which this
    // test has no repository store for; the object is made in the store, as lake makes them.
    const auto path_object = store->create_policy_object(
        {"path",
         // Named for the repository it sits in (lake#913).
         repository.value.at("id").get<std::string>() + ":node-" + suffix,
         repository_object->id,
         "admin",
         lake::control::new_uuid_v4(),
         true});
    ASSERT_TRUE(path_object) << path_object.error().message;
    const json path{{"id", path_object->id}};
    ASSERT_TRUE(call("get_object", {{"id", path.at("id")}}).ok);
    ASSERT_TRUE(call("list_objects", {{"parent_id", repository_object->id}}).ok);

    const ApiResult grant = call("create_grant",
                                 {{"principal_taap_id", "taap:1:ed25519:reader-" + suffix},
                                  {"object_id", repository_object->id},
                                  {"role", "read"}});
    ASSERT_TRUE(grant.ok) << grant.error.message;
    ASSERT_TRUE(call("list_grants", {{"object_id", repository_object->id}}).ok);
    const ApiResult access = call("effective_access", {{"object_id", path.at("id")}});
    ASSERT_TRUE(access.ok) << access.error.message;
    EXPECT_FALSE(access.value.at("items").empty());
}

TEST(PostgresCapabilities, OrganizationSpaceMembershipsCarryTheirGrantToTheSpace) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping());
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto founder = store->create_user(
        {"taap:1:ed25519:founder-" + suffix, "founder-" + suffix + "@example.com", "Founder"});
    const auto colleague = store->create_user(
        {"taap:1:ed25519:colleague-" + suffix, "colleague-" + suffix + "@example.com", "C"});
    ASSERT_TRUE(founder && colleague);
    auto context_for = [&](const lake::control::User& user) {
        ApiContext context;
        context.store = store.get();
        context.authorized = true;
        context.acting_identity = user.taap_id;
        context.acting_user_id = user.id;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        return context;
    };
    ApiContext owner = context_for(*founder);
    const ApiResult created = registry.call(
        "create_organization_space", {{"display_name", "Space " + suffix}}, owner);
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string organization_id = created.value.at("organization").at("id");
    const auto space_object =
        store->find_policy_object("customer", created.value.at("customer").at("id"));
    ASSERT_TRUE(space_object);
    auto may = [&](const std::string& principal, const std::string& action) {
        lake::control::AuthorizationCache cache;
        const auto decision =
            lake::control::authorize(*store, principal, space_object->id, action, cache);
        return decision && decision->allowed;
    };
    EXPECT_TRUE(may(founder->taap_id, "admin"));

    const ApiResult invitation = registry.call("create_invitation",
                                               {{"organization_id", organization_id},
                                                {"invitee_type", "email"},
                                                {"invitee", colleague->email},
                                                {"role", "admin"},
                                                {"expires_at", "2999-01-01T00:00:00Z"}},
                                               owner);
    ASSERT_TRUE(invitation.ok) << invitation.error.message;
    ApiContext invitee = context_for(*colleague);
    const ApiResult mine = registry.call("list_invitations", json::object(), invitee);
    ASSERT_TRUE(mine.ok) << mine.error.message;
    ASSERT_EQ(mine.value.at("items").size(), 1U);
    const ApiResult joined =
        registry.call("join_organization", {{"invitation_id", invitation.value.at("id")}}, invitee);
    ASSERT_TRUE(joined.ok) << joined.error.message;
    EXPECT_TRUE(may(colleague->taap_id, "admin"));
    EXPECT_TRUE(registry.call("list_invitations", json::object(), invitee).value.at("items").empty());

    owner.request_id = lake::control::new_uuid_v4();
    ASSERT_TRUE(registry
                    .call("set_member_role",
                          {{"id", joined.value.at("id")}, {"role", "member"}},
                          owner)
                    .ok);
    EXPECT_TRUE(may(colleague->taap_id, "read"));
    EXPECT_FALSE(may(colleague->taap_id, "admin"));
    owner.request_id = lake::control::new_uuid_v4();
    ASSERT_TRUE(registry.call("remove_member", {{"id", joined.value.at("id")}}, owner).ok);
    EXPECT_FALSE(may(colleague->taap_id, "read"));
    EXPECT_TRUE(may(founder->taap_id, "admin"));
}

// lake#341: migration 0054 records each forge_records row's owner, its repository's workspace,
// and the copy's rerun of it picks up late rows and repositories assigned after review.
// lake#317, migration 0063: each central event about a workspace's data is assigned to that
// workspace for the copy; the rest stay central.
TEST(PostgresAudit, MigrationRecordsEachWorkspaceEventsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    const std::string kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    auto store = lake::control::make_postgres_store(uri, kek);
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user({"taap:1:ed25519:audit-migration-" + suffix, "", "Audit"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        store->create_customer({"individual", user->id, "", "audit-migration-" + suffix});
    const auto other_user =
        store->create_user({"taap:1:ed25519:audit-other-" + suffix, "", "Other"});
    const auto other =
        store->create_customer({"individual", other_user->id, "", "audit-other-" + suffix});
    ASSERT_TRUE(customer && other);
    const auto live = store->create_workspace({customer->id, "live", "dev", ""});
    const auto doomed = store->create_workspace({customer->id, "doomed", "dev", ""});
    const auto elsewhere = store->create_workspace({other->id, "elsewhere", "dev", ""});
    ASSERT_TRUE(live && doomed && elsewhere);
    const auto owned = store->create_repository({customer->id, "owned", "", "private", live->id});
    const auto gone = store->create_repository({customer->id, "gone", "", "private", doomed->id});
    ASSERT_TRUE(owned && gone);
    const lake::audit::IdentityOptions options{.instance_kek = kek};
    const auto record = [&](lake::audit::EventDraft draft) {
        draft.kind = draft.kind.empty() ? "capability" : draft.kind;
        draft.principal = user->taap_id;
        draft.request_id = lake::control::new_uuid_v4();
        const auto recorded = lake::audit::record(*store, draft, options);
        EXPECT_TRUE(recorded) << recorded.error().message;
        return recorded->id;
    };
    const std::string pushed = record(
        {.object = "repository/" + owned->id, .action = "push", .outcome = "success"});
    const std::string queried = record({.customer_id = customer->id,
                                        .object = "capability/workspace_sql",
                                        .action = "workspace_sql",
                                        .outcome = "failure",
                                        .details = {{"workspace_id", live->id}}});
    const std::string protected_ref = record({.kind = "forge.branch_protection.refused",
                                              .object = "repository/" + owned->id + "/ref/main",
                                              .action = "reference_update",
                                              .outcome = "denied",
                                              .details = {{"repository", owned->id}}});
    const std::string deleted = record(
        {.object = "repository/" + gone->id, .action = "get_repository", .outcome = "success"});
    const std::string refused = record(
        {.object = "repository/" + owned->id, .action = "push", .outcome = "denied"});
    const std::string signed_in = record(
        {.object = "user/" + user->id, .action = "sign_in_password", .outcome = "success"});
    const std::string unnamed = record(
        {.object = "capability/workspace_act", .action = "workspace_act", .outcome = "success"});
    // A workspace of another space named by an event of this one is not trusted.
    const std::string crossed = record({.customer_id = customer->id,
                                        .object = "capability/workspace_query",
                                        .action = "workspace_query",
                                        .outcome = "success",
                                        .details = {{"workspace_id", elsewhere->id}}});
    ASSERT_TRUE(store->update_workspace(doomed->id, {.desired = "deleted"}));
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0063_audit_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0063_audit_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'audit_events' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(),
                                         found[0][1].as<std::string>()};
    };
    using Owner = std::pair<std::string, std::string>;
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(pushed), (Owner{"event_workspace", live->id}));
        EXPECT_EQ(outcome(queried), (Owner{"event_workspace", live->id}));
        EXPECT_EQ(outcome(protected_ref), (Owner{"event_workspace", live->id}))
            << "a branch protection refusal is the repository's, not an authority refusal";
        EXPECT_EQ(outcome(deleted), (Owner{"workspace_deleted", ""}));
        for (const std::string& id : {refused, signed_in, unnamed, crossed}) {
            EXPECT_TRUE(outcome(id).first.empty()) << id << " stays central";
        }
    }

    // An event an earlier replica wrote after the migration is picked up by the assignment each
    // copy reruns, with the current list of workspace actions.
    const std::string late = record(
        {.object = "repository/" + owned->id, .action = "create_issue", .outcome = "success"});
    EXPECT_TRUE(outcome(late).first.empty());
    ASSERT_TRUE(store->assign_legacy_audit_events(lake::api::workspace_audit_actions()));
    EXPECT_EQ(outcome(late), (Owner{"event_workspace", live->id}));

    // The listing pages the assigned events in append order, each with its owner.
    std::vector<std::string> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_audit_events(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            listed.push_back(row.event.id);
            if (row.event.id == pushed) {
                EXPECT_EQ(row.workspace_id, live->id);
                EXPECT_EQ(row.customer_id, customer->id);
                EXPECT_TRUE(lake::audit::verify_event(*store, row.event));
            }
            if (row.event.id == deleted) {
                EXPECT_TRUE(row.workspace_id.empty());
                EXPECT_EQ(row.outcome, "workspace_deleted");
            }
        }
        after = page->back().event.id;
    }
    const auto position = [&](const std::string& id) {
        return std::ranges::find(listed, id) - listed.begin();
    };
    for (const std::string& id : {pushed, queried, protected_ref, deleted, late}) {
        EXPECT_LT(position(id), static_cast<long>(listed.size())) << id;
    }
    EXPECT_LT(position(pushed), position(late)) << "append order";
    EXPECT_EQ(position(refused), static_cast<long>(listed.size()));
}

TEST(PostgresCapabilities, SearchFiltersRepositoryCandidates) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }

    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string customer = "search-customer-" + suffix;
    const std::string allowed_repository = "allowed-" + suffix;
    const std::string denied_repository = "denied-" + suffix;
    const auto customer_object = store->create_policy_object(
        {"customer", customer, "", "admin", "postgres-search-customer", true});
    ASSERT_TRUE(customer_object) << customer_object.error().message;
    const auto allowed_object = store->create_policy_object({"repository",
                                                             allowed_repository,
                                                             customer_object->id,
                                                             "admin",
                                                             "postgres-search-allowed",
                                                             true});
    ASSERT_TRUE(allowed_object) << allowed_object.error().message;
    const auto denied_object = store->create_policy_object({"repository",
                                                            denied_repository,
                                                            customer_object->id,
                                                            "admin",
                                                            "postgres-search-denied",
                                                            true});
    ASSERT_TRUE(denied_object) << denied_object.error().message;
    const auto grant = store->create_grant({"reader",
                                            allowed_object->id,
                                            "read",
                                            "sha512-256:postgres-search",
                                            "admin",
                                            "postgres-search-grant"});
    ASSERT_TRUE(grant) << grant.error().message;

    TemporarySearchStore search_store;
    lake::search::Service service(search_store.path());
    const std::string allowed_change =
        create_search_change(search_store.path() / customer / "repositories" / allowed_repository,
                             "postgres searchable allowed");
    const std::string denied_change =
        create_search_change(search_store.path() / customer / "repositories" / denied_repository,
                             "postgres searchable denied");
    ASSERT_TRUE(service.publish({customer, allowed_repository, "main", allowed_change}));
    ASSERT_TRUE(service.publish({customer, denied_repository, "main", denied_change}));

    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.search = &service;
    context.authorized = true;
    context.acting_identity = "reader";
    context.request_id = "postgres-search-" + suffix;
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const ApiResult result = registry.call(
        "search", {{"query", "postgres searchable"}, {"customer", customer}}, context);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("mode"), "hot");
    ASSERT_EQ(result.value.at("items").size(), 1U);
    EXPECT_EQ(result.value.at("items").front().at("repository"), allowed_repository);

    context.request_id = "postgres-search-scan-" + suffix;
    const ApiResult scan = registry.call("search",
                                         {{"query", "postgres searchable"},
                                          {"customer", customer},
                                          {"repository", allowed_repository},
                                          {"change", allowed_change}},
                                         context);
    ASSERT_TRUE(scan.ok) << scan.error.message;
    EXPECT_EQ(scan.value.at("mode"), "scan");
    EXPECT_EQ(scan.value.at("scanned_changes"), 1U);
    ASSERT_EQ(scan.value.at("items").size(), 1U);
    EXPECT_EQ(scan.value.at("items").front().at("change"), allowed_change);

    const auto events = lake::test::audit_events(*store, {.request_id = context.request_id});
    ASSERT_TRUE(events) << events.error().message;
    const auto scan_event = std::find_if(events->begin(), events->end(), [&](const auto& event) {
        return event.draft.request_id == context.request_id;
    });
    ASSERT_NE(scan_event, events->end());
    EXPECT_EQ(scan_event->draft.action, "search");
    EXPECT_EQ(scan_event->draft.details.value("mode", ""), "scan");
}

TEST(PostgresCapabilities, SecretsAndEncryptionRoundTrip) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }

    std::shared_ptr<lake::control::Store> store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    // Secrets live in their mount's workspace Zetta (lake#328): a Ready workspace whose record
    // store is the test's.
    lake::test::RecordWorkspaces workspaces(store, uri);
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.workspaces = workspaces.service.get();
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    auto call = [&](const std::string& name, json input = json::object()) {
        context.request_id.clear();
        const auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        return completed ? *completed : ApiResult::failure("store_error", "did not complete");
    };
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);

    const ApiResult organization =
        call("create_organization", {{"display_name", "Secrets integration " + suffix}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const ApiResult customer = call("create_customer",
                                    {{"customer_type", "organization"},
                                     {"organization_id", organization.value.at("id")},
                                     {"name", "secrets-" + suffix}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string workspace_id = workspaces.create(customer.value.at("id"), "secrets");
    const ApiResult mount = call("create_secret_mount",
                                 {{"customer_id", customer.value.at("id")},
                                  {"workspace_id", workspace_id},
                                  {"organization_id", organization.value.at("id")},
                                  {"name", "mount-" + suffix}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value.at("id");
    ASSERT_TRUE(call("get_secret_mount", {{"mount_id", mount_id}}).ok);
    const ApiResult listed = call(
        "list_secret_mounts", {{"customer_id", customer.value.at("id")}, {"workspace_id", workspace_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value.at("items").size(), 1U);
    {
        // Nothing of the mount is in PostgreSQL but its content-free route.
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_TRUE(tx.exec("SELECT to_regclass('secrets_mounts')")[0][0].is_null())
            << "migration 0070 dropped the retired table";
        EXPECT_EQ(tx.exec_params("SELECT workspace_id::text FROM secret_mount_routes "
                                 "WHERE mount_id = $1::uuid",
                                 mount_id)[0][0]
                      .as<std::string>(),
                  workspace_id);
    }

    const ApiResult first =
        call("put_secret",
             {{"mount_id", mount_id}, {"name", "database"}, {"value", {{"password", "one"}}}});
    ASSERT_TRUE(first.ok) << first.error.message;
    const ApiResult second = call("put_secret",
                                  {{"mount_id", mount_id},
                                   {"name", "database"},
                                   {"value", {{"password", "two"}}},
                                   {"check_and_set", 1}});
    ASSERT_TRUE(second.ok) << second.error.message;
    const ApiResult metadata =
        call("secret_metadata", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(metadata.ok) << metadata.error.message;
    EXPECT_EQ(metadata.value.at("versions").size(), 2);
    const ApiResult secrets = call("list_secrets", {{"mount_id", mount_id}});
    ASSERT_TRUE(secrets.ok) << secrets.error.message;
    ASSERT_EQ(secrets.value.at("items").size(), 1u);
    EXPECT_EQ(secrets.value.at("items").at(0).at("name"), "database");
    EXPECT_EQ(secrets.value.at("items").at(0).at("current_version"), 2);
    EXPECT_EQ(secrets.value.dump().find("\"two\""), std::string::npos) << "no value is secrets";
    ASSERT_TRUE(call("delete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    EXPECT_FALSE(
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}}).ok);
    ASSERT_TRUE(call("undelete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    ASSERT_TRUE(
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}}).ok);
    ASSERT_TRUE(call("destroy_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    EXPECT_FALSE(
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}}).ok);

    const ApiResult key =
        call("create_encryption_key", {{"mount_id", mount_id}, {"name", "transit"}});
    ASSERT_TRUE(key.ok) << key.error.message;
    ASSERT_TRUE(call("get_encryption_key", {{"mount_id", mount_id}, {"name", "transit"}}).ok);
    ASSERT_TRUE(call("list_encryption_keys", {{"mount_id", mount_id}}).ok);
    const std::string payload = "cG9zdGdyZXM";
    const ApiResult encrypted =
        call("encrypt", {{"mount_id", mount_id}, {"name", "transit"}, {"plaintext", payload}});
    ASSERT_TRUE(encrypted.ok) << encrypted.error.message;
    ASSERT_TRUE(call("rotate_encryption_key", {{"mount_id", mount_id}, {"name", "transit"}}).ok);
    const ApiResult decrypted = call(
        "decrypt",
        {{"mount_id", mount_id}, {"name", "transit"}, {"ciphertext", encrypted.value.at("value")}});
    ASSERT_TRUE(decrypted.ok) << decrypted.error.message;
    EXPECT_EQ(decrypted.value.at("value"), payload);
    const ApiResult rewrapped = call(
        "rewrap",
        {{"mount_id", mount_id}, {"name", "transit"}, {"ciphertext", encrypted.value.at("value")}});
    ASSERT_TRUE(rewrapped.ok) << rewrapped.error.message;
    EXPECT_EQ(rewrapped.value.at("key_version"), 2);
    const ApiResult signature =
        call("sign", {{"mount_id", mount_id}, {"name", "transit"}, {"message", payload}});
    ASSERT_TRUE(signature.ok) << signature.error.message;
    const ApiResult verified = call("verify",
                                    {{"mount_id", mount_id},
                                     {"name", "transit"},
                                     {"message", payload},
                                     {"signature", signature.value.at("value")}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_TRUE(verified.value.at("valid"));
    ASSERT_TRUE(
        call("hmac", {{"mount_id", mount_id}, {"name", "transit"}, {"message", payload}}).ok);
    ASSERT_TRUE(call("derive_encryption_key",
                     {{"mount_id", mount_id},
                      {"parent_name", "transit"},
                      {"name", "derived"},
                      {"context", "Y29udGV4dA"}})
                    .ok);

    const ApiResult kek = call("rotate_customer_kek", {{"customer_id", customer.value.at("id")}});
    ASSERT_TRUE(kek.ok) << kek.error.message;
    EXPECT_GE(kek.value.at("data_keys_rewrapped"), 4);
    const ApiResult after = call("read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(after.value.at("value").at("password"), "two");
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        const auto keystore = tx.exec_params(
            "SELECT version, previous_version IS NULL FROM keystore WHERE customer_id = $1::uuid",
            customer.value.at("id").get<std::string>());
        EXPECT_EQ(keystore[0][0].as<int>(), 2);
        EXPECT_TRUE(keystore[0][1].as<bool>()) << "the replaced KEK is forgotten once rewrapped";
        for (const std::string table : {"secrets_entries", "secrets_encryption_keys"}) {
            EXPECT_TRUE(tx.exec("SELECT to_regclass('" + table + "')")[0][0].is_null()) << table;
        }
    }
    ASSERT_TRUE(call("decrypt",
                     {{"mount_id", mount_id},
                      {"name", "transit"},
                      {"ciphertext", rewrapped.value.at("value")}})
                    .ok);

    context.operator_credential = false;
    context.acting_identity = "taap:1:ed25519:no-mount-grant-" + suffix;
    const ApiResult denied = call("read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.outcome, "denied");
    EXPECT_EQ(event->draft.details.at("mount_id"), mount_id);
}

// Migration 0058 (lake#362): secret_lease_routes, the content-free route and due index of each
// dynamic lease, and the store methods the expiry loop uses on it.
TEST(PostgresSecrets, LeaseRoutesAreAContentFreeDueIndex) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization = store->create_organization(
        {"taap:1:ed25519:lease-routes-" + suffix, "lease routes", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer = store->create_customer(
        {"organization", "", organization->id, "lease-routes-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    std::vector<std::string> workspaces;
    for (const std::string name : {"home", "lab"}) {
        const auto workspace = store->create_workspace({customer->id, name, "dev", ""});
        ASSERT_TRUE(workspace) << workspace.error().message;
        workspaces.push_back(workspace->id);
    }
    const std::string mount = lake::control::new_uuid_v4();
    ASSERT_TRUE(store->create_secret_mount_route(
        {mount, customer->id, organization->id, workspaces[0], ""}));

    pqxx::connection connection(uri);
    // Rerunning the schema from 0058 (whose legacy table 0070 dropped) leaves the routes as they
    // are.
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0058_secret_lease_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0058_secret_lease_workspaces:");
    ASSERT_NE(start, std::string::npos);
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
    }
    {
        pqxx::work tx(connection);
        EXPECT_FALSE(tx.exec("SELECT 1 FROM schema_migrations WHERE version = "
                             "'0058_secret_lease_workspaces'")
                         .empty());
        EXPECT_EQ(tx.exec("SELECT string_agg(column_name, ',' ORDER BY column_name) FROM "
                          "information_schema.columns WHERE table_name = 'secret_lease_routes' "
                          "AND table_schema = current_schema()")[0][0]
                      .as<std::string>(),
                  "created_at,customer_id,due_at,lease_id,mount_id,workspace_id")
            << "no role, value or ciphertext";
    }

    const auto now_us = [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const auto route = [&](std::int64_t due) {
        return lake::control::SecretLeaseRoute{lake::control::new_uuid_v4(), mount, customer->id,
                                               workspaces[0], due};
    };
    // A route names its mount's workspace and space, and nothing else.
    auto wrong = route(now_us());
    wrong.workspace_id = workspaces[1];
    EXPECT_EQ(store->record_secret_lease_route(wrong).error().code, "invalid_input");
    auto unrouted = route(now_us());
    unrouted.mount_id = lake::control::new_uuid_v4();
    EXPECT_EQ(store->record_secret_lease_route(unrouted).error().code, "invalid_input");

    const std::int64_t past = now_us() - 2'000'000;
    const auto early = route(past - 1'000'000), late = route(past), future = route(now_us() + 3'600'000'000);
    for (const auto& value : {late, early, future}) {
        const auto recorded = store->record_secret_lease_route(value);
        ASSERT_TRUE(recorded) << recorded.error().message;
        EXPECT_EQ(recorded->due_us, value.due_us);
    }
    auto again = early;
    again.due_us = 0;
    EXPECT_EQ(store->record_secret_lease_route(again)->due_us, early.due_us)
        << "a lease that has a route keeps it";

    // Due rows are claimed earliest first and held, so a second loop takes none of them.
    const auto claimed = store->claim_due_secret_leases(100, 60);
    ASSERT_TRUE(claimed) << claimed.error().message;
    std::vector<std::string> ours;
    for (const auto& row : *claimed) {
        if (row.mount_id == mount) {
            ours.push_back(row.lease_id);
        }
    }
    EXPECT_EQ(ours, (std::vector<std::string>{early.lease_id, late.lease_id}));
    const auto second = store->claim_due_secret_leases(100, 60);
    ASSERT_TRUE(second);
    EXPECT_TRUE(std::ranges::none_of(*second, [&](const auto& row) { return row.mount_id == mount; }));
    EXPECT_GT(store->get_secret_lease_route(early.lease_id)->due_us, now_us());

    // Settled routes are never made due again.
    ASSERT_TRUE(store->reschedule_secret_lease(early.lease_id, 0));
    EXPECT_EQ(store->get_secret_lease_route(early.lease_id)->due_us, 0);
    ASSERT_TRUE(store->reschedule_secret_lease(early.lease_id, past));
    EXPECT_EQ(store->get_secret_lease_route(early.lease_id)->due_us, 0);
    ASSERT_TRUE(store->reschedule_secret_lease(late.lease_id, past));
    EXPECT_EQ(store->get_secret_lease_route(late.lease_id)->due_us, past);
    ASSERT_TRUE(store->delete_secret_lease_route(late.lease_id));
    EXPECT_EQ(store->get_secret_lease_route(late.lease_id).error().code, "not_found");

    {
        pqxx::work tx(connection);
        // A route's workspace never moves, and a route needs its mount's route.
        EXPECT_THROW(tx.exec_params("UPDATE secret_lease_routes SET workspace_id = $2::uuid "
                                    "WHERE lease_id = $1::uuid",
                                    future.lease_id, workspaces[1]),
                     pqxx::check_violation);
    }
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("INSERT INTO secret_lease_routes (lease_id, mount_id, "
                                    "customer_id, workspace_id) VALUES ($1::uuid, $2::uuid, "
                                    "$3::uuid, $4::uuid)",
                                    lake::control::new_uuid_v4(), lake::control::new_uuid_v4(),
                                    customer->id, workspaces[0]),
                     pqxx::foreign_key_violation);
    }
}

} // namespace

TEST(PostgresUsageLimits, OneLimitPerDimensionAndZeroRemovesIt) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:lim-" + suffix, "lim-" + suffix + "@example.com", "Limits"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "lim-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    // Migration 0072: the table and its guard.
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_FALSE(tx.exec("SELECT to_regclass('customer_usage_limits')")[0][0].is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('usage_limits_guard')")[0][0].is_null());
    }
    const auto none = store->usage_limits(customer->id);
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_TRUE(none->empty());
    ASSERT_TRUE(store->set_usage_limit({customer->id, "calls", 1000, 80, {}}));
    const auto replaced = store->set_usage_limit({customer->id, "calls", 5000, 90, {}});
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_FALSE(replaced->updated_at.empty());
    ASSERT_TRUE(store->set_usage_limit({customer->id, "storage", 4096, 75, {}}));
    const auto both = store->usage_limits(customer->id);
    ASSERT_TRUE(both);
    ASSERT_EQ(both->size(), 2U);
    EXPECT_EQ((*both)[0].dimension, "calls");
    EXPECT_EQ((*both)[0].limit, 5000U);
    EXPECT_EQ((*both)[0].warn_percent, 90);
    ASSERT_TRUE(store->set_usage_limit({customer->id, "calls", 0, 80, {}}));
    EXPECT_EQ(store->usage_limits(customer->id)->size(), 1U);
    EXPECT_EQ(store->set_usage_limit({customer->id, "calls", 10, 0, {}}).error().code,
              "invalid_input");
    EXPECT_EQ(store->usage_limits(lake::control::new_uuid_v4()).error().code, "not_found");
}

// Billing records (migration 0073, lake#564): the price list (one stored row per
// publication since lake#635) and invoices with frozen lines and payment state, in the control
// plane. billing_subscriptions stays in the schema, unused since lake#635 (no plans to choose).
TEST(PostgresBilling, PriceListsAndInvoicesFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const char* table : {"billing_plans",
                                  "billing_plan_prices",
                                  "billing_subscriptions",
                                  "billing_invoices",
                                  "billing_invoice_lines",
                                  "billing_guard"}) {
            EXPECT_FALSE(
                tx.exec_params("SELECT to_regclass($1)", std::string(table))[0][0].is_null())
                << table;
        }
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0073_billing'").empty());
    }
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:bill-" + suffix, "bill-" + suffix + "@example.com", "Billing"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "bill-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    // A plan with two prices, replaced in place.
    const std::string plan_id = "team-" + suffix;
    lake::control::BillingPlan plan;
    plan.id = plan_id;
    plan.name = "Team";
    plan.currency = "USD";
    plan.base_micros = 20'000'000;
    plan.prices = {{"storage", 100'000, 1073741824ULL, 1073741824ULL},
                   {"calls", 1'000'000, 1000, 0}};
    const auto stored = store->put_billing_plan(plan);
    ASSERT_TRUE(stored) << stored.error().message;
    ASSERT_EQ(stored->prices.size(), 2U);
    EXPECT_EQ(stored->prices[0].dimension, "calls") << "prices by dimension";
    EXPECT_EQ(stored->prices[1].per, 1073741824ULL);
    plan.prices = {{"calls", 2'000'000, 1000, 500}};
    plan.active = false;
    const auto replaced = store->put_billing_plan(plan);
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_EQ(replaced->prices.size(), 1U);
    EXPECT_EQ(replaced->prices[0].included, 500U);
    EXPECT_FALSE(replaced->active);
    plan.id = "Bad Id";
    EXPECT_EQ(store->put_billing_plan(plan).error().code, "invalid_input");
    plan.id = plan_id;
    plan.prices = {{"calls", 1, 1, 0}, {"calls", 2, 1, 0}};
    EXPECT_EQ(store->put_billing_plan(plan).error().code, "invalid_input");

    // An invoice and its lines; one a month unless void.
    lake::control::Invoice invoice;
    invoice.customer_id = customer->id;
    invoice.period = "2026-09";
    invoice.plan_id = plan_id;
    invoice.plan_name = "Team";
    invoice.currency = "USD";
    invoice.total_micros = 22'500'000;
    invoice.provider = "manual";
    invoice.instructions = "Pay by transfer.";
    invoice.lines = {
        {"plan", "", "Team plan, 2026-09", "month", 1, 0, 1, 20'000'000, 1, 20'000'000},
        {"usage",
         "calls",
         "API and MCP Calls",
         "calls",
         3500,
         1000,
         2500,
         1'000'000,
         1000,
         2'500'000}};
    const auto issued = store->create_invoice(invoice);
    ASSERT_TRUE(issued) << issued.error().message;
    EXPECT_EQ(issued->state, "due");
    EXPECT_GT(issued->number, 0U);
    EXPECT_FALSE(issued->issued_at.empty());
    ASSERT_EQ(issued->lines.size(), 2U);
    EXPECT_EQ(issued->lines[1].billable, 2500U);
    EXPECT_EQ(store->create_invoice(invoice).error().code, "conflict");
    const auto listed = store->list_invoices(customer->id);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1U);
    EXPECT_EQ((*listed)[0].lines.size(), 2U);
    EXPECT_EQ(store->get_invoice(lake::control::new_uuid_v4()).error().code, "not_found");

    const auto voided = store->set_invoice_state(issued->id, "void", "", "wrong");
    ASSERT_TRUE(voided) << voided.error().message;
    EXPECT_EQ(voided->state, "void");
    EXPECT_FALSE(voided->voided_at.empty());
    EXPECT_EQ(store->set_invoice_state(issued->id, "paid", "WIRE", "").error().code, "conflict");
    const auto again = store->create_invoice(invoice);
    ASSERT_TRUE(again) << again.error().message;
    const auto paid = store->set_invoice_state(again->id, "paid", "WIRE-1", "");
    ASSERT_TRUE(paid) << paid.error().message;
    EXPECT_EQ(paid->provider_reference, "WIRE-1");
    EXPECT_FALSE(paid->paid_at.empty());
    EXPECT_EQ(store->set_invoice_state(again->id, "due", "", "").error().code, "invalid_input");
    EXPECT_EQ(store->list_invoices(customer->id)->size(), 2U);
}

TEST(PostgresBilling, PaymentProviderCustomersAndNoticesFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const char* table :
             {"billing_accounts", "billing_payment_events", "payment_provider_guard"}) {
            EXPECT_FALSE(
                tx.exec_params("SELECT to_regclass($1)", std::string(table))[0][0].is_null())
                << table;
        }
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0074_payment_provider'")
                .empty());
    }
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:pay-" + suffix, "pay-" + suffix + "@example.com", "Payments"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "pay-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    // The provider's customer id: none, then kept once.
    EXPECT_EQ(*store->billing_provider_customer(customer->id, "stripe"), "");
    EXPECT_EQ(*store->set_billing_provider_customer(customer->id, "stripe", "cus_1"), "cus_1");
    EXPECT_EQ(*store->set_billing_provider_customer(customer->id, "stripe", "cus_2"), "cus_1")
        << "the first id is kept";
    EXPECT_EQ(*store->billing_provider_customer(customer->id, "stripe"), "cus_1");
    EXPECT_EQ(store->billing_provider_customer(lake::control::new_uuid_v4(), "stripe").error().code,
              "not_found");
    // Replaced only while the row holds the id read (lake#856): a concurrent replacement gets the
    // first one's id back.
    EXPECT_EQ(*store->replace_billing_provider_customer(customer->id, "stripe", "cus_1", "cus_3"),
              "cus_3");
    EXPECT_EQ(*store->replace_billing_provider_customer(customer->id, "stripe", "cus_1", "cus_4"),
              "cus_3")
        << "the first replacement stays";
    EXPECT_EQ(*store->billing_provider_customer(customer->id, "stripe"), "cus_3");
    EXPECT_EQ(*store->replace_billing_provider_customer(customer->id, "manual", "cus_1", "cus_4"),
              "")
        << "nothing is kept to replace";
    EXPECT_EQ(
        store->replace_billing_provider_customer(customer->id, "stripe", "", "cus_4").error().code,
        "invalid_input");
    EXPECT_EQ(store
                  ->replace_billing_provider_customer(
                      lake::control::new_uuid_v4(), "stripe", "cus_3", "cus_4")
                  .error()
                  .code,
              "not_found");

    const std::string plan_id = "pay-" + suffix;
    lake::control::BillingPlan plan;
    plan.id = plan_id;
    plan.name = "Pay";
    plan.base_micros = 1'000'000;
    ASSERT_TRUE(store->put_billing_plan(plan));
    lake::control::Invoice invoice;
    invoice.customer_id = customer->id;
    invoice.period = "2026-09";
    invoice.plan_id = plan_id;
    invoice.plan_name = "Pay";
    invoice.currency = "USD";
    invoice.total_micros = 1'000'000;
    invoice.provider = "stripe";
    invoice.provider_reference = "in_" + suffix;
    invoice.payment_url = "https://invoice.stripe.com/i/" + suffix;
    invoice.provider_status = "open";
    invoice.lines = {{"plan", "", "Pay plan, 2026-09", "month", 1, 0, 1, 1'000'000, 1, 1'000'000}};
    const auto issued = store->create_invoice(invoice);
    ASSERT_TRUE(issued) << issued.error().message;
    EXPECT_EQ(issued->payment_url, invoice.payment_url);
    EXPECT_EQ(issued->provider_status, "open");

    // A send to the billing email that failed, then succeeded (lake#857): the status moves only
    // from the one the caller read.
    const auto unsent = store->set_invoice_provider_status(issued->id, "open", "send_failed");
    ASSERT_TRUE(unsent) << unsent.error().message;
    EXPECT_EQ(unsent->provider_status, "send_failed");
    EXPECT_EQ(store->set_invoice_provider_status(issued->id, "open", "open")->provider_status,
              "send_failed")
        << "no longer open: unchanged";
    EXPECT_EQ(
        store->set_invoice_provider_status(issued->id, "send_failed", "open")->provider_status,
        "open");
    EXPECT_EQ(
        store->set_invoice_provider_status(lake::control::new_uuid_v4(), "open", "send_failed")
            .error()
            .code,
        "not_found");

    const auto apply = [&](std::string id, std::string state, std::string status) {
        return store->apply_payment_event(
            {"stripe", "evt_" + id + suffix, "invoice.x", "in_" + suffix, state, status, ""});
    };
    const auto failed = apply("failed", "", "payment_failed");
    ASSERT_TRUE(failed) << failed.error().message;
    EXPECT_TRUE(failed->matched && failed->changed);
    EXPECT_EQ(failed->invoice.state, "due");
    EXPECT_EQ(failed->invoice.provider_status, "payment_failed");
    const auto paid = apply("paid", "paid", "paid");
    ASSERT_TRUE(paid) << paid.error().message;
    EXPECT_TRUE(paid->changed);
    EXPECT_EQ(paid->invoice.state, "paid");
    EXPECT_FALSE(paid->invoice.paid_at.empty());
    EXPECT_EQ(paid->invoice.provider_reference, "in_" + suffix);
    EXPECT_EQ(
        store->set_invoice_provider_status(issued->id, "paid", "send_failed")->provider_status,
        "paid")
        << "only a due invoice's status moves";
    const auto duplicate = apply("paid", "paid", "paid");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate->duplicate);
    const auto late = apply("void", "void", "void");
    ASSERT_TRUE(late);
    EXPECT_TRUE(late->matched);
    EXPECT_FALSE(late->changed) << "a paid invoice is never voided";
    const auto foreign = store->apply_payment_event(
        {"stripe", "evt_foreign" + suffix, "invoice.paid", "in_none", "paid", "paid", ""});
    ASSERT_TRUE(foreign);
    EXPECT_FALSE(foreign->matched);
    EXPECT_EQ(store->apply_payment_event({"stripe", "", "", "", "", "", ""}).error().code,
              "invalid_input");
    EXPECT_EQ(store->get_invoice(issued->id)->state, "paid");
}

TEST(PostgresServices, CreateListAndWithdrawFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:svc-" + suffix, "svc-" + suffix + "@example.com", "Services"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "svc-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    lake::control::NewManagedService workspace{customer->id, "zetta", "workspace", "taap:x"};
    workspace.is_default = true;
    const auto first = store->create_service(workspace);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->slug, lake::control::managed_service_slug("ws", customer->id));
    EXPECT_EQ(first->instances, 1);
    EXPECT_EQ(first->storage_gi, 10);
    EXPECT_EQ(first->phase, "") << "no status row yet";
    const auto again = store->create_service(workspace);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->id, first->id) << "the default is created once";

    lake::control::NewManagedService postgres{customer->id, "postgres", "crm", "taap:x"};
    postgres.instances = 2;
    postgres.storage_gi = 30;
    postgres.max_per_customer = 2;
    const auto crm = store->create_service(postgres);
    ASSERT_TRUE(crm) << crm.error().message;
    EXPECT_TRUE(crm->slug.starts_with("svc-"));
    EXPECT_EQ(crm->instances, 2);
    EXPECT_EQ(crm->storage_gi, 30);
    EXPECT_EQ(store->create_service(postgres).error().code, "conflict");
    postgres.name = "other";
    EXPECT_EQ(store->create_service(postgres).error().code, "quota_exceeded");

    const auto withdrawn = store->withdraw_service(first->id);
    ASSERT_TRUE(withdrawn) << withdrawn.error().message;
    EXPECT_EQ(withdrawn->desired, "absent");
    EXPECT_EQ(withdrawn->generation, 2);
    EXPECT_EQ(store->withdraw_service(first->id).error().code, "conflict");
    const auto second = store->create_service(workspace);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second->id, first->id);
    EXPECT_TRUE(second->slug.starts_with("svc-")) << "the ws- slug is never reused";

    const auto listed = store->list_services(customer->id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 3U)
        << "a withdrawn service stays listed until the operator deletes it";
    EXPECT_EQ(store->get_service(lake::control::new_uuid_v4()).error().code, "not_found");

    // A deleted workspace's primary Zetta leaves the list once the operator reports the workspace
    // Deleted, even though its service_status still says Ready (lake#452).
    const auto doomed = store->create_workspace({customer->id, "doomed", "prod", ""});
    ASSERT_TRUE(doomed) << doomed.error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO service_status (service_id, phase, message) "
                       "VALUES ($1::uuid, 'Ready', 'service ready')",
                       doomed->zetta_service_id);
        tx.commit();
    }
    ASSERT_TRUE(store->update_workspace(doomed->id, {.desired = "deleted"}));
    const auto listed_with = [&](const std::string& id) {
        const auto rows = store->list_services(customer->id);
        return std::ranges::any_of(*rows, [&](const auto& row) { return row.id == id; });
    };
    EXPECT_TRUE(listed_with(doomed->zetta_service_id)) << "listed while the workspace is deleting";
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspaces SET phase = 'Deleted', observed_generation = generation "
                       "WHERE id = $1::uuid",
                       doomed->id);
        tx.commit();
    }
    EXPECT_FALSE(listed_with(doomed->zetta_service_id));
    EXPECT_TRUE(listed_with(crm->id)) << "other services are unaffected";
}

// lake#1301 (orama-operator#781): migration 0111 refuses a service whose workspace belongs to
// another customer, however the row is written, and a workspace whose Zetta service or customer
// would disagree with its services. Status writes and withdrawals are never checked.
TEST(PostgresServices, AServiceIsNeverRoutedToAnotherCustomersWorkspace) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<lake::control::Workspace> workspaces;
    for (const std::string who : {"mine", "theirs"}) {
        const auto user = store->create_user({"taap:1:ed25519:route-" + who + "-" + suffix,
                                              "route-" + who + "-" + suffix + "@example.com",
                                              "Routing"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer =
            store->create_customer({"individual", user->id, "", "route-" + who + "-" + suffix});
        ASSERT_TRUE(customer) << customer.error().message;
        const auto workspace = store->create_workspace({customer->id, "home", "dev", ""});
        ASSERT_TRUE(workspace) << workspace.error().message;
        customers.push_back(customer->id);
        workspaces.push_back(*workspace);
    }
    const std::string& mine = customers[0];
    const std::string& theirs_workspace = workspaces[1].id;

    // The store's own insert: refused by the trigger, behind lake's own caller check.
    lake::control::NewManagedService crossed{mine, "postgres", "crossed", "taap:x"};
    crossed.workspace_id = theirs_workspace;
    const auto refused = store->create_service(crossed);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "invalid_input") << refused.error().message;

    lake::control::NewManagedService own{mine, "postgres", "crm", "taap:x"};
    own.workspace_id = workspaces[0].id;
    const auto crm = store->create_service(own);
    ASSERT_TRUE(crm) << crm.error().message;
    const auto unassigned = store->create_service({mine, "postgres", "legacy", "taap:x"});
    ASSERT_TRUE(unassigned) << "a service may still name no workspace: "
                            << unassigned.error().message;

    pqxx::connection connection(uri);
    const auto refuses = [&](const std::string& sql, const std::string& id,
                             const std::string& value) {
        EXPECT_THROW(([&] {
                         pqxx::work tx(connection);
                         tx.exec_params(sql, id, value);
                         tx.commit();
                     })(),
                     pqxx::check_violation)
            << sql;
    };
    // Raw writes are refused too: moving a service to another customer's workspace, or the
    // service to another customer while it names this one's workspace.
    refuses("UPDATE services SET workspace_id = $2::uuid WHERE id = $1::uuid", crm->id,
            theirs_workspace);
    refuses("UPDATE services SET customer_id = $2::uuid WHERE id = $1::uuid", crm->id,
            customers[1]);
    refuses("UPDATE services SET workspace_id = $2::uuid WHERE id = $1::uuid", unassigned->id,
            theirs_workspace);
    // From the workspace's side: its Zetta service must be its customer's, and it cannot move to
    // another customer while services of this one name it.
    refuses("UPDATE workspaces SET zetta_service_id = $2::uuid WHERE id = $1::uuid",
            workspaces[0].id, workspaces[1].zetta_service_id);
    refuses("UPDATE workspaces SET customer_id = $2::uuid WHERE id = $1::uuid", workspaces[0].id,
            customers[1]);
    {
        // Naming a workspace of the same customer, or none, is allowed (rolled back here).
        pqxx::work tx(connection);
        tx.exec_params("UPDATE services SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       unassigned->id, workspaces[0].id);
        tx.exec_params("UPDATE services SET workspace_id = NULL WHERE id = $1::uuid", crm->id);
    }

    // Status writes (the operator's service_status row) and withdrawals touch neither routing
    // column.
    {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO service_status (service_id, phase, message) "
                       "VALUES ($1::uuid, 'Ready', 'service ready')",
                       crm->id);
        tx.exec_params("UPDATE services SET updated_at = now() WHERE id = $1::uuid", crm->id);
        tx.commit();
    }
    const auto withdrawn = store->withdraw_service(crm->id);
    ASSERT_TRUE(withdrawn) << withdrawn.error().message;
    EXPECT_EQ(store->get_service(crm->id)->workspace_id, workspaces[0].id);

    {
        pqxx::work tx(connection);
        EXPECT_FALSE(tx.exec("SELECT 1 FROM schema_migrations "
                             "WHERE version = '0111_service_workspace_routing'")
                         .empty());
        EXPECT_FALSE(
            tx.exec("SELECT to_regclass('service_workspace_routing_guard')")[0][0].is_null());
        EXPECT_EQ(tx.exec("SELECT count(*) FROM pg_trigger WHERE NOT tgisinternal AND tgname IN "
                          "('service_workspace_routing', 'workspace_service_routing')")[0][0]
                      .as<int>(),
                  2);
    }

    // Applying the migration again, unrecorded and then recorded, keeps the rows and the checks.
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0111_service_workspace_routing");
    ASSERT_NE(start, std::string::npos);
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        if (pass == 0) {
            tx.exec("DELETE FROM schema_migrations WHERE version = '0111_service_workspace_routing'");
        }
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
    }
    EXPECT_EQ(store->get_service(crm->id)->workspace_id, workspaces[0].id);
    refuses("UPDATE services SET workspace_id = $2::uuid WHERE id = $1::uuid", unassigned->id,
            theirs_workspace);
}

// lake#1228: restart_app writes restart_generation, the column the operator renders on the pod
// template; any other change leaves it.
// lake#987 (LSR-120), against either store: an account at LAKE_WORKSPACES_PER_CUSTOMER live
// workspaces, or at LAKE_SERVICES_PER_CUSTOMER live services (a workspace's own Zetta is one), is
// refused its next workspace with quota_exceeded; a deleted workspace stops counting, and another
// account is unaffected.
static void expect_workspace_limits(lake::control::Store& store, const std::string& suffix) {
    const auto first_user = store.create_user(
        {"taap:1:ed25519:wsl-a-" + suffix, "wsl-a-" + suffix + "@example.com", "Limits A"});
    const auto other_user = store.create_user(
        {"taap:1:ed25519:wsl-b-" + suffix, "wsl-b-" + suffix + "@example.com", "Limits B"});
    ASSERT_TRUE(first_user) << first_user.error().message;
    ASSERT_TRUE(other_user) << other_user.error().message;
    const auto account =
        store.create_customer({"individual", first_user->id, "", "wsl-a-" + suffix});
    const auto other = store.create_customer({"individual", other_user->id, "", "wsl-b-" + suffix});
    ASSERT_TRUE(account) << account.error().message;
    ASSERT_TRUE(other) << other.error().message;

    const auto limited = [](const std::string& customer_id, const std::string& name) {
        lake::control::NewWorkspace input{customer_id, name, "prod", "taap:x"};
        input.max_workspaces = 2;
        return input;
    };
    const auto one = store.create_workspace(limited(account->id, "one"));
    ASSERT_TRUE(one) << one.error().message;
    const auto two = store.create_workspace(limited(account->id, "two"));
    ASSERT_TRUE(two) << two.error().message;
    const auto three = store.create_workspace(limited(account->id, "three"));
    ASSERT_FALSE(three);
    EXPECT_EQ(three.error().code, "quota_exceeded");
    const auto listed = store.list_workspaces(account->id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 2U) << "nothing is written when the limit refuses";

    // Another account still creates one.
    const auto elsewhere = store.create_workspace(limited(other->id, "one"));
    EXPECT_TRUE(elsewhere) << elsewhere.error().message;

    // No limit (0) is the default.
    EXPECT_TRUE(store.create_workspace({account->id, "unlimited", "prod", "taap:x"}));

    // A deleted workspace no longer counts.
    lake::control::WorkspacePatch deleted;
    deleted.desired = "deleted";
    ASSERT_TRUE(store.update_workspace(one->id, deleted));
    ASSERT_TRUE(store.update_workspace(two->id, deleted));
    EXPECT_TRUE(store.create_workspace(limited(account->id, "after")));

    // The workspace's own Zetta counts against the service limit.
    lake::control::NewWorkspace by_services{other->id, "two", "prod", "taap:x"};
    by_services.max_services = 1;
    const auto refused = store.create_workspace(by_services);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "quota_exceeded");
    by_services.max_services = 2;
    EXPECT_TRUE(store.create_workspace(by_services));
}

TEST(WorkspaceLimits, MemoryStoreRefusesTheNextWorkspaceOnly) {
    lake::control::InMemoryStore store;
    expect_workspace_limits(store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(WorkspaceLimits, PostgresStoreRefusesTheNextWorkspaceOnly) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    expect_workspace_limits(*store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(PostgresWebApps, ARestartRecordsTheGenerationTheOperatorRollsThePodsOn) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:restart-" + suffix, "restart-" + suffix + "@example.com", "Apps"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "rs-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "apps", "dev", ""});
    ASSERT_TRUE(workspace);
    lake::control::NewWebApp app;
    app.customer_id = customer->id;
    app.workspace_id = workspace->id;
    app.name = "insights";
    app.mode = "image";
    app.image = "registry.example/insights@sha256:" + std::string(64, 'a');
    app.framework = "container";
    app.port = 8080;
    app.host = "insights-" + suffix + ".apps.example.test";
    const auto created = store->create_web_app(app);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->restart_generation, 0);

    lake::control::WebAppPatch restart;
    restart.desired = "running";
    restart.restart = true;
    const auto restarted = store->update_web_app(created->id, restart);
    ASSERT_TRUE(restarted) << restarted.error().message;
    EXPECT_EQ(restarted->generation, created->generation + 1);
    EXPECT_EQ(restarted->restart_generation, restarted->generation);

    lake::control::WebAppPatch scale;
    scale.replicas = 2;
    const auto scaled = store->update_web_app(created->id, scale);
    ASSERT_TRUE(scaled) << scaled.error().message;
    EXPECT_EQ(scaled->generation, restarted->generation + 1);
    EXPECT_EQ(scaled->restart_generation, restarted->generation);

    pqxx::connection connection(uri);
    pqxx::work tx(connection);
    EXPECT_EQ(
        tx.exec_params("SELECT restart_generation FROM apps WHERE id = $1::uuid", created->id)[0][0]
            .as<std::int64_t>(),
        restarted->generation);
    EXPECT_FALSE(tx.exec("SELECT to_regclass('app_restarts_guard')")[0][0].is_null());
}

// lake#1070: rotation is desired state. Lake asks for the next token generation, a hash verifies
// only while the operator's acknowledged generation matches, and Lake writes token_hashes_json only
// for an operator that has never reported a generation.
TEST(PostgresWebApps, TokenRotationIsAGenerationTheOperatorAcknowledges) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:rotate-" + suffix, "rotate-" + suffix + "@example.com", "Apps"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "rt-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "apps", "dev", ""});
    ASSERT_TRUE(workspace);
    lake::control::NewWebApp app;
    app.customer_id = customer->id;
    app.workspace_id = workspace->id;
    app.name = "rotating";
    app.mode = "image";
    app.image = "registry.example/rotating@sha256:" + std::string(64, 'b');
    app.framework = "container";
    app.port = 8080;
    app.host = "rotating-" + suffix + ".apps.example.test";
    const auto created = store->create_web_app(app);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->token_generation, 0);
    EXPECT_FALSE(created->token_hashes_generation);

    // An events token: an image app has no source token (a source token needs a runtime app's
    // repository and revision), and the events purpose verifies for any app's mode.
    const std::string token = "rotate-events-" + suffix;
    const json hashes{{"events", lake::control::identity_crypto::sha256_hex(token)}};
    // What the operator writes: the hashes, and from an upgraded operator, their generation.
    const auto operator_writes = [&](const json& values, std::optional<std::int64_t> generation) {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        if (generation) {
            tx.exec_params("UPDATE apps SET token_hashes_json = $2::jsonb, "
                           "token_hashes_generation = $3 WHERE id = $1::uuid",
                           created->id,
                           values.dump(),
                           *generation);
        } else {
            tx.exec_params("UPDATE apps SET token_hashes_json = $2::jsonb WHERE id = $1::uuid",
                           created->id,
                           values.dump());
        }
        tx.commit();
    };
    lake::control::WebAppPatch rotate;
    rotate.rotate_tokens = true;

    // An operator that reports no generation: Lake clears the hashes, as before.
    operator_writes(hashes, std::nullopt);
    ASSERT_TRUE(store->context_for(token));
    const auto legacy = store->update_web_app(created->id, rotate);
    ASSERT_TRUE(legacy) << legacy.error().message;
    EXPECT_EQ(legacy->token_generation, 1);
    EXPECT_TRUE(legacy->token_hashes.empty());
    EXPECT_FALSE(store->context_for(token));

    // An operator that acknowledges: Lake leaves the hashes alone and the mismatch revokes them.
    operator_writes(hashes, 1);
    ASSERT_TRUE(store->context_for(token));
    const auto acknowledged = store->update_web_app(created->id, rotate);
    ASSERT_TRUE(acknowledged) << acknowledged.error().message;
    EXPECT_EQ(acknowledged->token_generation, 2);
    EXPECT_EQ(acknowledged->token_hashes, hashes);
    EXPECT_EQ(acknowledged->token_hashes_generation, 1);
    EXPECT_FALSE(store->context_for(token));

    // The operator mints the next generation; only its tokens verify.
    const std::string replacement = "rotate-replacement-" + suffix;
    operator_writes({{"events", lake::control::identity_crypto::sha256_hex(replacement)}}, 2);
    EXPECT_FALSE(store->context_for(token));
    EXPECT_TRUE(store->context_for(replacement));

    pqxx::connection connection(uri);
    pqxx::work tx(connection);
    EXPECT_FALSE(tx.exec("SELECT to_regclass('app_token_generation_guard')")[0][0].is_null());
}

TEST(PostgresWebApps, DesiredStateQuotaNotifyAndOperatorStatusFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:app-" + suffix, "app-" + suffix + "@example.com", "Apps"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "app-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_apps",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    lake::control::NewWebApp app;
    app.customer_id = customer->id;
    const auto app_workspace = store->create_workspace({customer->id, "apps", "dev", ""});
    ASSERT_TRUE(app_workspace);
    app.workspace_id = app_workspace->id;
    app.name = "reports";
    app.mode = "runtime";
    const auto repository = store->create_repository({customer->id, "source", "", "private",
                                                      lake::test::repository_workspace(
                                                          *store, customer->id)});
    ASSERT_TRUE(repository);
    app.source_repository = repository->id;
    app.source_revision = "main";
    app.source_path = "web";
    app.source_entrypoint = "main:app";
    app.framework = "node";
    app.port = 3000;
    app.replicas = 2;
    app.host = "reports-" + suffix + ".apps.example.test";
    app.bindings = {{"zetta", {{"scopes", json::array({"read"})}}}, {"services", json::array()}};
    app.env = {{"LOG_LEVEL", "info"}};
    app.max_apps = 1;
    app.max_replicas = 3;
    const auto created = store->create_web_app(app);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->generation, 1);
    EXPECT_EQ(created->observed_generation, 0);
    EXPECT_FALSE(created->ready);
    EXPECT_EQ(created->source_entrypoint, "main:app");
    EXPECT_TRUE(created->token_hashes.empty());
    EXPECT_FALSE(store->context_for("operator-source"));
    const json hashes{{"source", lake::control::identity_crypto::sha256_hex("operator-source")},
                      {"zetta", lake::control::identity_crypto::sha256_hex("operator-zetta")},
                      {"events", lake::control::identity_crypto::sha256_hex("operator-events")},
                      {"pull", lake::control::identity_crypto::sha256_hex("operator-pull")}};
    const auto write_hashes = [&](const json& values) {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE apps SET token_hashes_json = $2::jsonb WHERE id = $1::uuid",
                       created->id,
                       values.dump());
        tx.commit();
    };
    write_hashes(hashes);
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](const lake::control::Principal& principal,
                          const std::string& name,
                          json input) {
        ApiContext context;
        context.store = store.get();
        context.authorized = true;
        context.session_credential = true;
        context.acting_identity = principal.taap_id;
        context.credential_kind = principal.kind;
        context.credential_workload_id = principal.workload_id;
        context.credential_customer_id = principal.workload_customer_id;
        context.credential_repository_id = principal.workload_repository_id;
        context.credential_revision = principal.workload_revision;
        context.credential_token_hash = principal.workload_token_hash;
        context.scopes = principal.scopes;
        context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        if (input.is_object() && registry.find(name)->input_schema["properties"].contains("workspace_id")) {
            input["workspace_id"] = app.workspace_id;
        }
        auto result = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(result);
        return *result;
    };
    for (const std::string purpose : {"source", "zetta", "events", "pull"}) {
        const auto principal = store->context_for("operator-" + purpose);
        ASSERT_TRUE(principal) << principal.error().message;
        EXPECT_EQ(principal->kind, "app_" + purpose);
        EXPECT_EQ(principal->workload_id, created->id);
        EXPECT_EQ(principal->workload_customer_id, customer->id);
        EXPECT_EQ(call(*principal, "server_info", json::object()).error.code, "unauthorized");
        EXPECT_EQ(call(*principal, "server_info", nullptr).error.code, "invalid_input");
        EXPECT_EQ(
            call(*principal,
                 "publish_app_event",
                 {{"app_id", lake::control::new_uuid_v4()}, {"event", {{"phase", "Running"}}}})
                .error.code,
            "unauthorized");
        EXPECT_EQ(
            call(*principal,
                 "workspace_query",
                 {{"customer_id", lake::control::new_uuid_v4()}, {"capability", "list_datasets"}})
                .error.code,
            "unauthorized");
        EXPECT_EQ(call(*principal,
                       "repository_tree",
                       {{"id", lake::control::new_uuid_v4()}, {"ref", "main"}})
                      .error.code,
                  "unauthorized");
    }
    const auto source = *store->context_for("operator-source");
    EXPECT_EQ(
        call(source, "repository_tree", {{"id", repository->id}, {"ref", "other"}}).error.code,
        "unauthorized");
    EXPECT_EQ(call(source,
                   "repository_file",
                   {{"id", repository->id}, {"ref", "other"}, {"path", "app.toml"}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(call(source, "get_app", {{"customer_id", customer->id}, {"app_id", created->id}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(call(source, "repository_tree", {{"id", repository->id}, {"ref", "main"}}).error.code,
              "store_unavailable")
        << "authorized source reads reach the repository content store";
    EXPECT_EQ(call(source,
                   "repository_file",
                   {{"id", repository->id}, {"ref", "main"}, {"path", "app.toml"}})
                  .error.code,
              "store_unavailable");
    const auto zetta = *store->context_for("operator-zetta");
    EXPECT_EQ(call(zetta,
                   "workspace_query",
                   {{"customer_id", customer->id}, {"capability", "list_datasets"}})
                  .error.code,
              "store_unavailable");
    EXPECT_EQ(call(zetta,
                   "workspace_sql",
                   {{"customer_id", customer->id}, {"statement", "SELECT * FROM data.rows"}})
                  .error.code,
              "store_unavailable");
    EXPECT_EQ(call(zetta,
                   "workspace_act",
                   {{"customer_id", customer->id}, {"capability", "create_project"}})
                  .error.code,
              "unauthorized");
    const auto events = *store->context_for("operator-events");
    EXPECT_EQ(call(events,
                   "publish_app_event",
                   {{"app_id", created->id}, {"event", {{"phase", "Running"}}}})
                  .error.code,
              "store_unavailable");
    {
        pqxx::connection connection(uri);
        pqxx::read_transaction tx(connection);
        EXPECT_TRUE(tx.exec1("SELECT to_regclass('app_credentials')")[0].is_null());
        const auto row = tx.exec_params1(
            "SELECT token_hashes_json::text FROM apps WHERE id = $1::uuid", created->id);
        EXPECT_EQ(json::parse(row[0].as<std::string>()), hashes);
        EXPECT_FALSE(row[0].as<std::string>().contains("operator-"));
    }
    EXPECT_EQ(store->create_web_app(app).error().code, "conflict");
    app.name = "second";
    app.host = "second-" + suffix + ".apps.example.test";
    EXPECT_EQ(store->create_web_app(app).error().code, "quota_exceeded");

    lake::control::WebAppPatch scale;
    scale.replicas = 3;
    scale.max_replicas = 3;
    const auto scaled = store->update_web_app(created->id, scale);
    ASSERT_TRUE(scaled) << scaled.error().message;
    EXPECT_EQ(scaled->replicas, 3);
    EXPECT_EQ(scaled->generation, 2);
    scale.replicas = 4;
    EXPECT_EQ(store->update_web_app(created->id, scale).error().code, "quota_exceeded");

    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE apps SET phase = 'Running', url = $2, ready = true, "
                       "message = 'available', observed_generation = generation, "
                       "updated_at = now() WHERE id = $1::uuid",
                       created->id,
                       "https://" + created->host);
        tx.commit();
    }
    const auto observed = store->get_web_app(created->id);
    ASSERT_TRUE(observed) << observed.error().message;
    EXPECT_EQ(observed->phase, "Running");
    EXPECT_TRUE(observed->ready);
    EXPECT_EQ(observed->url, "https://" + created->host);
    EXPECT_EQ(observed->observed_generation, 2);
    EXPECT_EQ(observed->token_hashes, hashes)
        << "desired and status edits preserve operator hashes";

    for (int waited = 0; notified.size() < 2 && waited < 50; ++waited) {
        listener.await_notification(0, 100000);
    }
    ASSERT_GE(notified.size(), 2U);
    EXPECT_EQ(notified.front(), created->id);
    EXPECT_EQ(notified.back(), created->id);

    auto rotated = hashes;
    rotated["source"] = lake::control::identity_crypto::sha256_hex("replacement-source");
    write_hashes(rotated);
    EXPECT_FALSE(store->context_for("operator-source"));
    EXPECT_TRUE(store->context_for("replacement-source"));
    EXPECT_EQ(call(source, "repository_tree", {{"id", repository->id}, {"ref", "main"}}).error.code,
              "unauthorized");
    lake::control::WebAppPatch rotation;
    rotation.rotate_tokens = true;
    ASSERT_TRUE(store->update_web_app(created->id, rotation));
    EXPECT_TRUE(store->get_web_app(created->id)->token_hashes.empty());
    EXPECT_FALSE(store->context_for("operator-events"));
    write_hashes(hashes);
    lake::control::WebAppPatch stopped;
    stopped.desired = "stopped";
    const auto stop = store->update_web_app(created->id, stopped);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->replicas, 3);
    ASSERT_TRUE(store->purge_deleted_web_apps());
    EXPECT_TRUE(store->get_web_app(created->id));
    lake::control::WebAppPatch deleted;
    deleted.desired = "deleted";
    ASSERT_TRUE(store->update_web_app(created->id, deleted));
    EXPECT_FALSE(store->context_for("operator-source"));
    EXPECT_FALSE(store->context_for("operator-zetta"));
    EXPECT_TRUE(store->context_for("operator-events"));
    ASSERT_TRUE(store->purge_deleted_web_apps());
    EXPECT_TRUE(store->get_web_app(created->id));
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE apps SET phase = 'Deleted' WHERE id = $1::uuid", created->id);
        tx.commit();
    }
    EXPECT_FALSE(store->context_for("operator-events"));
    ASSERT_TRUE(store->purge_deleted_web_apps());
    EXPECT_FALSE(store->get_web_app(created->id));
}

TEST(PostgresBuilds, DesiredRowStatusTokensAndCancellationFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:build-" + suffix, "build-" + suffix + "@example.com", "Builds"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "build-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = store->create_repository({customer->id, "source", "", "private",
                                                      lake::test::repository_workspace(
                                                          *store, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;

    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_builds",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    lake::control::NewBuild requested;
    requested.customer_id = customer->id;
    const auto build_workspace = store->create_workspace({customer->id, "builds", "dev", ""});
    ASSERT_TRUE(build_workspace);
    requested.workspace_id = build_workspace->id;
    requested.repository = repository->id;
    requested.revision = "main";
    requested.context = "services/api";
    requested.dockerfile = "services/api/Dockerfile";
    requested.target = "lake-prod.prod.svc:80/" + customer->name + "/service:v1";
    requested.build_args = {{"PYTHON_VERSION", "3.13"}};
    const auto created = store->create_build(requested);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->desired, "queued");
    EXPECT_EQ(created->phase, "Queued");
    EXPECT_TRUE(created->token_hashes.empty());
    EXPECT_EQ(created->build_args, requested.build_args);

    const json hashes{{"source", lake::control::identity_crypto::sha256_hex("pg-build-source")},
                      {"push", lake::control::identity_crypto::sha256_hex("pg-build-push")},
                      {"events", lake::control::identity_crypto::sha256_hex("pg-build-events")}};
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE builds SET phase = 'Running', message = 'builder running', "
                       "token_hashes_json = $2::jsonb, updated_at = now() WHERE id = $1::uuid",
                       created->id,
                       hashes.dump());
        tx.commit();
    }
    const auto running = store->get_build(created->id);
    ASSERT_TRUE(running) << running.error().message;
    EXPECT_EQ(running->phase, "Running");
    EXPECT_EQ(running->token_hashes, hashes);
    const auto source = store->context_for("pg-build-source");
    const auto push = store->context_for("pg-build-push");
    const auto events = store->context_for("pg-build-events");
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(push) << push.error().message;
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_EQ(source->kind, "build_source");
    EXPECT_EQ(push->kind, "build_push");
    EXPECT_EQ(events->kind, "build_events");
    EXPECT_EQ(source->workload_repository_id, repository->id);
    EXPECT_EQ(source->workload_revision, "main");

    const auto listed = store->list_builds(customer->id);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed->size(), 1U);
    EXPECT_EQ(listed->front().id, created->id);
    const auto cancelled = store->cancel_build(created->id);
    ASSERT_TRUE(cancelled) << cancelled.error().message;
    EXPECT_EQ(cancelled->desired, "cancelled");
    EXPECT_EQ(store->cancel_build(created->id).error().code, "conflict");
    EXPECT_FALSE(store->context_for("pg-build-source"));
    EXPECT_FALSE(store->context_for("pg-build-push"));
    EXPECT_FALSE(store->context_for("pg-build-events"));

    for (int waited = 0; notified.size() < 2 && waited < 50; ++waited) {
        listener.await_notification(0, 100000);
    }
    ASSERT_GE(notified.size(), 2U);
    EXPECT_EQ(notified.front(), created->id);
    EXPECT_EQ(notified.back(), created->id);
}

// lake#666: store_mounts, written with the workload in one transaction, checked under the store's
// lock, read back with the names, refused for a second workload on a volume and for a deleted
// store, and removed with the workload.
TEST(PostgresStores, MountsFollowTheirWorkloadAndKeepAVolumeToOne) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:mt-" + suffix, "mt-" + suffix + "@example.com", "Mounts"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "mt-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "mounts", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto other_workspace = store->create_workspace({customer->id, "other", "dev", ""});
    ASSERT_TRUE(other_workspace) << other_workspace.error().message;

    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_services",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    const auto make =
        [&](std::string type, std::string provider, std::string name, std::string workspace_id) {
            lake::control::NewManagedStore value{customer->id, type, provider, name, "taap:x"};
            value.workspace_id = workspace_id;
            return store->create_store(value);
        };
    const auto volume = make("volume", "cluster", "uploads", workspace->id);
    ASSERT_TRUE(volume) << volume.error().message;
    const auto bucket = make("object", "s3", "media", workspace->id);
    ASSERT_TRUE(bucket) << bucket.error().message;
    const auto foreign = make("volume", "cluster", "foreign", other_workspace->id);
    ASSERT_TRUE(foreign) << foreign.error().message;

    lake::control::NewManagedService sqlite;
    sqlite.customer_id = customer->id;
    sqlite.workspace_id = workspace->id;
    sqlite.kind = "sqlite";
    sqlite.name = "db";
    sqlite.created_by_taap_id = "taap:x";
    sqlite.stores = {{foreign->id, "/srv/uploads", false}};
    EXPECT_EQ(store->create_service(sqlite).error().code, "invalid_input")
        << "a store of another workspace";
    EXPECT_TRUE(store->list_services(customer->id)->size() == 2U)
        << "nothing written: the two workspace Zettas only";
    sqlite.stores = {{volume->id, "/srv/uploads", false}, {bucket->id, "", true}};
    const auto db = store->create_service(sqlite);
    ASSERT_TRUE(db) << db.error().message;

    auto mounts = store->list_store_mounts(customer->id, workspace->id);
    ASSERT_TRUE(mounts) << mounts.error().message;
    ASSERT_EQ(mounts->size(), 2U);
    EXPECT_EQ(mounts->at(0).store_name, "media") << "by store name";
    EXPECT_EQ(mounts->at(0).mount_path, "");
    EXPECT_TRUE(mounts->at(0).read_only);
    EXPECT_EQ(mounts->at(1).store_name, "uploads");
    EXPECT_EQ(mounts->at(1).object_kind, "service");
    EXPECT_EQ(mounts->at(1).object_id, db->id);
    EXPECT_EQ(mounts->at(1).workload_name, "db");
    EXPECT_EQ(mounts->at(1).store_type, "volume");
    EXPECT_TRUE(store->list_store_mounts(customer->id, other_workspace->id)->empty());

    // A second workload on the volume is refused, naming the first; a bucket takes any number.
    const auto app = store->create_web_app({.customer_id = customer->id,
                                            .workspace_id = workspace->id,
                                            .name = "site",
                                            .mode = "image",
                                            .image = "site:v1",
                                            .port = 3000,
                                            .host = "site-" + suffix + ".test",
                                            .stores = {{volume->id, "/srv/uploads", false}}});
    ASSERT_FALSE(app);
    EXPECT_EQ(app.error().code, "conflict");
    EXPECT_NE(app.error().message.find("service db"), std::string::npos) << app.error().message;
    const auto site = store->create_web_app({.customer_id = customer->id,
                                             .workspace_id = workspace->id,
                                             .name = "site",
                                             .mode = "image",
                                             .image = "site:v1",
                                             .port = 3000,
                                             .host = "site-" + suffix + ".test",
                                             .stores = {{bucket->id, "", false}}});
    ASSERT_TRUE(site) << site.error().message;
    lake::control::WebAppPatch steal;
    steal.stores = std::vector<lake::control::StoreMountRequest>{{volume->id, "/v", false}};
    EXPECT_EQ(store->update_web_app(site->id, steal).error().code, "conflict");
    EXPECT_EQ(store->get_web_app(site->id)->generation, site->generation) << "nothing changed";

    // list_stores and get_store read who mounts each; the operator's used bytes come back.
    auto listed = store->list_stores(customer->id, workspace->id);
    ASSERT_TRUE(listed) << listed.error().message;
    const auto media = std::ranges::find(*listed, bucket->id, &lake::control::ManagedStore::id);
    ASSERT_NE(media, listed->end());
    ASSERT_EQ(media->mounted_by.size(), 2U);
    EXPECT_EQ(media->mounted_by[0].object_kind, "app");
    EXPECT_EQ(media->mounted_by[1].object_kind, "service");
    EXPECT_FALSE(media->used_bytes.has_value());
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO store_status (store_id, phase, used_bytes, used_bytes_at) "
                       "VALUES ($1::uuid, 'Ready', 4096, now())",
                       volume->id);
        tx.commit();
    }
    const auto read = store->get_store(volume->id);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(read->used_bytes, 4096);
    EXPECT_FALSE(read->used_bytes_at.empty());
    ASSERT_EQ(read->mounted_by.size(), 1U);
    EXPECT_EQ(read->mounted_by[0].workload_name, "db");

    // A mounted store is not withdrawn.
    const auto refused = store->withdraw_store(volume->id);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "conflict");
    EXPECT_NE(refused.error().message.find("service db"), std::string::npos)
        << refused.error().message;

    // set_service_stores replaces, bumps the generation and notifies.
    // What earlier writes notified is delivered first; then the service's own notification.
    while (listener.await_notification(0, 200000) != 0) {
    }
    notified.clear();
    const auto replaced = store->set_service_stores(db->id, {{bucket->id, "", false}});
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_EQ(replaced->generation, db->generation + 1);
    for (int waited = 0; notified.empty() && waited < 50; ++waited) {
        listener.await_notification(0, 100000);
    }
    EXPECT_EQ(notified, (std::vector<std::string>{db->id}));
    EXPECT_TRUE(store->get_store(volume->id)->mounted_by.empty());
    const auto moved = store->update_web_app(site->id, steal);
    ASSERT_TRUE(moved) << "the volume is free: " << moved.error().message;
    EXPECT_EQ(store->get_store(volume->id)->mounted_by.at(0).workload_name, "site");

    // Deleting the workload removes its mounts.
    ASSERT_TRUE(store->withdraw_service(db->id));
    lake::control::WebAppPatch remove;
    remove.desired = "deleted";
    ASSERT_TRUE(store->update_web_app(site->id, remove));
    EXPECT_TRUE(store->list_store_mounts(customer->id)->empty());
    const auto withdrawn = store->withdraw_store(volume->id);
    ASSERT_TRUE(withdrawn) << withdrawn.error().message;
    sqlite.name = "late";
    sqlite.stores = {{volume->id, "/srv/uploads", false}};
    EXPECT_EQ(store->create_service(sqlite).error().code, "invalid_input")
        << "a store being deleted is not mounted";
}

TEST(PostgresStores, CreateListWithdrawAndNotifyFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:st-" + suffix, "st-" + suffix + "@example.com", "Stores"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "st-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "storage", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;

    // The operator's listener: every insert and withdrawal wakes it on the services channel.
    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_services",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    lake::control::NewManagedStore volume{customer->id, "volume", "cluster", "uploads", "taap:x"};
    volume.size_gi = 20;
    // lake#465: a store names its workspace; the routing trigger refuses one that does not.
    EXPECT_FALSE(store->create_store(volume));
    volume.workspace_id = workspace->id;
    const auto first = store->create_store(volume);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->workspace_id, workspace->id);
    EXPECT_TRUE(store->list_stores(customer->id, lake::control::new_uuid_v4())->empty())
        << "another workspace lists none of this one's stores";
    EXPECT_EQ(store->list_stores(customer->id, workspace->id)->size(), 1U);
    EXPECT_EQ(first->slug, lake::control::managed_service_slug("st", first->id));
    EXPECT_EQ(first->type, "volume");
    EXPECT_EQ(first->provider, "cluster");
    EXPECT_EQ(first->size_gi, 20);
    EXPECT_FALSE(first->is_public);
    EXPECT_EQ(first->desired, "present");
    EXPECT_EQ(first->generation, 1);
    EXPECT_EQ(first->phase, "") << "no status row yet";
    EXPECT_EQ(store->create_store(volume).error().code, "conflict");
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        const auto spec =
            tx.exec_params1("SELECT spec::text FROM stores WHERE id = $1::uuid", first->id)[0]
                .as<std::string>();
        const json parsed = json::parse(spec);
        EXPECT_EQ(parsed["size_gi"], 20);
        EXPECT_EQ(parsed["public"], false);
        EXPECT_TRUE(parsed.contains("region"));
    }

    const auto withdrawn = store->withdraw_store(first->id);
    ASSERT_TRUE(withdrawn) << withdrawn.error().message;
    EXPECT_EQ(withdrawn->desired, "absent");
    EXPECT_EQ(withdrawn->generation, 2);
    EXPECT_EQ(store->withdraw_store(first->id).error().code, "conflict");
    const auto second = store->create_store(volume);
    ASSERT_TRUE(second) << "a withdrawn store frees its name: " << second.error().message;
    EXPECT_NE(second->slug, first->slug) << "a slug is never reused";

    for (int waited = 0; notified.size() < 3 && waited < 50; ++waited) {
        listener.await_notification(0, 100000);
    }
    EXPECT_EQ(notified, (std::vector<std::string>{first->id, first->id, second->id}));

    auto listed = store->list_stores(customer->id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 2U) << "a withdrawn store stays listed until the operator deletes it";
    EXPECT_EQ(listed->front().id, first->id) << "oldest first";
    {
        // What the operator writes once the claim is gone.
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO store_status (store_id, phase, message, observed_generation) "
                       "VALUES ($1::uuid, 'Deleted', 'deleted', 2)",
                       first->id);
        tx.exec_params("INSERT INTO store_status (store_id, phase, message, endpoint, "
                       "observed_generation) VALUES ($1::uuid, 'Ready', 'binds on first use', "
                       "'pvc/x', 1)",
                       second->id);
        tx.commit();
    }
    listed = store->list_stores(customer->id);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1U) << "a finished deletion is history";
    EXPECT_EQ(listed->front().phase, "Ready");
    EXPECT_EQ(listed->front().message, "binds on first use");
    EXPECT_EQ(listed->front().endpoint, "pvc/x");
    EXPECT_EQ(listed->front().observed_generation, 1);
    EXPECT_EQ(store->get_store(first->id)->phase, "Deleted") << "history is still readable";
    EXPECT_EQ(store->get_store(lake::control::new_uuid_v4()).error().code, "not_found");
}

// lake#465: migration 0044 gives earlier stores the space's only live workspace, records a space
// with several as needing a selection, and makes the workspace immutable afterwards.
TEST(PostgresStores, MigrationAssignsTheSoleWorkspaceAndRecordsTheRest) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "store-workspace-migration-test");
    std::vector<std::string> customers, stores;
    std::vector<std::vector<std::string>> workspaces;
    for (int index = 0; index < 2; ++index) {
        const std::string name = "store-migration-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        workspaces.emplace_back();
        // The second space has two workspaces, so its store cannot be assigned by itself.
        for (int count = 0; count <= index; ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            workspaces.back().push_back(workspace->id);
        }
        lake::control::NewManagedStore volume{customer->id, "volume", "cluster", "uploads", "t"};
        volume.workspace_id = workspaces.back().front();
        const auto created = store->create_store(volume);
        ASSERT_TRUE(created) << created.error().message;
        stores.push_back(created->id);
    }
    pqxx::connection connection(uri);
    {
        // The state before 0044: stores name no workspace.
        pqxx::work tx(connection);
        tx.exec("DROP TRIGGER store_workspace_routing ON stores");
        tx.exec("UPDATE stores SET workspace_id = NULL");
        tx.exec("DELETE FROM workspace_assignments WHERE object_table = 'stores'");
        tx.exec("DELETE FROM schema_migrations WHERE version = '0044_store_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0044_store_workspaces:");
    ASSERT_NE(start, std::string::npos);
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        EXPECT_EQ(tx.exec("SELECT count(*) FROM workspace_assignments WHERE object_table = 'stores' "
                          "AND outcome = 'sole_workspace'")[0][0].as<int>(), 1);
        EXPECT_EQ(tx.exec("SELECT count(*) FROM workspace_assignments WHERE object_table = 'stores' "
                          "AND outcome = 'needs_selection'")[0][0].as<int>(), 1);
        tx.commit();
    }
    EXPECT_EQ(store->get_store(stores[0])->workspace_id, workspaces[0].front());
    EXPECT_TRUE(store->get_store(stores[1])->workspace_id.empty()) << "two workspaces: unassigned";
    EXPECT_TRUE(store->withdraw_store(stores[1])) << "an unassigned store can still be withdrawn";
    EXPECT_THROW(([&] {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE stores SET workspace_id = NULL WHERE id = $1::uuid", stores[0]);
        tx.commit();
    })(), pqxx::check_violation);
}

// lake#315: migration 0051 gives each earlier repository, tombstoned ones included, its space's
// only live workspace, records a space with several or none as needing a selection, and the
// routing trigger keeps the workspace and the space fixed afterwards.
TEST(PostgresRepositories, MigrationAssignsTheSoleWorkspaceAndRoutingStaysFixed) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "repository-workspace-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    // Space 0 has one live workspace (and a deleted one), space 1 two, space 2 none by the time the
    // migration runs.
    std::vector<std::string> customers, repositories;
    std::vector<std::vector<std::string>> workspaces;
    for (int index = 0; index < 3; ++index) {
        const std::string name = "repository-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        workspaces.emplace_back();
        for (int count = 0; count < (index == 2 ? 1 : index + 1); ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            workspaces.back().push_back(workspace->id);
        }
        const auto repository = store->create_repository(
            {customer->id, "source", "", "private", workspaces.back().front()});
        ASSERT_TRUE(repository) << repository.error().message;
        EXPECT_EQ(repository->workspace_id, workspaces.back().front());
        repositories.push_back(repository->id);
    }
    const auto deleted_workspace = store->create_workspace({customers[0], "old", "dev", ""});
    ASSERT_TRUE(deleted_workspace);
    const auto tombstoned =
        store->create_repository({customers[0], "retired", "", "private", workspaces[0].front()});
    ASSERT_TRUE(tombstoned) << tombstoned.error().message;
    ASSERT_TRUE(store->delete_repository(
        tombstoned->id, {"2026-10-01T00:00:00Z", "2026-10-31T00:00:00Z"}));
    repositories.push_back(tombstoned->id);
    // The store names the field for each refusal the trigger would make.
    for (const std::string& wrong :
         {std::string(), std::string("not-a-uuid"), deleted_workspace->id, workspaces[1].front()}) {
        if (wrong == deleted_workspace->id) {
            ASSERT_TRUE(store->update_workspace(wrong, {.desired = "deleted"}));
        }
        const auto refused = store->create_repository({customers[0], "refused", "", "private", wrong});
        ASSERT_FALSE(refused) << wrong;
        EXPECT_EQ(refused.error().code, "invalid_input") << wrong;
    }

    pqxx::connection connection(uri);
    const std::string mine = "ARRAY['" + repositories[0] + "','" + repositories[1] + "','" +
                             repositories[2] + "','" + repositories[3] + "']::uuid[]";
    {
        // The state before 0051 for these repositories: no workspace; space 2's workspace is gone.
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec("UPDATE repositories SET workspace_id = NULL WHERE id = ANY(" + mine + ")");
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE workspaces SET desired = 'deleted' WHERE id = $1::uuid",
                       workspaces[2].front());
        tx.exec("DELETE FROM workspace_assignments WHERE object_table = 'repositories' AND "
                "object_id = ANY(" + mine + ")");
        tx.exec("DELETE FROM schema_migrations WHERE version = '0051_repository_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0051_repository_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto rows = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'repositories' AND object_id = $1::uuid",
            id);
        return rows.empty() ? std::pair<std::string, std::string>{}
                            : std::pair{rows[0][0].as<std::string>(), rows[0][1].as<std::string>()};
    };
    for (int pass = 0; pass < 2; ++pass) {
        {
            pqxx::work tx(connection);
            tx.exec("SET LOCAL client_min_messages TO error");
            tx.exec(schema.substr(start));
            tx.commit();
        }
        EXPECT_EQ(outcome(repositories[0]),
                  (std::pair<std::string, std::string>{"sole_workspace", workspaces[0].front()}));
        EXPECT_EQ(outcome(repositories[1]),
                  (std::pair<std::string, std::string>{"needs_selection", ""}));
        EXPECT_EQ(outcome(repositories[2]),
                  (std::pair<std::string, std::string>{"needs_selection", ""}));
        EXPECT_EQ(outcome(repositories[3]),
                  (std::pair<std::string, std::string>{"sole_workspace", workspaces[0].front()}));
    }
    EXPECT_EQ(store->get_repository(repositories[0])->workspace_id, workspaces[0].front());
    EXPECT_EQ(store->get_repository(repositories[3])->workspace_id, workspaces[0].front());
    EXPECT_TRUE(store->get_repository(repositories[1])->workspace_id.empty());
    const auto listed = store->list_repositories(customers[0], 10, workspaces[0].front());
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 2U);
    // An unassigned repository keeps working; its routing is assigned only by a reviewed migration.
    EXPECT_TRUE(store->rename_repository(repositories[1], "renamed"));
    const auto refused = [&](const std::string& statement) {
        EXPECT_THROW(([&] {
                         pqxx::work tx(connection);
                         tx.exec(statement);
                         tx.commit();
                     })(),
                     pqxx::check_violation)
            << statement;
    };
    refused("UPDATE repositories SET workspace_id = '" + workspaces[1].back() + "' WHERE id = '" +
            repositories[0] + "'");
    refused("UPDATE repositories SET workspace_id = NULL WHERE id = '" + repositories[0] + "'");
    refused("UPDATE repositories SET workspace_id = '" + workspaces[1].front() + "' WHERE id = '" +
            repositories[1] + "'");
    refused("UPDATE repositories SET customer_id = '" + customers[1] + "' WHERE id = '" +
            repositories[0] + "'");
    refused("INSERT INTO repositories (customer_id, name) VALUES ('" + customers[0] +
            "', 'no-workspace-" + suffix + "')");
    refused("INSERT INTO repositories (customer_id, name, workspace_id) VALUES ('" +
            customers[0] + "', 'foreign-" + suffix + "', '" + workspaces[1].front() + "')");
}

// lake#483: migration 0050 drops the retired ask_messages table and keeps the workspace
// assignments 0045 recorded for it. A rerun of the whole schema (as every schema Job does) leaves
// it dropped, and the 0050 section drops a table an older schema still has, with its rows.
TEST(PostgresAskMessages, MigrationDropsTheTableAndKeepsItsAssignments) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "ask-message-drop-test");
    const std::string name = "ask-drop-" + lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user({"taap:" + name, "", "Migration"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", name});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "ws-0", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;

    pqxx::connection connection(uri);
    const auto table_exists = [&] {
        pqxx::work tx(connection);
        return !tx.exec("SELECT to_regclass('ask_messages')")[0][0].is_null();
    };
    const auto assignments = [&] {
        pqxx::work tx(connection);
        return tx.exec_params("SELECT count(*) FROM workspace_assignments "
                              "WHERE object_table = 'ask_messages' AND customer_id = $1::uuid",
                              customer->id)[0][0]
            .as<int>();
    };
    EXPECT_FALSE(table_exists()) << "a fresh database never has the table";

    // A database from before 0050: the table with a row, and 0045's assignment of it.
    {
        pqxx::work tx(connection);
        tx.exec("CREATE TABLE IF NOT EXISTS ask_messages (id uuid PRIMARY KEY DEFAULT "
                "gen_random_uuid(), conversation_id uuid NOT NULL, customer_id uuid, user_id uuid "
                "NOT NULL, role text NOT NULL, content text NOT NULL DEFAULT '', payload jsonb NOT "
                "NULL DEFAULT 'null'::jsonb, created_at timestamptz NOT NULL DEFAULT now())");
        const std::string id = tx.exec_params(
            "INSERT INTO ask_messages (conversation_id, customer_id, user_id, role, content) "
            "VALUES (gen_random_uuid(), $1::uuid, $2::uuid, 'user', 'hello') RETURNING id::text",
            customer->id, user->id)[0][0].as<std::string>();
        tx.exec_params("INSERT INTO workspace_assignments (object_table, object_id, customer_id, "
                       "workspace_id, outcome) VALUES ('ask_messages', $1::uuid, $2::uuid, "
                       "$3::uuid, 'sole_workspace')",
                       id, customer->id, workspace->id);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0050_drop_ask_messages'");
        tx.commit();
    }
    ASSERT_TRUE(table_exists());

    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0050_drop_ask_messages:");
    ASSERT_NE(start, std::string::npos);
    {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
    }
    EXPECT_FALSE(table_exists()) << "0050 drops the table and its rows";
    EXPECT_EQ(assignments(), 1) << "the assignment records where the row went";

    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema);
        tx.commit();
        EXPECT_FALSE(table_exists()) << "a rerun of the whole schema does not recreate it";
        EXPECT_EQ(assignments(), 1);
    }
    pqxx::work tx(connection);
    EXPECT_EQ(tx.exec("SELECT count(*) FROM schema_migrations "
                      "WHERE version = '0050_drop_ask_messages'")[0][0]
                  .as<int>(),
              1);
    EXPECT_FALSE(tx.exec("SELECT to_regclass('drop_ask_messages_guard')")[0][0].is_null());
}

// Migration 0070 (lake#311) refuses rows with no recorded workspace owner, and drops every retired
// copy of workspace data once each row has one (or went with a deleted workspace). The check is
// about owners, not copies: copy and verify are a manual gate before applying. It runs in a
// PostgreSQL schema of its own, so the tables it recreates as an older database had them, and the
// row it leaves unresolved, are seen by no other test running against the same database.
TEST(PostgresMovedTables, MigrationRefusesRowsWithNoRecordedWorkspaceOwner) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    const std::string own = "m0070_" + lake::control::new_uuid_v4().substr(0, 8);
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("CREATE SCHEMA " + own);
        tx.commit();
    }
    connection.set_session_var("search_path", own);
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0070_drop_moved_tables:");
    ASSERT_NE(start, std::string::npos);
    const auto apply = [&](const std::string& sql) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(sql);
        tx.commit();
    };
    apply(schema);

    const std::vector<std::string> tables = {
        "opportunities",          "stream_owners",           "drive_share_links",
        "auth_app_webhook_deliveries", "auth_app_webhook_events", "auth_app_webhooks",
        "auth_app_signing_keys",  "auth_app_organizations",  "auth_app_users",
        "auth_app_authorization_codes", "metering_usage",    "metering_build_records",
        "forge_records",          "secrets_dynamic_leases",  "secrets_versions",
        "secrets_entries",        "secrets_encryption_keys", "secrets_data_keys",
        "secrets_mounts",         "platform_mirror",         "platform_environments"};
    const std::vector<std::pair<std::string, std::string>> columns = {
        {"auth_applications", "name"},          {"auth_applications", "allowed_origins"},
        {"auth_applications", "redirect_uris"}, {"auth_applications", "theme"},
        {"agent_tasks", "body"},                {"agent_task_status", "message"},
        {"agent_task_status", "result"},        {"repositories", "description"}};
    const auto present = [&] {
        pqxx::work tx(connection);
        std::vector<std::string> found;
        for (const auto& table : tables) {
            if (!tx.exec_params("SELECT to_regclass($1)", table)[0][0].is_null()) {
                found.push_back(table);
            }
        }
        for (const auto& [table, column] : columns) {
            if (!tx.exec_params("SELECT 1 FROM information_schema.columns WHERE table_schema = "
                                "current_schema() AND table_name = $1 AND column_name = $2",
                                table, column)
                     .empty()) {
                found.push_back(table + "." + column);
            }
        }
        return found;
    };
    EXPECT_TRUE(present().empty()) << "a fresh database never has them";

    // A database from before 0070, with one row in each retired table and column, each with the
    // owner its migration (0046-0064) recorded.
    std::string customer, workspace, application, repository, task;
    {
        pqxx::work tx(connection);
        const std::string user =
            tx.exec("INSERT INTO users (taap_id, display_name) VALUES ('taap:m0070', 'M') "
                    "RETURNING id::text")[0][0].as<std::string>();
        customer = tx.exec_params("INSERT INTO customers (customer_type, user_id, name) VALUES "
                                  "('individual', $1::uuid, 'm0070') RETURNING id::text",
                                  user)[0][0].as<std::string>();
        workspace = tx.exec_params("INSERT INTO workspaces (customer_id, name, environment) "
                                   "VALUES ($1::uuid, 'home', 'dev') RETURNING id::text",
                                   customer)[0][0].as<std::string>();
        application = lake::control::new_uuid_v4();
        tx.exec("ALTER TABLE auth_applications ADD COLUMN name text, ADD COLUMN allowed_origins "
                "jsonb, ADD COLUMN redirect_uris jsonb, ADD COLUMN theme jsonb");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, workspace_id, "
                       "name) VALUES ($1::uuid, $2::uuid, 'pool-m0070', $3::uuid, 'Store')",
                       application, customer, workspace);
        tx.exec("ALTER TABLE repositories ADD COLUMN description text NOT NULL DEFAULT ''");
        repository = tx.exec_params("INSERT INTO repositories (customer_id, name, workspace_id, "
                                    "description) VALUES ($1::uuid, 'liquid', $2::uuid, 'VCS') "
                                    "RETURNING id::text",
                                    customer, workspace)[0][0].as<std::string>();
        tx.exec("ALTER TABLE agent_tasks ADD COLUMN body jsonb NOT NULL DEFAULT '{}'::jsonb");
        tx.exec("ALTER TABLE agent_task_status ADD COLUMN message text NOT NULL DEFAULT '', "
                "ADD COLUMN result jsonb NOT NULL DEFAULT 'null'::jsonb");
        task = tx.exec_params("INSERT INTO agent_tasks (customer_id, agent, scopes, workspace_id, "
                              "body) VALUES ($1::uuid, 'agent-ask-orama', ARRAY['ask'], $2::uuid, "
                              "'{\"question\": \"hi\"}') RETURNING id::text",
                              customer, workspace)[0][0].as<std::string>();
        tx.exec_params("INSERT INTO agent_task_status (task_id, message) VALUES ($1::uuid, 'done')",
                       task);
        // The retired tables, with the columns the migration reads.
        tx.exec("CREATE TABLE opportunities (id uuid PRIMARY KEY, customer_id uuid)");
        tx.exec("CREATE TABLE stream_owners (customer_id uuid, workspace_id text, topic text)");
        tx.exec("CREATE TABLE drive_share_links (id uuid PRIMARY KEY)");
        tx.exec("CREATE TABLE auth_app_webhooks (id uuid PRIMARY KEY, application_id uuid)");
        tx.exec("CREATE TABLE auth_app_webhook_events (id uuid PRIMARY KEY, application_id uuid)");
        tx.exec("CREATE TABLE auth_app_webhook_deliveries (id uuid PRIMARY KEY, event_id uuid "
                "REFERENCES auth_app_webhook_events(id), webhook_id uuid REFERENCES "
                "auth_app_webhooks(id))");
        tx.exec("CREATE TABLE auth_app_signing_keys (application_id uuid, version integer)");
        tx.exec("CREATE TABLE auth_app_organizations (id uuid PRIMARY KEY, application_id uuid)");
        tx.exec("CREATE TABLE auth_app_users (application_id uuid, user_id uuid)");
        tx.exec("CREATE TABLE auth_app_authorization_codes (code_hash text PRIMARY KEY, "
                "application_id uuid, used_at timestamptz, expires_at timestamptz)");
        tx.exec("CREATE TABLE metering_usage (customer_id uuid, hour timestamptz, "
                "transfer_in_bytes bigint, transfer_out_bytes bigint)");
        tx.exec("CREATE TABLE metering_build_records (customer_id uuid, result_id text)");
        tx.exec("CREATE TABLE forge_records (id uuid PRIMARY KEY, repository_id uuid)");
        tx.exec("CREATE TABLE secrets_mounts (id uuid PRIMARY KEY)");
        tx.exec("CREATE TABLE secrets_entries (id uuid PRIMARY KEY, mount_id uuid REFERENCES "
                "secrets_mounts(id))");
        tx.exec("CREATE TABLE secrets_versions (entry_id uuid REFERENCES secrets_entries(id))");
        tx.exec("CREATE TABLE secrets_encryption_keys (id uuid PRIMARY KEY, mount_id uuid "
                "REFERENCES secrets_mounts(id), derived_from_id uuid REFERENCES "
                "secrets_encryption_keys(id))");
        tx.exec("CREATE TABLE secrets_data_keys (owner_id uuid)");
        tx.exec("CREATE TABLE secrets_dynamic_leases (id uuid PRIMARY KEY, mount_id uuid)");
        tx.exec("CREATE TABLE platform_environments (id uuid PRIMARY KEY)");
        tx.exec("CREATE TABLE platform_mirror (kind text, key text, value jsonb)");

        const std::string row = lake::control::new_uuid_v4();
        const std::string mount = lake::control::new_uuid_v4();
        const std::string secret = lake::control::new_uuid_v4();
        tx.exec_params("INSERT INTO opportunities VALUES ($1::uuid, $2::uuid)", row, customer);
        tx.exec_params("INSERT INTO stream_owners VALUES ($1::uuid, $2, 'orders')", customer,
                       workspace);
        tx.exec_params("INSERT INTO drive_share_links VALUES ($1::uuid)", row);
        tx.exec_params("INSERT INTO auth_app_webhooks VALUES ($1::uuid, $2::uuid)", row,
                       application);
        tx.exec_params("INSERT INTO auth_app_webhook_events VALUES ($1::uuid, $2::uuid)", row,
                       application);
        tx.exec_params("INSERT INTO auth_app_webhook_deliveries VALUES (gen_random_uuid(), "
                       "$1::uuid, $1::uuid)",
                       row);
        tx.exec_params("INSERT INTO auth_app_signing_keys VALUES ($1::uuid, 1)", application);
        tx.exec_params("INSERT INTO auth_app_organizations VALUES ($1::uuid, $2::uuid)", row,
                       application);
        tx.exec_params("INSERT INTO auth_app_users VALUES ($1::uuid, $2::uuid)", application, user);
        tx.exec_params("INSERT INTO auth_app_authorization_codes VALUES ('code', $1::uuid, NULL, "
                       "now() + interval '5 minutes')",
                       application);
        tx.exec_params("INSERT INTO metering_usage VALUES ($1::uuid, '2026-10-01T10:00:00Z', 10, 0)",
                       customer);
        tx.exec_params("INSERT INTO metering_build_records VALUES ($1::uuid, 'result-1')",
                       customer);
        tx.exec_params("INSERT INTO forge_records VALUES ($1::uuid, $2::uuid)", row, repository);
        tx.exec_params("INSERT INTO secrets_mounts VALUES ($1::uuid)", mount);
        tx.exec_params("INSERT INTO secrets_entries VALUES ($1::uuid, $2::uuid)", secret, mount);
        tx.exec_params("INSERT INTO secrets_versions VALUES ($1::uuid)", secret);
        tx.exec_params("INSERT INTO secrets_encryption_keys VALUES ($1::uuid, $2::uuid, NULL)",
                       secret, mount);
        tx.exec_params("INSERT INTO secrets_data_keys VALUES ($1::uuid)", mount);
        tx.exec_params("INSERT INTO secrets_dynamic_leases VALUES ($1::uuid, $2::uuid)", row,
                       mount);
        const std::string organization =
            tx.exec("INSERT INTO organizations (taap_id) VALUES ('taap:m0070-org') "
                    "RETURNING id::text")[0][0].as<std::string>();
        tx.exec_params("INSERT INTO secret_mount_routes (mount_id, customer_id, organization_id, "
                       "workspace_id) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid)",
                       mount, customer, organization, workspace);
        tx.exec_params("INSERT INTO platform_environments VALUES ($1::uuid)", row);
        tx.exec_params("INSERT INTO platform_mirror VALUES ('deployment', 'd-1', "
                       "jsonb_build_object('repository', $1::text))",
                       repository);
        // Each owner, with the outcome the table's migration records for an owned row.
        const std::map<std::string, std::string> outcomes = {
            {"opportunities", "task_workspace"},
            {"stream_owners", "recorded_workspace"},
            {"drive_share_links", "sole_workspace"},
            {"auth_app_webhook_events", "application_workspace"},
            {"auth_app_organizations", "application_workspace"},
            {"auth_app_users", "application_workspace"},
            {"auth_app_webhooks", "application_workspace"},
            {"auth_app_signing_keys", "application_workspace"},
            {"auth_app_authorization_codes", "application_workspace"},
            {"auth_application_settings", "application_workspace"},
            {"metering_usage", "sole_workspace"},
            {"metering_build_records", "sole_workspace"},
            {"forge_records", "repository_workspace"},
            {"platform_environments", "repository_workspace"},
            {"platform_mirror", "repository_workspace"},
            {"agent_task_content", "task_workspace"},
            {"repository_descriptions", "repository_workspace"}};
        const auto owned = [&](const std::string& table, const std::string& id) {
            tx.exec_params("INSERT INTO workspace_assignments (object_table, object_id, "
                           "customer_id, workspace_id, outcome) VALUES ($1, " + id + ", "
                           "$2::uuid, $3::uuid, $4) ON CONFLICT DO NOTHING",
                           table, customer, workspace, outcomes.at(table));
        };
        // An opportunity whose scan's workspace was deleted: 0046 recorded it as
        // deleted_workspace, without a workspace. It went with that workspace and does not block.
        const std::string gone = tx.exec_params("INSERT INTO workspaces (customer_id, name, "
                                                "environment, desired) VALUES ($1::uuid, 'gone', "
                                                "'dev', 'deleted') RETURNING id::text",
                                                customer)[0][0].as<std::string>();
        const std::string orphan = lake::control::new_uuid_v4();
        tx.exec_params("INSERT INTO opportunities VALUES ($1::uuid, $2::uuid)", orphan, customer);
        tx.exec_params("INSERT INTO workspace_assignments (object_table, object_id, customer_id, "
                       "workspace_id, outcome) VALUES ('opportunities', $1::uuid, $2::uuid, NULL, "
                       "'deleted_workspace')",
                       orphan, customer);
        EXPECT_FALSE(gone.empty());
        const std::string quoted = "'" + row + "'::uuid";
        for (const std::string table :
             {"opportunities", "drive_share_links", "auth_app_webhook_events",
              "auth_app_organizations", "auth_app_webhooks", "forge_records",
              "platform_environments"}) {
            owned(table, quoted);
        }
        owned("stream_owners", "md5('" + customer + "' || E'\\n' || '" + workspace +
                                   "' || E'\\n' || 'orders')::uuid");
        owned("auth_app_signing_keys", "'" + application + "'::uuid");
        owned("auth_app_users", "md5('" + application + "' || E'\\n' || '" + user + "')::uuid");
        owned("auth_app_authorization_codes", "md5('code')::uuid");
        owned("metering_usage", "md5('" + customer + "' || E'\\n' || '2026-10-01T10:00:00Z')::uuid");
        owned("metering_build_records", "md5('" + customer + "' || E'\\n' || 'result-1')::uuid");
        owned("platform_mirror", "md5('deployment' || E'\\n' || 'd-1')::uuid");
        owned("auth_application_settings", "'" + application + "'::uuid");
        owned("agent_task_content", "'" + task + "'::uuid");
        owned("repository_descriptions", "'" + repository + "'::uuid");
        tx.exec("DELETE FROM schema_migrations WHERE version = '0070_drop_moved_tables'");
        tx.commit();
    }
    ASSERT_EQ(present().size(), tables.size() + columns.size());

    // A row whose owner is still to be chosen holds the whole migration back: nothing is dropped.
    const std::string open = lake::control::new_uuid_v4();
    {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO forge_records VALUES ($1::uuid, $2::uuid)", open, repository);
        tx.exec_params("INSERT INTO workspace_assignments (object_table, object_id, customer_id, "
                       "workspace_id, outcome) VALUES ('forge_records', $1::uuid, $2::uuid, NULL, "
                       "'needs_selection')",
                       open, customer);
        tx.commit();
    }
    try {
        apply(schema.substr(start));
        ADD_FAILURE() << "0070 applied with an unresolved row";
    } catch (const pqxx::sql_error& error) {
        EXPECT_NE(std::string(error.what()).find("forge_records=1"), std::string::npos)
            << error.what();
        EXPECT_EQ(std::string(error.what()).find("opportunities="), std::string::npos)
            << "a deleted_workspace opportunity does not block: " << error.what();
    }
    EXPECT_EQ(present().size(), tables.size() + columns.size()) << "nothing dropped";
    // A row that went with a deleted workspace does not.
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspace_assignments SET outcome = 'workspace_deleted' "
                       "WHERE object_table = 'forge_records' AND object_id = $1::uuid",
                       open);
        tx.commit();
    }

    // The section from 0070 on, twice, then the whole schema twice: dropped and never recreated.
    for (int pass = 0; pass < 2; ++pass) {
        apply(schema.substr(start));
        EXPECT_TRUE(present().empty()) << pass;
    }
    for (int pass = 0; pass < 2; ++pass) {
        apply(schema);
        EXPECT_TRUE(present().empty()) << pass;
    }
    {
        pqxx::work tx(connection);
        EXPECT_GE(tx.exec("SELECT count(*) FROM workspace_assignments")[0][0].as<int>(), 16)
            << "the record of where each row went stays";
        EXPECT_FALSE(tx.exec("SELECT to_regclass('drop_moved_tables_guard')")[0][0].is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('drive_share_link_routes')")[0][0].is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('secret_mount_routes')")[0][0].is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('secret_lease_routes')")[0][0].is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('audit_events')")[0][0].is_null());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('keystore')")[0][0].is_null());
        EXPECT_EQ(tx.exec("SELECT count(*) FROM auth_applications")[0][0].as<int>(), 1)
            << "the routing rows stay";
        EXPECT_EQ(tx.exec("SELECT count(*) FROM repositories")[0][0].as<int>(), 1);
        EXPECT_EQ(tx.exec("SELECT count(*) FROM agent_task_status")[0][0].as<int>(), 1);
    }
    pqxx::work drop(connection);
    drop.exec("DROP SCHEMA " + own + " CASCADE");
    drop.commit();
}

// Migration 0064 (S11): repository_refs, a copy of liquid's refs nothing read, is
// dropped (lake#319) and never recreated by a rerun. The description that 0064 left in the row
// for the copy is gone too (0070), so the row is routing only and a create under a chosen id keeps
// it.
TEST(PostgresRepositories, MigrationDropsRefsAndTheRowIsRoutingOnly) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "repository-records-migration-test");
    const std::string name = "repo-records-" + lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user({"taap:" + name, "", "Migration"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", name});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "ws-0", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto owned = store->create_repository({customer->id, "owned", "", "private", workspace->id});
    ASSERT_TRUE(owned) << owned.error().message;

    pqxx::connection connection(uri);
    const auto refs_exist = [&] {
        pqxx::work tx(connection);
        return !tx.exec("SELECT to_regclass('repository_refs')")[0][0].is_null();
    };
    EXPECT_FALSE(refs_exist()) << "a fresh database never has the table";
    // A database from before 0064: repository_refs with a row.
    {
        pqxx::work tx(connection);
        tx.exec("CREATE TABLE IF NOT EXISTS repository_refs (repository_id uuid NOT NULL "
                "REFERENCES repositories(id) ON DELETE CASCADE, name varchar(256) NOT NULL, head "
                "text NOT NULL, updated_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY "
                "(repository_id, name))");
        tx.exec_params("INSERT INTO repository_refs (repository_id, name, head) VALUES "
                       "($1::uuid, 'main', 'change-1')",
                       owned->id);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0064_repository_records'");
        tx.commit();
    }
    ASSERT_TRUE(refs_exist());

    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0064_repository_records:");
    ASSERT_NE(start, std::string::npos);
    // The schema section from 0064 on, twice: the second run changes nothing.
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_FALSE(refs_exist()) << "0064 drops the table and its rows";
    }
    // The whole schema, twice: repository_refs is never recreated.
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema);
        tx.commit();
        EXPECT_FALSE(refs_exist()) << "a rerun of the whole schema does not recreate it";
    }

    // The routing row answers no description and has no column for one.
    const auto routing = store->get_repository(owned->id);
    ASSERT_TRUE(routing);
    EXPECT_TRUE(routing->description.empty());
    {
        pqxx::work tx(connection);
        EXPECT_TRUE(tx.exec("SELECT 1 FROM information_schema.columns WHERE table_schema = "
                            "current_schema() AND table_name = 'repositories' AND column_name = "
                            "'description'")
                        .empty());
    }
    // A create under a chosen id keeps it (the settings record is written under it first).
    const std::string chosen = lake::control::new_uuid_v4();
    lake::control::NewRepository named{customer->id, "chosen", "", "private", workspace->id};
    named.id = chosen;
    const auto with_id = store->create_repository(named);
    ASSERT_TRUE(with_id) << with_id.error().message;
    EXPECT_EQ(with_id->id, chosen);

    pqxx::work tx(connection);
    EXPECT_FALSE(tx.exec("SELECT to_regclass('repository_records_guard')")[0][0].is_null());
}

// Migration 0066 (lake#513): a repository 0051 left without a workspace is
// assigned its space's only live workspace once the space has exactly one, with its routing and
// description owners recorded; a space with two workspaces is left alone and reported. The
// trigger lets only the migration's own block make that change: every other change of
// workspace_id is still refused, in the migration's transaction and afterwards.
TEST(PostgresRepositories, MigrationAssignsUnroutedRepositoriesTheirSoleWorkspace) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "unrouted-repository-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    // Space 0 had one workspace, deleted before 0051 (so it had none then); space 1 has two.
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "unrouted-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        for (int count = 0; count <= index; ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            workspaces[index].push_back(workspace->id);
        }
    }
    std::vector<std::string> repositories;
    for (int index = 0; index < 2; ++index) {
        const auto created = store->create_repository(
            {customers[index], "liquid", "", "private", workspaces[index].front()});
        ASSERT_TRUE(created) << created.error().message;
        repositories.push_back(created->id);
    }
    pqxx::connection connection(uri);
    const std::string mine =
        "ARRAY['" + repositories[0] + "','" + repositories[1] + "']::uuid[]";
    {
        // The state 0051 and 0064 left: no workspace, needs_selection for the routing row and the
        // description, and space 0 without a live workspace.
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec("UPDATE repositories SET workspace_id = NULL WHERE id = ANY(" + mine + ")");
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE workspaces SET desired = 'deleted' WHERE id = $1::uuid",
                       workspaces[0].front());
        tx.exec("DELETE FROM workspace_assignments WHERE object_id = ANY(" + mine + ")");
        tx.exec("INSERT INTO workspace_assignments (object_table, object_id, customer_id, "
                "workspace_id, outcome) SELECT t, r.id, r.customer_id, NULL, 'needs_selection' "
                "FROM repositories r, unnest(ARRAY['repositories', 'repository_descriptions']) t "
                "WHERE r.id = ANY(" + mine + ")");
        tx.commit();
    }
    // The space gets its first live workspace after 0051, as on dev.
    const auto later = store->create_workspace({customers[0], "workspace", "prod", ""});
    ASSERT_TRUE(later) << later.error().message;

    using Outcome = std::pair<std::string, std::string>;
    const auto outcome = [&](const std::string& table, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = $1 AND object_id = $2::uuid",
            table, id);
        return found.empty() ? Outcome{}
                             : Outcome{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    const auto refused = [&](pqxx::dbtransaction& tx, const std::string& statement) {
        pqxx::subtransaction attempt(tx, "refused");
        EXPECT_THROW(attempt.exec(statement), pqxx::check_violation) << statement;
    };
    std::vector<std::string> warnings;
    connection.set_notice_handler([&](pqxx::zview notice) { warnings.emplace_back(notice); });

    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0066_assign_unrouted_repositories:");
    ASSERT_NE(start, std::string::npos);
    for (int pass = 0; pass < 2; ++pass) {
        warnings.clear();
        {
            // The first pass runs the block as a database from before 0066 would; the second is a
            // rerun, which changes nothing.
            pqxx::work tx(connection);
            if (pass == 0) {
                tx.exec("DELETE FROM schema_migrations "
                        "WHERE version = '0066_assign_unrouted_repositories'");
            }
            tx.exec(schema.substr(start));
            // In the migration's own transaction, after its block, the exception is closed.
            refused(tx, "UPDATE repositories SET workspace_id = '" + workspaces[1].front() +
                            "' WHERE id = '" + repositories[1] + "'");
            tx.commit();
        }
        EXPECT_EQ(store->get_repository(repositories[0])->workspace_id, later->id) << pass;
        EXPECT_TRUE(store->get_repository(repositories[1])->workspace_id.empty()) << pass;
        EXPECT_EQ(outcome("repositories", repositories[0]), (Outcome{"sole_workspace", later->id}));
        EXPECT_EQ(outcome("repository_descriptions", repositories[0]),
                  (Outcome{"repository_workspace", later->id}));
        EXPECT_EQ(outcome("repositories", repositories[1]), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome("repository_descriptions", repositories[1]),
                  (Outcome{"needs_selection", ""}));
        const auto reported = std::ranges::count_if(warnings, [&](const std::string& notice) {
            return notice.find("workspace_id requires selection: table=repositories, object=" +
                               repositories[1]) != std::string::npos &&
                   notice.find("live_workspaces=2") != std::string::npos;
        });
        EXPECT_EQ(reported, pass == 0 ? 1 : 0) << "reported on the run that assigns, once";
    }
    connection.set_notice_handler([](pqxx::zview) {});

    // Afterwards the trigger refuses every change of workspace_id, as before.
    pqxx::work tx(connection);
    refused(tx, "UPDATE repositories SET workspace_id = '" + workspaces[1].front() +
                    "' WHERE id = '" + repositories[1] + "'");
    refused(tx, "UPDATE repositories SET workspace_id = NULL WHERE id = '" + repositories[0] + "'");
    refused(tx, "UPDATE repositories SET workspace_id = '" + workspaces[1].front() +
                    "' WHERE id = '" + repositories[0] + "'");
    // The setting alone opens only NULL to a live workspace of the repository's own space.
    tx.exec("SELECT set_config('lake.repository_assignment', "
            "'0066_assign_unrouted_repositories', true)");
    refused(tx, "UPDATE repositories SET workspace_id = '" + workspaces[1].back() +
                    "' WHERE id = '" + repositories[0] + "'");
    refused(tx, "UPDATE repositories SET workspace_id = '" + later->id + "' WHERE id = '" +
                    repositories[1] + "'");
    refused(tx, "UPDATE repositories SET customer_id = '" + customers[0] + "' WHERE id = '" +
                    repositories[1] + "'");
    tx.abort();
    pqxx::work check(connection);
    EXPECT_FALSE(
        check.exec("SELECT to_regclass('assign_unrouted_repositories_guard')")[0][0].is_null());
}

// Agent launches, their credential and cron schedules against the production
// store (docs/api/agent-launches.md, lake#231).
TEST(PostgresAgentLaunches, LaunchesCredentialsSchedulesAndConversationsFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:al-" + suffix, "al-" + suffix + "@example.com", "Launcher"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "al-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_services",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    const auto workspace = store->create_workspace({customer->id, "agents", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    // lake#442: a launch names its workspace; the routing trigger refuses one that does not.
    EXPECT_FALSE(store->create_agent_launch({customer->id,
                                             "a-1",
                                             "Watcher",
                                             "job",
                                             "manual",
                                             nullptr,
                                             lake::control::agent_launch_scopes,
                                             user->id,
                                             user->taap_id}));
    const auto launch = store->create_agent_launch({customer->id,
                                                    "a-1",
                                                    "Watcher",
                                                    "service",
                                                    "manual",
                                                    json{{"week", 39}},
                                                    lake::control::agent_launch_scopes,
                                                    user->id,
                                                    user->taap_id,
                                                    workspace->id});
    ASSERT_TRUE(launch) << launch.error().message;
    EXPECT_EQ(launch->workspace_id, workspace->id);
    EXPECT_TRUE(store->list_agent_launches(customer->id, "", 10, lake::control::new_uuid_v4())
                    ->empty())
        << "another workspace lists none of this one's runs";
    EXPECT_EQ(store->list_agent_launches(customer->id, "", 10, workspace->id)->size(), 1U);
    EXPECT_EQ(launch->slug, lake::control::managed_service_slug("al", launch->id));
    EXPECT_EQ(launch->input["week"], 39);
    EXPECT_EQ(launch->scopes, lake::control::agent_launch_scopes);
    EXPECT_EQ(launch->phase, "") << "no status row yet";
    listener.await_notification(5, 0);
    EXPECT_NE(std::ranges::find(notified, launch->id), notified.end());

    const std::string credential = "lal." + launch->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(store->context_for(credential)) << "not before the operator reports its digest";
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO agent_launch_status (launch_id, phase, endpoint, "
                       "credential_sha256, exit_code, result) VALUES ($1::uuid, 'Ready', "
                       "'http://al:8080', $2, NULL, '{\"reply\":\"ok\"}'::jsonb)",
                       launch->id,
                       lake::control::identity_crypto::sha256_hex(credential));
        tx.commit();
    }
    const auto acting = store->context_for(credential);
    ASSERT_TRUE(acting) << acting.error().message;
    EXPECT_EQ(acting->user_id, user->id);
    EXPECT_EQ(acting->kind, "agent_launch");
    EXPECT_EQ(acting->scopes, lake::control::agent_launch_scopes);
    const auto listed = store->list_agent_launches(customer->id, "a-1", 10);
    ASSERT_TRUE(listed && listed->size() == 1U);
    EXPECT_EQ(listed->front().phase, "Ready");
    EXPECT_EQ(listed->front().endpoint, "http://al:8080");
    EXPECT_EQ(listed->front().result["reply"], "ok");
    ASSERT_TRUE(store->withdraw_agent_launch(launch->id));
    EXPECT_FALSE(store->context_for(credential)) << "a stopped launch's credential stops too";

    // lake#442: a launch the operator never reports on is withdrawn with a reason, once; one it
    // reported on is left alone.
    const auto stuck = store->create_agent_launch({customer->id,
                                                   "a-1",
                                                   "Watcher",
                                                   "job",
                                                   "manual",
                                                   nullptr,
                                                   lake::control::agent_launch_scopes,
                                                   user->id,
                                                   user->taap_id,
                                                   workspace->id});
    ASSERT_TRUE(stuck) << stuck.error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("UPDATE agent_launches SET created_at = now() - interval '10 minutes' "
                       "WHERE id = $1::uuid",
                       stuck->id);
        tx.commit();
    }
    const auto expired = store->expire_unstarted_agent_launches(300, "Timed out: test.");
    ASSERT_TRUE(expired) << expired.error().message;
    EXPECT_GE(*expired, 1U);
    const auto withdrawn = store->get_agent_launch(stuck->id);
    EXPECT_EQ(withdrawn->desired, "absent");
    EXPECT_EQ(withdrawn->generation, 2);
    EXPECT_EQ(withdrawn->stop_reason, "Timed out: test.");
    EXPECT_EQ(store->get_agent_launch(launch->id)->stop_reason, "") << "it had a status row";
    EXPECT_EQ(store->get_agent_launch(stuck->id)->generation, 2);

    // Cron schedules: several per agent, each with its own slug and credential (migration 0031).
    const auto weekly =
        store->create_agent_cron_schedule({.customer_id = customer->id,
                                           .agent_id = "a-1",
                                           .agent_name = "Watcher",
                                           .cron = "0 9 * * 1",
                                           .description = "every monday at 09:00",
                                           .input = json{{"week", 39}},
                                           .scopes = lake::control::agent_launch_scopes,
                                           .created_by_user_id = user->id,
                                           .created_by_taap_id = user->taap_id,
                                           .workspace_id = workspace->id});
    ASSERT_TRUE(weekly) << weekly.error().message;
    const auto nightly =
        store->create_agent_cron_schedule({.customer_id = customer->id,
                                           .agent_id = "a-1",
                                           .agent_name = "Watcher",
                                           .cron = "30 2 * * *",
                                           .time_zone = "America/Toronto",
                                           .scopes = lake::control::agent_launch_scopes,
                                           .created_by_user_id = user->id,
                                           .created_by_taap_id = user->taap_id,
                                           .workspace_id = workspace->id});
    ASSERT_TRUE(nightly) << nightly.error().message;
    EXPECT_EQ(weekly->slug, lake::control::managed_service_slug("as", weekly->id));
    EXPECT_EQ(weekly->time_zone, "Etc/UTC");
    EXPECT_EQ(nightly->time_zone, "America/Toronto");
    listener.await_notification(5, 0);
    EXPECT_NE(std::ranges::find(notified, weekly->id), notified.end());
    auto schedules = store->list_agent_cron_schedules(customer->id, "a-1");
    ASSERT_TRUE(schedules && schedules->size() == 2U);
    EXPECT_EQ(schedules->front().id, weekly->id) << "oldest first";

    const std::string schedule_credential =
        "las." + weekly->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(store->context_for(schedule_credential));
    const std::string run_id = lake::control::new_uuid_v4();
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO agent_cron_schedule_status (schedule_id, phase, message, "
                       "last_schedule_at, credential_sha256, observed_generation) VALUES "
                       "($1::uuid, 'Ready', 'scheduled', now(), $2, 1)",
                       weekly->id,
                       lake::control::identity_crypto::sha256_hex(schedule_credential));
        tx.exec_params("INSERT INTO agent_cron_runs (id, schedule_id, job_name, phase, message, "
                       "exit_code, result) VALUES ($1::uuid, $2::uuid, 'lake-as-x-1', "
                       "'Succeeded', 'finished', 0, '{\"reply\":\"3 at risk\"}'::jsonb)",
                       run_id,
                       weekly->id);
        tx.commit();
    }
    const auto scheduled_acting = store->context_for(schedule_credential);
    ASSERT_TRUE(scheduled_acting) << scheduled_acting.error().message;
    EXPECT_EQ(scheduled_acting->user_id, user->id);
    EXPECT_EQ(scheduled_acting->kind, "agent_schedule");
    const auto got = store->get_agent_cron_schedule(weekly->id);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->phase, "Ready");
    EXPECT_FALSE(got->last_schedule_at.empty());
    const auto runs = store->list_agent_cron_runs(customer->id, "a-1", 10);
    ASSERT_TRUE(runs && runs->size() == 1U);
    EXPECT_EQ(runs->front().workspace_id, workspace->id);
    EXPECT_TRUE(store->list_agent_cron_runs(customer->id, "a-1", 10, lake::control::new_uuid_v4())
                    ->empty());
    EXPECT_EQ(store->list_agent_cron_schedules(customer->id, "a-1", workspace->id)->size(), 2U);
    EXPECT_EQ(runs->front().id, run_id);
    EXPECT_EQ(runs->front().trigger, "schedule");
    EXPECT_EQ(runs->front().exit_code, 0);
    EXPECT_EQ(runs->front().result["reply"], "3 at risk");
    EXPECT_EQ(runs->front().input["week"], 39);
    ASSERT_TRUE(store->withdraw_agent_cron_schedule(weekly->id));
    EXPECT_FALSE(store->context_for(schedule_credential))
        << "a removed schedule's credential stops";
    EXPECT_FALSE(store->withdraw_agent_cron_schedule(weekly->id)) << "already removed";
    schedules = store->list_agent_cron_schedules(customer->id, "a-1");
    ASSERT_TRUE(schedules && schedules->size() == 1U);

}

TEST(PostgresSessions, AnExpiredCredentialIsToldApartFromAnUnknownOne) {
    // lake#450: a caller holding the real secret of an expired token learns it expired; any other
    // secret is simply not active. The token's expiry is moved into the past explicitly rather
    // than waited out, so the test does not depend on how fast a loaded host runs (lake#679).
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:exp-" + suffix, "exp-" + suffix + "@example.com", "Expiring"});
    ASSERT_TRUE(user) << user.error().message;
    const auto token =
        store->create_token({user->id, "lake", "short", "test", {"list_customers"}, 3600, false});
    ASSERT_TRUE(token) << token.error().message;
    ASSERT_TRUE(store->context_for(token->secret)) << "live before it expires";
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        const pqxx::result moved = tx.exec_params(
            "UPDATE sessions SET expires_at = now() - interval '1 second' WHERE id = $1::uuid",
            token->session.id);
        ASSERT_EQ(moved.affected_rows(), 1);
        tx.commit();
    }
    const auto expired = store->context_for(token->secret);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "expired");
    std::string wrong = token->secret;
    wrong.back() = wrong.back() == 'a' ? 'b' : 'a';
    const auto unknown = store->context_for(wrong);
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().code, "expired") << "a wrong secret never learns of the session";
}

// Migration 0068 (lake#383 phase A): tenant image repositories get a route only where their owner
// is unambiguous (the apps and builds of one live workspace use the name; both `<name>` and
// `cache/<name>`), a name two workspaces use and one used only by a deleted workspace get none, a
// rerun changes nothing, and a route never changes or points at a deleted workspace.
TEST(PostgresImageRoutes, MigrationRoutesUnambiguousImagesOnly) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "image-routes-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string name = "image-routes-" + suffix;
    const auto user = store->create_user({"taap:" + name, "", "Migration"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", name});
    ASSERT_TRUE(customer) << customer.error().message;
    std::vector<std::string> workspaces;
    for (const char* label : {"one", "two", "gone"}) {
        const auto workspace = store->create_workspace({customer->id, label, "dev", ""});
        ASSERT_TRUE(workspace) << workspace.error().message;
        workspaces.push_back(workspace->id);
    }
    const auto app = [&](const std::string& workspace, const std::string& image) {
        const auto created = store->create_web_app(
            {.customer_id = customer->id,
             .workspace_id = workspace,
             .name = image,
             .mode = "image",
             .image = "lake.example.test/" + name + "/" + image + ":v1",
             .framework = "container",
             .port = 8080,
             .host = image + "-" + suffix + ".apps.example.test"});
        ASSERT_TRUE(created) << created.error().message;
    };
    app(workspaces[0], "web");
    app(workspaces[0], "shared");
    app(workspaces[2], "lost");
    const auto source = store->create_repository({customer->id, "source", "", "private", workspaces[1]});
    ASSERT_TRUE(source) << source.error().message;
    for (const char* image : {"shared", "api"}) {
        const auto build = store->create_build({.customer_id = customer->id,
                                                .workspace_id = workspaces[1],
                                                .repository = source->id,
                                                .revision = "main",
                                                .target = "registry/" + name + "/" + image + ":v1"});
        ASSERT_TRUE(build) << build.error().message;
    }
    ASSERT_TRUE(store->update_workspace(workspaces[2], {.desired = "deleted"}));

    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0068_oci_repository_routes'");
        tx.exec_params("DELETE FROM oci_repository_routes WHERE customer_id = $1::uuid",
                       customer->id);
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0068_oci_repository_routes:");
    ASSERT_NE(start, std::string::npos);
    const auto routes = [&] {
        std::map<std::string, std::string> found;
        pqxx::work tx(connection);
        for (const auto& row : tx.exec_params(
                 "SELECT name, workspace_id::text FROM oci_repository_routes "
                 "WHERE customer_id = $1::uuid",
                 customer->id)) {
            found[row[0].as<std::string>()] = row[1].as<std::string>();
        }
        return found;
    };
    const std::map<std::string, std::string> expected{{"web", workspaces[0]},
                                                      {"cache/web", workspaces[0]},
                                                      {"api", workspaces[1]},
                                                      {"cache/api", workspaces[1]}};
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(routes(), expected) << "pass " << pass;
    }
    // A route made after the migration is untouched by a rerun of the whole schema.
    ASSERT_TRUE(store->create_oci_repository_route(customer->id, "shared", workspaces[1]));
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema);
        tx.commit();
    }
    EXPECT_EQ(routes().size(), expected.size() + 1);
    EXPECT_EQ(routes().at("shared"), workspaces[1]);

    // Immutable, and only to a live workspace of the same space.
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE oci_repository_routes SET workspace_id = $2::uuid "
                                    "WHERE customer_id = $1::uuid AND name = 'web'",
                                    customer->id, workspaces[1]),
                     pqxx::check_violation);
    }
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE oci_repository_routes SET name = 'renamed' "
                                    "WHERE customer_id = $1::uuid AND name = 'web'",
                                    customer->id),
                     pqxx::check_violation);
    }
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("INSERT INTO oci_repository_routes (customer_id, name, "
                                    "workspace_id) VALUES ($1::uuid, 'late', $2::uuid)",
                                    customer->id, workspaces[2]),
                     pqxx::check_violation);
    }
    const auto refused = store->create_oci_repository_route(customer->id, "web", workspaces[1]);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "conflict");
    const auto deleted = store->create_oci_repository_route(customer->id, "late", workspaces[2]);
    ASSERT_FALSE(deleted);
    EXPECT_EQ(deleted.error().code, "invalid_input");
}

// Migration 0069 (operator#259, operator#260): a workspace's storage only grows, whoever writes
// it; every workspace and lake's store get a default snapshot policy, by the backfill and by the
// trigger on new workspaces; a rerun of the schema changes neither the sizes nor the policies; and
// the policy accepts only `store` or a workspace id, five cron fields and a bounded keep.
TEST(PostgresVolumes, StorageNeverShrinksAndEveryTargetHasAPolicy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "workspace-volumes-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string name = "volumes-" + suffix;
    const auto user = store->create_user({"taap:" + name, "", "Volumes"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", name});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "main", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    EXPECT_EQ(workspace->storage_gi, 5);

    pqxx::connection connection(uri);
    const auto policy_count = [&](const std::string& target) {
        pqxx::work tx(connection);
        return tx.exec_params("SELECT count(*) FROM volume_snapshot_policies WHERE target = $1",
                              target)[0][0]
            .as<int>();
    };
    EXPECT_EQ(policy_count("store"), 1);
    EXPECT_EQ(policy_count(workspace->id), 1) << "the trigger adds a new workspace's policy";

    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspaces SET storage_gi = 20 WHERE id = $1::uuid", workspace->id);
        tx.commit();
    }
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE workspaces SET storage_gi = 10 WHERE id = $1::uuid",
                                    workspace->id),
                     pqxx::check_violation);
    }
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE workspaces SET storage_phase = 'Shrinking' WHERE id = "
                                    "$1::uuid",
                                    workspace->id),
                     pqxx::check_violation);
    }
    const auto refused = store->set_workspace_storage(workspace->id, 19);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "invalid_input");
    EXPECT_EQ(store->get_workspace(workspace->id)->storage_gi, 20);

    // The backfill: a workspace from before the migration gets its policy, once.
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0069_workspace_volumes'");
        tx.exec_params("DELETE FROM volume_snapshot_policies WHERE target IN ($1, 'store')",
                       workspace->id);
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema);
        tx.commit();
        EXPECT_EQ(policy_count("store"), 1) << "pass " << pass;
        EXPECT_EQ(policy_count(workspace->id), 1) << "pass " << pass;
    }
    EXPECT_EQ(store->get_workspace(workspace->id)->storage_gi, 20) << "a rerun keeps the size";

    for (const char* statement :
         {"INSERT INTO volume_snapshot_policies (target) VALUES ('everything')",
          "INSERT INTO volume_snapshot_policies (target, schedule) VALUES "
          "('00000000-0000-4000-8000-000000000001', '@daily')",
          "INSERT INTO volume_snapshot_policies (target, keep) VALUES "
          "('00000000-0000-4000-8000-000000000001', 0)"}) {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec(statement), pqxx::check_violation) << statement;
    }
}

// lake#561: an organization's own space, as get_organization reads it.
TEST(PostgresWebApp, AnOrganizationNamesItsOwnSpace) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization =
        store->create_organization({"taap:1:ed25519:web-app-" + suffix, "web app", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto none = store->organization_customer(organization->id);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, "not_found");
    const auto customer =
        store->create_customer({"organization", "", organization->id, "web-app-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto space = store->organization_customer(organization->id);
    ASSERT_TRUE(space) << space.error().message;
    EXPECT_EQ(space->id, customer->id);
    // MFA enrollment on PostgreSQL is read through `viewer` in
    // HttpServer/HttpResponseIsolation.TheViewerSaysWhoIsSignedInAndWhereTheyAre.
}

namespace {

// lake#643 against one store: the resources the operator reports per workload, rates per class,
// an invoice line naming its class, and when a launch's status was last written.
void workload_resources_and_classes_follow_the_contract(lake::control::Store& store,
                                                        const std::string& suffix) {
    const auto user = store.create_user(
        {"taap:1:ed25519:wr-" + suffix, "wr-" + suffix + "@example.com", "Resources"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", "wr-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    const std::string app = lake::control::new_uuid_v4();
    const std::string build = lake::control::new_uuid_v4();
    ASSERT_TRUE(
        store.put_workload_resources({"app", app, 2, 1500, 3ULL << 30, 1, "h100", 80ULL << 30}));
    ASSERT_TRUE(store.put_workload_resources({"build", build, 1, 500, 1ULL << 30}));
    // The operator replaces a row: the build's pods finished, its resources stay.
    ASSERT_TRUE(store.put_workload_resources({"build", build, 0, 500, 1ULL << 30}));
    EXPECT_EQ(store.put_workload_resources({"repository", app, 1}).error().code, "invalid_input");
    const auto rows = store.workload_resources();
    ASSERT_TRUE(rows) << rows.error().message;
    const auto find = [&](const std::string& kind, const std::string& id) {
        return std::ranges::find_if(*rows, [&](const lake::control::WorkloadResources& row) {
            return row.object_kind == kind && row.object_id == id;
        });
    };
    const auto gpu_app = find("app", app);
    ASSERT_NE(gpu_app, rows->end());
    EXPECT_EQ(gpu_app->pods, 2);
    EXPECT_EQ(gpu_app->cpu_millis, 1500U);
    EXPECT_EQ(gpu_app->memory_bytes, 3ULL << 30);
    EXPECT_EQ(gpu_app->gpus, 1);
    EXPECT_EQ(gpu_app->gpu_class, "h100");
    EXPECT_EQ(gpu_app->vram_bytes, 80ULL << 30);
    EXPECT_FALSE(gpu_app->observed_at.empty());
    const auto finished = find("build", build);
    ASSERT_NE(finished, rows->end());
    EXPECT_EQ(finished->pods, 0);
    EXPECT_EQ(finished->cpu_millis, 500U);

    // A GPU rate per class beside the default; the same class twice is refused.
    lake::control::BillingPlan list;
    list.id = "usage-202611-" + std::string(17 - suffix.size(), '0') + suffix;
    list.name = "Usage";
    list.currency = "USD";
    list.prices = {{"gpu", 38'750, 60, 0},
                   {"gpu", 10'800, 60, 0, "l4"},
                   {"gpu", 78'750, 60, 0, "h100"},
                   {"cpu", 1'500, 60'000, 0}};
    const auto stored = store.put_billing_plan(list);
    ASSERT_TRUE(stored) << stored.error().message;
    ASSERT_EQ(stored->prices.size(), 4U);
    EXPECT_EQ(stored->prices[0].dimension, "cpu");
    EXPECT_EQ(stored->prices[1].class_name, "") << "by dimension, then class";
    EXPECT_EQ(stored->prices[2].class_name, "h100");
    EXPECT_EQ(stored->prices[3].class_name, "l4");
    EXPECT_EQ(stored->prices[3].unit_micros, 10'800U);
    list.prices.push_back({"gpu", 1, 60, 0, "l4"});
    EXPECT_EQ(store.put_billing_plan(list).error().code, "invalid_input");

    // An invoice line keeps the class it billed.
    lake::control::Invoice invoice;
    invoice.customer_id = customer->id;
    invoice.period = "2026-11";
    invoice.plan_id = list.id;
    invoice.plan_name = "Usage";
    invoice.currency = "USD";
    invoice.total_micros = 648'000;
    invoice.provider = "manual";
    invoice.lines = {
        {"usage", "gpu", "GPU (L4)", "gpu_seconds", 3600, 0, 3600, 10'800, 60, 648'000, "l4"},
        {"usage",
         "instances",
         "Service and App Instances",
         "instance_seconds",
         60,
         0,
         60,
         1'350,
         60,
         1'350}};
    const auto issued = store.create_invoice(invoice);
    ASSERT_TRUE(issued) << issued.error().message;
    const auto read = store.get_invoice(issued->id);
    ASSERT_TRUE(read) << read.error().message;
    ASSERT_EQ(read->lines.size(), 2U);
    EXPECT_EQ(read->lines[0].class_name, "l4");
    EXPECT_EQ(read->lines[1].class_name, "") << "a line from before lake#643 has no class";
    EXPECT_EQ(read->lines[1].dimension, "instances");
}

} // namespace

// The billing contact's contract (migration 0090, lake#857), on either store: none, set and read
// back, replaced whole, a refused contact changing nothing, an unknown customer not_found.
void billing_contact_contract(lake::control::Store& store, const std::string& customer_id) {
    const auto none = store.billing_contact(customer_id);
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_FALSE(*none);

    lake::control::BillingContact contact{customer_id,
                                          "billing@example.com",
                                          "Contact GmbH",
                                          "Hauptstrasse 1",
                                          "Hof 2",
                                          "Berlin",
                                          "10115",
                                          "Berlin",
                                          "DE",
                                          {}};
    const auto set = store.set_billing_contact(contact);
    ASSERT_TRUE(set) << set.error().message;
    EXPECT_FALSE(set->updated_at.empty());
    const auto read = store.billing_contact(customer_id);
    ASSERT_TRUE(read && *read);
    const lake::control::BillingContact& kept = **read;
    EXPECT_EQ(kept.email, contact.email);
    EXPECT_EQ(kept.name, contact.name);
    EXPECT_EQ(kept.line1, contact.line1);
    EXPECT_EQ(kept.line2, contact.line2);
    EXPECT_EQ(kept.city, contact.city);
    EXPECT_EQ(kept.postal_code, contact.postal_code);
    EXPECT_EQ(kept.state, contact.state);
    EXPECT_EQ(kept.country, contact.country);
    EXPECT_EQ(kept.updated_at, set->updated_at);

    // A save replaces the whole contact: a field left empty is emptied.
    const auto replaced = store.set_billing_contact({.customer_id = customer_id,
                                                     .email = "ap@example.com",
                                                     .name = "Contact AG",
                                                     .country = "CH"});
    ASSERT_TRUE(replaced) << replaced.error().message;
    const auto after = store.billing_contact(customer_id);
    ASSERT_TRUE(after && *after);
    EXPECT_EQ((*after)->email, "ap@example.com");
    EXPECT_EQ((*after)->name, "Contact AG");
    EXPECT_EQ((*after)->line1, "");
    EXPECT_EQ((*after)->city, "");
    EXPECT_EQ((*after)->country, "CH");
    EXPECT_GE((*after)->updated_at, set->updated_at);

    // What billing_contacts' checks refuse is refused, and changes nothing.
    for (const lake::control::BillingContact& refused :
         {lake::control::BillingContact{.customer_id = customer_id, .email = "no-at-sign"},
          lake::control::BillingContact{.customer_id = customer_id, .email = ""},
          lake::control::BillingContact{.customer_id = customer_id, .email = "a b@example.com"},
          lake::control::BillingContact{
              .customer_id = customer_id, .email = "a@example.com", .country = "de"},
          lake::control::BillingContact{.customer_id = customer_id,
                                        .email = "a@example.com",
                                        .city = std::string(129, 'c')}}) {
        const auto refusal = store.set_billing_contact(refused);
        ASSERT_FALSE(refusal) << refused.email << " " << refused.country;
        EXPECT_EQ(refusal.error().code, "invalid_input");
    }
    // Lengths are characters, as PostgreSQL counts them: 256 two-byte characters fit a name.
    std::string umlauts;
    for (int index = 0; index < 256; ++index) {
        umlauts += "\xC3\xA4";
    }
    const auto wide = store.set_billing_contact(
        {.customer_id = customer_id, .email = "ap@example.com", .name = umlauts, .country = "CH"});
    ASSERT_TRUE(wide) << wide.error().message;
    EXPECT_EQ((*store.billing_contact(customer_id))->name, umlauts);

    const std::string unknown = lake::control::new_uuid_v4();
    EXPECT_EQ(store.billing_contact(unknown).error().code, "not_found");
    EXPECT_EQ(
        store.set_billing_contact({.customer_id = unknown, .email = "a@example.com"}).error().code,
        "not_found");
}

// The prepaid balance's contract (migration 0099, lake#1114), on either store: an account starts
// with no row at $0, the row's fields change only against the version read, a credit is recorded
// once per event and once per payment and resets the crossings' marks, a month an invoice froze is
// never replaced by a refresh, and the store's checks refuse what the table refuses.
void account_balance_contract(lake::control::Store& store, const std::string& customer_id) {
    auto fresh = store.account_balance(customer_id);
    ASSERT_TRUE(fresh) << fresh.error().message;
    EXPECT_EQ(fresh->version, 0);
    EXPECT_EQ(fresh->balance_micros(), 0);
    EXPECT_TRUE(fresh->first_top_up_at.empty());
    EXPECT_FALSE(fresh->paid_order);
    EXPECT_FALSE(fresh->auto_top_up) << "auto top-up is off by default";
    EXPECT_EQ(fresh->auto_top_up_threshold_micros, 5'000'000);
    EXPECT_EQ(fresh->auto_top_up_amount_micros, 20'000'000);
    EXPECT_EQ(store.account_balance(lake::control::new_uuid_v4()).error().code, "not_found");

    auto next = *fresh;
    next.overdraft_limit_micros = 75'000'000;
    next.negative_notified_at = "2026-10-24T01:02:03Z";
    next.auto_top_up_pending = "lake-auto-top-up-x-1";
    next.auto_top_up_day = "2026-10-24";
    next.auto_top_ups_today = 1;
    auto written = store.update_account_balance(next, 0);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_GT(written->version, 0);
    EXPECT_EQ(written->overdraft_limit_micros, 75'000'000);
    EXPECT_EQ(written->negative_notified_at.substr(0, 19), "2026-10-24T01:02:03");
    EXPECT_EQ(written->auto_top_up_day, "2026-10-24");
    EXPECT_EQ(store.update_account_balance(next, 0).error().code, "conflict")
        << "a row is there now";
    EXPECT_EQ(store.update_account_balance(next, written->version + 7).error().code, "conflict");
    auto refused = next;
    refused.overdraft_limit_micros = -1;
    EXPECT_EQ(store.update_account_balance(refused, written->version).error().code,
              "invalid_input");

    lake::control::BalanceCredit credit;
    credit.customer_id = customer_id;
    credit.kind = "top_up";
    credit.amount_micros = 20'000'000;
    credit.provider = "stripe";
    credit.reference = "cs_contract_1";
    credit.event_id = "evt_contract_1";
    auto applied = store.apply_balance_credit(credit);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_FALSE(applied->duplicate);
    EXPECT_FALSE(applied->credit.id.empty());
    EXPECT_EQ(applied->balance.balance_micros(), 20'000'000);
    EXPECT_FALSE(applied->balance.first_top_up_at.empty());
    EXPECT_TRUE(applied->balance.negative_notified_at.empty()) << "a credit resets the marks";
    EXPECT_EQ(applied->balance.auto_top_up_pending, "lake-auto-top-up-x-1")
        << "only an auto top-up's credit clears the charge in flight";
    EXPECT_TRUE(store.apply_balance_credit(credit)->duplicate) << "the same event";
    credit.event_id = "evt_contract_1_async";
    EXPECT_TRUE(store.apply_balance_credit(credit)->duplicate) << "the same payment";
    credit.reference = "pi_contract_2";
    credit.event_id = "evt_contract_2";
    credit.kind = "auto_top_up";
    credit.amount_micros = 30'000'000;
    applied = store.apply_balance_credit(credit);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->balance.credits_micros, 50'000'000);
    EXPECT_EQ(applied->balance.credit_count, 2);
    EXPECT_TRUE(applied->balance.auto_top_up_pending.empty());
    credit.amount_micros = 0;
    credit.reference = "pi_contract_3";
    EXPECT_EQ(store.apply_balance_credit(credit).error().code, "invalid_input");
    const auto credits = store.balance_credits(customer_id, 10);
    ASSERT_TRUE(credits) << credits.error().message;
    ASSERT_EQ(credits->size(), 2U);
    EXPECT_EQ((*credits)[0].reference, "pi_contract_2") << "newest first";

    ASSERT_TRUE(store.set_balance_charge({customer_id, "2026-09", 7'000'000, "usage-a", {}, {}}));
    ASSERT_TRUE(store.set_balance_charge({customer_id, "2026-10", 5'000'000, "usage-a", {}, {}}));
    ASSERT_TRUE(store.set_balance_charge({customer_id, "2026-10", 6'000'000, "usage-a", {}, {}}));
    EXPECT_EQ(store.account_balance(customer_id)->charges_micros, 13'000'000);
    lake::control::Invoice invoice;
    invoice.customer_id = customer_id;
    invoice.period = "2026-09";
    invoice.plan_id = "usage-a";
    invoice.plan_name = "Usage";
    invoice.currency = "USD";
    invoice.total_micros = 7'250'000;
    invoice.provider = "balance";
    const auto issued = store.create_invoice(invoice);
    ASSERT_TRUE(issued) << issued.error().message;
    ASSERT_TRUE(
        store.set_balance_charge({customer_id, "2026-09", 7'250'000, "usage-a", issued->id, {}}));
    ASSERT_TRUE(store.set_balance_charge({customer_id, "2026-09", 1, "usage-b", {}, {}}));
    const auto charges = store.balance_charges(customer_id);
    ASSERT_TRUE(charges) << charges.error().message;
    ASSERT_EQ(charges->size(), 2U);
    EXPECT_EQ((*charges)[0].period, "2026-10") << "newest month first";
    EXPECT_EQ((*charges)[1].amount_micros, 7'250'000) << "frozen by its invoice";
    EXPECT_EQ((*charges)[1].invoice_id, issued->id);
    EXPECT_EQ(store.account_balance(customer_id)->balance_micros(), 50'000'000 - 13'250'000);

    const auto listed = store.balance_customers();
    ASSERT_TRUE(listed);
    EXPECT_NE(std::ranges::find(*listed, customer_id), listed->end());
}

TEST(MemoryBilling, AccountBalancesFollowTheContract) {
    lake::control::InMemoryStore store;
    const auto user =
        store.create_user({"taap:1:ed25519:balance", "balance@example.com", "Balance"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", "balance"});
    ASSERT_TRUE(customer) << customer.error().message;
    account_balance_contract(store, customer->id);
}

TEST(PostgresBilling, AccountBalancesFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const char* table : {"account_balances",
                                  "account_balance_credits",
                                  "account_balance_charges",
                                  "account_balances_guard"}) {
            EXPECT_FALSE(
                tx.exec_params("SELECT to_regclass($1)", std::string(table))[0][0].is_null())
                << table;
        }
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0099_account_balances'")
                .empty());
    }
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:balance-" + suffix, "balance-" + suffix + "@example.com", "Balance"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "balance-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    account_balance_contract(*store, customer->id);

    // The tables' own checks hold for a writer other than lake's store.
    pqxx::connection connection(uri);
    for (const char* statement :
         {"UPDATE account_balances SET auto_top_up_amount_micros = 0 WHERE customer_id = $1::uuid",
          "INSERT INTO account_balance_credits (customer_id, kind, amount_micros, provider, "
          "reference) VALUES ($1::uuid, 'gift', 1, 'stripe', 'x')",
          "INSERT INTO account_balance_charges (customer_id, period, amount_micros) VALUES "
          "($1::uuid, '2026-13', 0)"}) {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params(statement, customer->id), pqxx::check_violation) << statement;
    }
}

// Grants from Orama (migration 0100, lake#1134), on either store: a grant is a credit that never
// sets first_top_up_at, records the charges when it was given, is found by id, ends once (revoked
// or expired) with only its unused part forfeited, and a person's email finds their own account.
void balance_grant_contract(lake::control::Store& store,
                            const std::string& customer_id,
                            const std::string& email) {
    lake::control::BalanceCredit grant;
    grant.customer_id = customer_id;
    grant.kind = "grant";
    grant.amount_micros = 25'000'000;
    grant.provider = "orama";
    grant.reference = "grant-contract-1";
    grant.reason = "design partner";
    grant.granted_by = "operator";
    grant.expires_at = "2030-01-01T00:00:00Z";
    ASSERT_TRUE(store.set_balance_charge({customer_id, "2026-09", 3'000'000, "usage-a", {}, {}}));
    auto applied = store.apply_balance_credit(grant);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_FALSE(applied->duplicate);
    EXPECT_EQ(applied->credit.kind, "grant");
    EXPECT_EQ(applied->credit.reason, "design partner");
    EXPECT_EQ(applied->credit.expires_at, "2030-01-01T00:00:00Z");
    EXPECT_EQ(applied->credit.charges_at_grant_micros, 3'000'000);
    EXPECT_TRUE(applied->balance.first_top_up_at.empty()) << "a grant is not a payment";
    EXPECT_FALSE(applied->balance.first_grant_at.empty());
    EXPECT_EQ(applied->balance.charges_since(), applied->balance.first_grant_at);
    EXPECT_EQ(applied->balance.balance_micros(), 22'000'000);
    EXPECT_EQ(applied->balance.grants_unused_micros, 25'000'000)
        << "the $3 charged before it is not its to pay";
    EXPECT_TRUE(store.apply_balance_credit(grant)->duplicate) << "once per reference";
    const std::string id = applied->credit.id;
    const auto found = store.balance_credit(id);
    ASSERT_TRUE(found) << found.error().message;
    EXPECT_EQ(found->customer_id, customer_id);
    EXPECT_EQ(found->granted_by, "operator");
    EXPECT_EQ(store.balance_credit(lake::control::new_uuid_v4()).error().code, "not_found");

    // $10 used after it, then revoked: $15 stops counting.
    ASSERT_TRUE(store.set_balance_charge({customer_id, "2026-10", 10'000'000, "usage-a", {}, {}}));
    EXPECT_EQ(store.account_balance(customer_id)->grants_unused_micros, 15'000'000);
    auto ended = store.end_balance_grants(customer_id, "2026-10-24T12:00:00Z", id);
    ASSERT_TRUE(ended) << ended.error().message;
    ASSERT_EQ(ended->size(), 1U);
    EXPECT_EQ((*ended)[0].end_reason, "revoked");
    EXPECT_EQ((*ended)[0].forfeited_micros, 15'000'000);
    EXPECT_EQ((*ended)[0].charges_at_end_micros, 13'000'000);
    EXPECT_EQ((*ended)[0].ended_at.substr(0, 19), "2026-10-24T12:00:00");
    auto balance = store.account_balance(customer_id);
    ASSERT_TRUE(balance);
    EXPECT_EQ(balance->credits_micros, 10'000'000);
    EXPECT_EQ(balance->balance_micros(), -3'000'000);
    EXPECT_EQ(balance->grants_unused_micros, 0);
    EXPECT_EQ(store.end_balance_grants(customer_id, "2026-10-24T12:00:01Z", id).error().code,
              "conflict")
        << "a grant ends once";

    // An expiring grant ends at its expiry, not before; nothing else ends with it.
    grant.reference = "grant-contract-2";
    grant.expires_at = "2026-11-01T00:00:00Z";
    applied = store.apply_balance_credit(grant);
    ASSERT_TRUE(applied) << applied.error().message;
    ended = store.end_balance_grants(customer_id, "2026-10-31T23:59:59Z", "");
    ASSERT_TRUE(ended);
    EXPECT_TRUE(ended->empty());
    ended = store.end_balance_grants(customer_id, "2026-11-01T00:00:00Z", "");
    ASSERT_TRUE(ended);
    ASSERT_EQ(ended->size(), 1U);
    EXPECT_EQ((*ended)[0].end_reason, "expired");
    EXPECT_EQ((*ended)[0].forfeited_micros, 25'000'000) << "nothing was used after it";
    EXPECT_EQ(store.account_balance(customer_id)->credits_micros, 10'000'000);

    // The store's checks refuse what the table refuses.
    grant.reference = "grant-contract-3";
    grant.amount_micros = lake::control::balance_grant_most_micros + 10'000;
    EXPECT_EQ(store.apply_balance_credit(grant).error().code, "invalid_input");
    grant.amount_micros = 1'000'000;
    grant.reason.clear();
    EXPECT_EQ(store.apply_balance_credit(grant).error().code, "invalid_input");
    auto top_up = grant;
    top_up.kind = "top_up";
    top_up.provider = "stripe";
    top_up.reason = "not a grant";
    EXPECT_EQ(store.apply_balance_credit(top_up).error().code, "invalid_input");

    const auto person = store.personal_account_by_email(email);
    ASSERT_TRUE(person) << person.error().message;
    EXPECT_EQ(person->id, customer_id);
    std::string shouted = email;
    for (char& c : shouted) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    EXPECT_EQ(store.personal_account_by_email(shouted)->id, customer_id);
    EXPECT_EQ(store.personal_account_by_email("nobody@example.com").error().code, "not_found");
    EXPECT_EQ(store.personal_account_by_email("").error().code, "not_found");
}

TEST(MemoryBilling, BalanceGrantsFollowTheContract) {
    lake::control::InMemoryStore store;
    const auto user = store.create_user({"taap:1:ed25519:grant", "grant@example.com", "Grant"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", "grant"});
    ASSERT_TRUE(customer) << customer.error().message;
    balance_grant_contract(store, customer->id, "grant@example.com");
}

TEST(PostgresBilling, BalanceGrantsFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_FALSE(tx.exec("SELECT to_regclass('balance_grants_guard')")[0][0].is_null());
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0100_balance_grants'")
                .empty());
    }
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string email = "grant-" + suffix + "@example.com";
    const auto user = store->create_user({"taap:1:ed25519:grant-" + suffix, email, "Grant"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "grant-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    balance_grant_contract(*store, customer->id, email);

    // The table's own checks hold for a writer other than lake's store; a top-up row is valid as
    // before the migration.
    pqxx::connection connection(uri);
    for (const char* statement :
         {"INSERT INTO account_balance_credits (customer_id, kind, amount_micros, provider, "
          "reference) VALUES ($1::uuid, 'grant', 1, 'orama', 'no-reason')",
          "INSERT INTO account_balance_credits (customer_id, kind, amount_micros, provider, "
          "reference, reason) VALUES ($1::uuid, 'grant', 1, 'stripe', 'not-orama', 'x')",
          "INSERT INTO account_balance_credits (customer_id, kind, amount_micros, provider, "
          "reference, reason) VALUES ($1::uuid, 'grant', 10000000001, 'orama', 'too-much', 'x')",
          "INSERT INTO account_balance_credits (customer_id, kind, amount_micros, provider, "
          "reference, reason) VALUES ($1::uuid, 'top_up', 1, 'stripe', 'with-reason', 'x')",
          "INSERT INTO account_balance_credits (customer_id, kind, amount_micros, provider, "
          "reference, reason, forfeited_micros) VALUES ($1::uuid, 'grant', 1, 'orama', "
          "'forfeit-open', 'x', 1)",
          "UPDATE account_balance_credits SET forfeited_micros = amount_micros + 1 WHERE "
          "customer_id = $1::uuid AND kind = 'grant'"}) {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params(statement, customer->id), pqxx::check_violation) << statement;
    }
    pqxx::work tx(connection);
    EXPECT_NO_THROW(tx.exec_params("INSERT INTO account_balance_credits (customer_id, kind, "
                                   "amount_micros, provider, reference) VALUES ($1::uuid, "
                                   "'top_up', 1, 'stripe', 'plain-top-up')",
                                   customer->id));
}

TEST(MemoryBilling, BillingContactsFollowTheContract) {
    lake::control::InMemoryStore store;
    const auto user =
        store.create_user({"taap:1:ed25519:contact", "contact@example.com", "Contact"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", "contact"});
    ASSERT_TRUE(customer) << customer.error().message;
    billing_contact_contract(store, customer->id);
}

TEST(PostgresBilling, BillingContactsFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const char* table : {"billing_contacts", "billing_contacts_guard"}) {
            EXPECT_FALSE(
                tx.exec_params("SELECT to_regclass($1)", std::string(table))[0][0].is_null())
                << table;
        }
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0090_billing_contacts'")
                .empty());
    }
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:contact-" + suffix, "contact-" + suffix + "@example.com", "Contact"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "contact-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    billing_contact_contract(*store, customer->id);

    // The table's own checks hold for a writer other than lake's store.
    pqxx::connection connection(uri);
    for (const char* values : {"'no-at-sign', 'DE'", "'a@example.com', 'de'"}) {
        pqxx::work tx(connection);
        EXPECT_THROW(
            tx.exec_params(std::string("UPDATE billing_contacts SET (email, country) = (") +
                               values + ") WHERE customer_id = $1::uuid",
                           customer->id),
            pqxx::check_violation)
            << values;
    }
    // The contact goes with its account.
    {
        pqxx::work tx(connection);
        tx.exec_params("DELETE FROM customers WHERE id = $1::uuid", customer->id);
        EXPECT_TRUE(tx.exec_params("SELECT 1 FROM billing_contacts WHERE customer_id = $1::uuid",
                                   customer->id)
                        .empty());
        tx.commit();
    }
}

TEST(PostgresBilling, WorkloadResourcesAndClassRatesFollowTheContract) {
    {
        lake::control::InMemoryStore memory;
        workload_resources_and_classes_follow_the_contract(memory, "mem");
    }
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const char* table : {"workload_resources", "workload_resources_guard"}) {
            EXPECT_FALSE(
                tx.exec_params("SELECT to_regclass($1)", std::string(table))[0][0].is_null())
                << table;
        }
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0075_workload_resources'")
                .empty());
        // The primary key of a price is its plan, dimension and class.
        EXPECT_EQ(
            tx.exec("SELECT array_to_string(array_agg(a.attname ORDER BY k.ord), ',') FROM "
                    "pg_index i CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY k(attnum, "
                    "ord) JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = "
                    "k.attnum WHERE i.indrelid = 'billing_plan_prices'::regclass AND "
                    "i.indisprimary")[0][0]
                .as<std::string>(),
            "plan_id,dimension,class");
        // A class that is not a lower-case name is refused by the schema.
        EXPECT_THROW(tx.exec("INSERT INTO workload_resources (object_kind, object_id, gpu_class) "
                             "VALUES ('app', gen_random_uuid(), 'H100')"),
                     pqxx::check_violation);
    }
    workload_resources_and_classes_follow_the_contract(*store,
                                                       lake::control::new_uuid_v4().substr(0, 8));
}

namespace {

// lake#661 against one store: the operator adds each poll's increase to the row of the app and
// hour, and lake reads the rows from an hour on, oldest first.
void app_traffic_adds_up_by_app_and_hour(lake::control::Store& store) {
    const std::string app = lake::control::new_uuid_v4();
    const std::string other = lake::control::new_uuid_v4();
    const std::string workspace = lake::control::new_uuid_v4();
    ASSERT_TRUE(store.add_app_transfer({app, workspace, "2026-10-11T09:00:00Z", 100, 1000}));
    ASSERT_TRUE(store.add_app_transfer({app, workspace, "2026-10-11T09:00:00Z", 5, 50}));
    ASSERT_TRUE(store.add_app_transfer({app, workspace, "2026-10-11T10:00:00Z", 1, 2}));
    ASSERT_TRUE(store.add_app_transfer({other, workspace, "2026-10-10T23:00:00Z", 7, 7}));
    EXPECT_FALSE(store.add_app_transfer({app, workspace, "2026-10-11T09:30:00Z", 1, 1}))
        << "an hour starts on the hour";
    const auto rows = store.app_transfer("2026-10-11T09:00:00Z");
    ASSERT_TRUE(rows) << rows.error().message;
    std::vector<lake::control::AppTransfer> mine;
    for (const auto& row : *rows) {
        if (row.app_id == app || row.app_id == other) {
            mine.push_back(row);
        }
    }
    ASSERT_EQ(mine.size(), 2U) << "the earlier hour is not read";
    EXPECT_EQ(mine[0].hour, "2026-10-11T09:00:00Z");
    EXPECT_EQ(mine[0].workspace_id, workspace);
    EXPECT_EQ(mine[0].in_bytes, 105U);
    EXPECT_EQ(mine[0].out_bytes, 1050U);
    EXPECT_FALSE(mine[0].observed_at.empty());
    EXPECT_EQ(mine[1].hour, "2026-10-11T10:00:00Z");
    EXPECT_EQ(mine[1].in_bytes, 1U);
}

} // namespace

// lake#661, migration 0081_app_transfer: each app's traffic through the ingress by hour.
TEST(PostgresBilling, AppTrafficAddsUpByAppAndHour) {
    {
        lake::control::InMemoryStore memory;
        app_traffic_adds_up_by_app_and_hour(memory);
    }
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        for (const char* table : {"app_transfer", "app_transfer_guard"}) {
            EXPECT_FALSE(
                tx.exec_params("SELECT to_regclass($1)", std::string(table))[0][0].is_null())
                << table;
        }
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0081_app_transfer'").empty());
    }
    app_traffic_adds_up_by_app_and_hour(*store);
}

// lake#658, migration 0077_metered_jobs: an agent cron run's resources are a workload_resources
// kind, and a cron run's finish time reaches lake so its duration can be charged.
TEST(PostgresBilling, CronRunsAreAWorkloadKindAndTheirEndIsRead) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string run = lake::control::new_uuid_v4();
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_FALSE(
            tx.exec_params("SELECT to_regclass($1)", std::string("metered_jobs_guard"))[0][0]
                .is_null());
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0077_metered_jobs'").empty());
        tx.exec_params("INSERT INTO workload_resources (object_kind, object_id, cpu_millis) "
                       "VALUES ('agent_cron_run', $1::uuid, 250)",
                       run);
        tx.commit();
    }
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec("INSERT INTO workload_resources (object_kind, object_id) "
                             "VALUES ('cron', gen_random_uuid())"),
                     pqxx::check_violation);
    }
    const auto rows = store->workload_resources();
    ASSERT_TRUE(rows) << rows.error().message;
    EXPECT_TRUE(std::ranges::any_of(*rows, [&](const auto& row) {
        return row.object_kind == "agent_cron_run" && row.object_id == run && row.cpu_millis == 250;
    }));
    ASSERT_TRUE(store->put_workload_resources({"agent_cron_run", run, 0, 500, 1024}));

    // A finished cron run answers its end as status_updated_at.
    const auto user = store->create_user(
        {"taap:1:ed25519:cron-" + run, run + "@example.test", "cron-" + run.substr(0, 8)});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        store->create_customer({"individual", user->id, "", "cron-" + run.substr(0, 8)});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto workspace = store->create_workspace({customer->id, "home", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto schedule = store->create_agent_cron_schedule({.customer_id = customer->id,
                                                             .agent_id = "agent-1",
                                                             .agent_name = "Nightly",
                                                             .cron = "0 3 * * *",
                                                             .time_zone = "Etc/UTC",
                                                             .scopes = {"workspace_query"},
                                                             .created_by_user_id = user->id,
                                                             .workspace_id = workspace->id});
    ASSERT_TRUE(schedule) << schedule.error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO agent_cron_runs (id, schedule_id, job_name, phase, started_at, "
                       "finished_at) VALUES ($1::uuid, $2::uuid, 'nightly-1', 'Succeeded', "
                       "'2026-10-11T03:00:00Z', '2026-10-11T03:02:30Z')",
                       run,
                       schedule->id);
        tx.commit();
    }
    const auto runs = store->list_agent_cron_runs(customer->id, "", 10);
    ASSERT_TRUE(runs) << runs.error().message;
    ASSERT_EQ(runs->size(), 1U);
    EXPECT_EQ(runs->front().created_at, "2026-10-11T03:00:00Z");
    EXPECT_EQ(runs->front().status_updated_at, "2026-10-11T03:02:30Z");
}

// Model/direction rates and frozen invoice lines must survive the PostgreSQL constraints.
TEST(PostgresBilling, ModelDirectionPricesAndInvoiceLinesRoundTrip) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping());
    const auto user =
        store->create_user({"taap:1:ed25519:model-billing", "model@example.test", "Model"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "model-billing"});
    ASSERT_TRUE(customer) << customer.error().message;
    lake::control::BillingPlan plan;
    plan.id = "usage-200001-00000000000000001";
    plan.name = "Usage";
    plan.currency = "USD";
    plan.prices = {{"model_tokens", 6, 1, 0, "gpt-5.5/input"},
                   {"model_tokens", 36, 1, 0, "gpt-5.5/output"}};
    const auto kept = store->put_billing_plan(plan);
    ASSERT_TRUE(kept) << kept.error().message;
    ASSERT_EQ(kept->prices.size(), 2U);
    EXPECT_EQ(kept->prices[0].class_name, "gpt-5.5/input");
    EXPECT_EQ(kept->prices[1].class_name, "gpt-5.5/output");
    lake::control::Invoice invoice;
    invoice.customer_id = customer->id;
    invoice.period = "2026-09";
    invoice.plan_id = plan.id;
    invoice.plan_name = plan.name;
    invoice.currency = "USD";
    invoice.provider = "manual";
    invoice.total_micros = 7800;
    invoice.lines = {
        {"usage", "model_tokens", "Input", "tokens", 100, 0, 100, 6, 1, 600, "gpt-5.5/input"},
        {"usage", "model_tokens", "Output", "tokens", 200, 0, 200, 36, 1, 7200, "gpt-5.5/output"}};
    const auto issued = store->create_invoice(invoice);
    ASSERT_TRUE(issued) << issued.error().message;
    ASSERT_EQ(issued->lines.size(), 2U);
    EXPECT_EQ(issued->lines[0].class_name, "gpt-5.5/input");
    EXPECT_EQ(issued->lines[1].class_name, "gpt-5.5/output");
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0095_model_token_prices'")
                .empty());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('model_token_prices_guard')")[0][0].is_null());
        EXPECT_THROW(tx.exec_params("INSERT INTO billing_plan_prices (plan_id, dimension, class, "
                                    "unit_micros) VALUES ($1, 'gpu', 'gpt-5.5/input', 1)",
                                    plan.id),
                     pqxx::check_violation);
    }
}

// Migration 0095 seeds the default gateway models' token prices once, on a fresh and an existing
// control plane alike (docs/api/billing.md#rates). The fixture database is fresh: it already has
// them, in a list for this month. An existing instance is simulated by putting back what it had
// before 0095 (a list in force with its own rates, one for a later month, the migration not yet
// recorded) and running the migration's seed block again, twice.
TEST(PostgresBilling, ModelTokenPricesAreSeededOnceOnFreshAndExistingInstances) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("DO $model_token_prices$");
    ASSERT_NE(start, std::string::npos);
    const std::string end_marker = "$model_token_prices$;";
    const auto end = schema.find(end_marker, start + 1);
    ASSERT_NE(end, std::string::npos);
    const std::string seed = schema.substr(start, end + end_marker.size() - start);
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const std::string month = std::format("{:04}{:02}", int(today.year()), unsigned(today.month()));
    const std::map<std::string, std::int64_t> seeded{{"gpt-5.5/input", 6'500'000},
                                                     {"gpt-5.5/output", 39'000'000},
                                                     {"gpt-5/input", 1'625'000},
                                                     {"gpt-5/output", 13'000'000},
                                                     {"gpt-4o/input", 3'250'000},
                                                     {"gpt-4o/output", 13'000'000},
                                                     {"text-embedding-3-small/input", 26'000},
                                                     {"text-embedding-3-small/output", 26'000}};
    const auto rates = [](pqxx::work& tx, const std::string& plan) {
        std::map<std::string, std::int64_t> out;
        for (const auto& row :
             tx.exec_params("SELECT dimension || ':' || class, unit_micros, per FROM "
                            "billing_plan_prices WHERE plan_id = $1",
                            plan)) {
            out[row[0].as<std::string>()] = row[1].as<std::int64_t>();
            if (row[0].as<std::string>().starts_with("model_tokens:") &&
                row[0].as<std::string>() != "model_tokens:") {
                EXPECT_EQ(row[2].as<std::int64_t>(), 1'000'000) << row[0].as<std::string>();
            }
        }
        return out;
    };
    const auto lists = [](pqxx::work& tx) {
        std::vector<std::string> out;
        for (const auto& row : tx.exec("SELECT id FROM billing_plans ORDER BY id")) {
            out.push_back(row[0].as<std::string>());
        }
        return out;
    };

    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        const auto fresh = lists(tx);
        // A fresh instance gets one list for this month from this migration, and one after it
        // from 0105 (lake#1230), which keeps these rates and adds its own.
        ASSERT_EQ(fresh.size(), 2U) << "0095's list and 0105's, both for this month";
        EXPECT_TRUE(fresh[0].starts_with("usage-" + month + "-")) << fresh[0];
        EXPECT_TRUE(fresh[1].starts_with("usage-" + month + "-")) << fresh[1];
        std::map<std::string, std::int64_t> expected;
        for (const auto& [name, micros] : seeded) {
            expected["model_tokens:" + name] = micros;
        }
        EXPECT_EQ(rates(tx, fresh[0]), expected);
        for (const auto& [name, micros] : expected) {
            EXPECT_EQ(rates(tx, fresh[1]).at(name), micros) << name;
        }
        tx.commit();
    }
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM billing_plans");
        tx.exec("DELETE FROM schema_migrations WHERE version = '0095_model_token_prices'");
        const std::string current = "usage-" + month + "-00000000000000001";
        tx.exec_params(
            "INSERT INTO billing_plans (id, name, currency) VALUES ($1, 'Usage', 'USD'), "
            "('usage-209912-00000000000000001', 'Usage', 'USD')",
            current);
        tx.exec_params(
            "INSERT INTO billing_plan_prices (plan_id, dimension, class, unit_micros, per) VALUES "
            "($1, 'calls', '', 2000000, 1000000), ($1, 'model_tokens', '', 9000000, 1000000), "
            "($1, 'model_tokens', 'gpt-5/input', 1700000, 1000000), "
            "('usage-209912-00000000000000001', 'storage', '', 40000, 1073741824)",
            current);
        tx.exec(seed);
        tx.exec("INSERT INTO schema_migrations (version) VALUES ('0095_model_token_prices')");
        tx.exec(seed); // a later schema application seeds nothing more
        tx.commit();
    }
    {
        pqxx::work tx(connection);
        const auto after = lists(tx);
        ASSERT_EQ(after.size(), 3U);
        const std::string current = "usage-" + month + "-00000000000000001";
        EXPECT_EQ(after[0], current);
        EXPECT_TRUE(after[1].starts_with("usage-" + month + "-")) << after[1];
        EXPECT_GT(after[1], current) << "the new publication is the one in force";
        EXPECT_EQ(after[2], "usage-209912-00000000000000001");
        // The published list is kept as it was.
        EXPECT_EQ(rates(tx, current),
                  (std::map<std::string, std::int64_t>{{"calls:", 2'000'000},
                                                       {"model_tokens:", 9'000'000},
                                                       {"model_tokens:gpt-5/input", 1'700'000}}));
        // The new one: its rates, the operator's own model rate kept, the seeded ones added.
        std::map<std::string, std::int64_t> in_force{{"calls:", 2'000'000},
                                                     {"model_tokens:", 9'000'000}};
        std::map<std::string, std::int64_t> later{{"storage:", 40'000}};
        for (const auto& [name, micros] : seeded) {
            in_force["model_tokens:" + name] = micros;
            later["model_tokens:" + name] = micros;
        }
        in_force["model_tokens:gpt-5/input"] = 1'700'000;
        EXPECT_EQ(rates(tx, after[1]), in_force);
        EXPECT_EQ(rates(tx, after[2]), later);
    }
}

// Migration 0105 (lake#1230) seeds the gpt-6.x models' token prices and the long-context rates
// once, on a fresh and an existing control plane alike, by 0095's rules: a new publication for this
// month with the rates of the list in force, an operator's own rate kept, a later month's list
// given the new rates, a list in another currency left alone, and nothing more on a later run.
TEST(PostgresBilling, AgentModelPricesAreSeededOnceOnFreshAndExistingInstances) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    std::ifstream file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("DO $agent_model_prices$");
    ASSERT_NE(start, std::string::npos);
    const std::string end_marker = "$agent_model_prices$;";
    const auto end = schema.find(end_marker, start + 1);
    ASSERT_NE(end, std::string::npos);
    const std::string seed = schema.substr(start, end + end_marker.size() - start);
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const std::string month = std::format("{:04}{:02}", int(today.year()), unsigned(today.month()));
    // OpenAI's standard price read 2026-10-26, plus 30%; long: 2x input and 1.5x output, plus 30%.
    const std::map<std::string, std::int64_t> seeded{{"gpt-6.1-sol/input", 2'600'000},
                                                     {"gpt-6.1-sol/output", 13'000'000},
                                                     {"gpt-6.1-sol@long/input", 5'200'000},
                                                     {"gpt-6.1-sol@long/output", 19'500'000},
                                                     {"gpt-6.1-terra/input", 1'300'000},
                                                     {"gpt-6.1-terra/output", 6'500'000},
                                                     {"gpt-6.1-terra@long/input", 2'600'000},
                                                     {"gpt-6.1-terra@long/output", 9'750'000},
                                                     {"gpt-6-astra/input", 13'000'000},
                                                     {"gpt-6-astra/output", 65'000'000},
                                                     {"gpt-6-astra@long/input", 26'000'000},
                                                     {"gpt-6-astra@long/output", 97'500'000},
                                                     {"gpt-6-luna/input", 130'000},
                                                     {"gpt-6-luna/output", 650'000},
                                                     {"gpt-6-luna@long/input", 260'000},
                                                     {"gpt-6-luna@long/output", 975'000},
                                                     {"gpt-5.5@long/input", 13'000'000},
                                                     {"gpt-5.5@long/output", 58'500'000}};
    const auto rates = [](pqxx::work& tx, const std::string& plan) {
        std::map<std::string, std::int64_t> out;
        for (const auto& row :
             tx.exec_params("SELECT dimension || ':' || class, unit_micros, per FROM "
                            "billing_plan_prices WHERE plan_id = $1",
                            plan)) {
            out[row[0].as<std::string>()] = row[1].as<std::int64_t>();
            if (row[0].as<std::string>().starts_with("model_tokens:") &&
                row[0].as<std::string>() != "model_tokens:") {
                EXPECT_EQ(row[2].as<std::int64_t>(), 1'000'000) << row[0].as<std::string>();
            }
        }
        return out;
    };
    const auto lists = [](pqxx::work& tx) {
        std::vector<std::string> out;
        for (const auto& row : tx.exec("SELECT id FROM billing_plans ORDER BY id")) {
            out.push_back(row[0].as<std::string>());
        }
        return out;
    };

    pqxx::connection connection(uri);
    {
        // Fresh: the newest list of this month has 0095's rates and these.
        pqxx::work tx(connection);
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0105_agent_model_prices'")
                .empty());
        EXPECT_FALSE(tx.exec("SELECT to_regclass('agent_model_prices_guard')")[0][0].is_null());
        const auto fresh = lists(tx);
        ASSERT_FALSE(fresh.empty());
        const auto newest = rates(tx, fresh.back());
        EXPECT_TRUE(fresh.back().starts_with("usage-" + month + "-")) << fresh.back();
        for (const auto& [name, micros] : seeded) {
            ASSERT_TRUE(newest.contains("model_tokens:" + name)) << name;
            EXPECT_EQ(newest.at("model_tokens:" + name), micros) << name;
        }
        EXPECT_EQ(newest.at("model_tokens:gpt-5.5/input"), 6'500'000) << "0095's rate stays";
        tx.commit();
    }
    {
        // Existing: a list in force with an operator's own gpt-6.1-sol input rate, a later USD
        // list, a later EUR list; the migration not yet recorded. Run twice, recorded between.
        pqxx::work tx(connection);
        tx.exec("DELETE FROM billing_plans");
        tx.exec("DELETE FROM schema_migrations WHERE version = '0105_agent_model_prices'");
        const std::string current = "usage-" + month + "-00000000000000001";
        tx.exec_params(
            "INSERT INTO billing_plans (id, name, currency) VALUES ($1, 'Usage', 'USD'), "
            "('usage-209912-00000000000000001', 'Usage', 'USD'), "
            "('usage-209912-00000000000000002', 'Usage', 'EUR')",
            current);
        tx.exec_params(
            "INSERT INTO billing_plan_prices (plan_id, dimension, class, unit_micros, per) VALUES "
            "($1, 'calls', '', 2000000, 1000000), "
            "($1, 'model_tokens', 'gpt-5.5/input', 6500000, 1000000), "
            "($1, 'model_tokens', 'gpt-6.1-sol/input', 3000000, 1000000)",
            current);
        tx.exec(seed);
        tx.exec("INSERT INTO schema_migrations (version) VALUES ('0105_agent_model_prices')");
        tx.exec(seed); // a later schema application seeds nothing more
        tx.commit();
    }
    {
        pqxx::work tx(connection);
        const auto after = lists(tx);
        ASSERT_EQ(after.size(), 4U);
        const std::string current = "usage-" + month + "-00000000000000001";
        EXPECT_EQ(after[0], current);
        EXPECT_TRUE(after[1].starts_with("usage-" + month + "-")) << after[1];
        EXPECT_GT(after[1], current) << "the new publication is the one in force";
        // The published list is kept as it was.
        EXPECT_EQ(
            rates(tx, current),
            (std::map<std::string, std::int64_t>{{"calls:", 2'000'000},
                                                 {"model_tokens:gpt-5.5/input", 6'500'000},
                                                 {"model_tokens:gpt-6.1-sol/input", 3'000'000}}));
        std::map<std::string, std::int64_t> in_force{{"calls:", 2'000'000},
                                                     {"model_tokens:gpt-5.5/input", 6'500'000}};
        std::map<std::string, std::int64_t> later;
        for (const auto& [name, micros] : seeded) {
            in_force["model_tokens:" + name] = micros;
            later["model_tokens:" + name] = micros;
        }
        in_force["model_tokens:gpt-6.1-sol/input"] = 3'000'000; // the operator's own, kept
        EXPECT_EQ(rates(tx, after[1]), in_force);
        EXPECT_EQ(rates(tx, "usage-209912-00000000000000001"), later);
        EXPECT_TRUE(rates(tx, "usage-209912-00000000000000002").empty())
            << "a list in another currency is left for the operator";
    }
}
