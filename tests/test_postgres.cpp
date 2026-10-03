#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
                                       {"role", "owner"},
                                       {"attestation_digest", "sha512-256:owner-" + suffix}});
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
                                     {"principal_taap_id", second_user.value.at("taap_id")},
                                     {"attestation_digest", "sha512-256:accepted-" + suffix}});
    ASSERT_TRUE(accepted.ok) << accepted.error.message;

    const auto repository_object =
        store->find_policy_object("repository", repository.value.at("id"));
    ASSERT_TRUE(repository_object) << repository_object.error().message;
    const ApiResult path = call("create_object",
                                {{"object_type", "path"},
                                 {"external_id", "node-" + suffix},
                                 {"parent_id", repository_object->id},
                                 {"inherit", true}});
    ASSERT_TRUE(path.ok) << path.error.message;
    ASSERT_TRUE(call("get_object", {{"id", path.value.at("id")}}).ok);
    ASSERT_TRUE(call("list_objects", {{"parent_id", repository_object->id}}).ok);

    const ApiResult grant = call("create_grant",
                                 {{"principal_taap_id", "taap:1:ed25519:reader-" + suffix},
                                  {"object_id", repository_object->id},
                                  {"role", "read"},
                                  {"attestation_digest", "sha512-256:grant-" + suffix}});
    ASSERT_TRUE(grant.ok) << grant.error.message;
    ASSERT_TRUE(call("list_grants", {{"object_id", repository_object->id}}).ok);
    const ApiResult access = call("effective_access", {{"object_id", path.value.at("id")}});
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

// Migration 0048 (lake#350): each retired drive_share_links row gets the owner it is copied into
// and, once assigned, a content-free route from the token digest a pre-move token presents.
TEST(PostgresDriveShareLinks, MigrationRecordsOwnersAndContentFreeRoutes) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> repositories, folders, workspaces;
    for (int index = 0; index < 2; ++index) {
        const std::string name = "drive-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:1:ed25519:" + name, "", "Drive"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        // The second space has two workspaces, so its links cannot be assigned by themselves.
        std::string first_workspace;
        for (int count = 0; count <= index; ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            if (count == 0) {
                first_workspace = workspace->id;
            }
            if (index == 0) {
                workspaces.push_back(workspace->id);
            }
        }
        const auto repository =
            store->create_repository({customer->id, "drive", "", "private", first_workspace});
        ASSERT_TRUE(repository) << repository.error().message;
        const auto parent = store->find_policy_object("repository", repository->id);
        ASSERT_TRUE(parent);
        const auto folder = store->create_policy_object(
            {"path", repository->id + ":" + suffix + "000000000000000000000000", parent->id,
             user->taap_id, "request-folder-" + name, true});
        ASSERT_TRUE(folder) << folder.error().message;
        repositories.push_back(repository->id);
        folders.push_back(folder->id);
    }
    pqxx::connection connection(uri);
    const auto insert = [&](int space, const std::string& token_hash) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO drive_share_links (repository_id, object_id, node_id, "
                           "path, role, token_hash, expires_at, created_by_taap_id) VALUES "
                           "($1::uuid, $2::uuid, $3, 'shared', 'read', $4, "
                           "'2999-01-01T00:00:00Z', 'taap:1:ed25519:creator') RETURNING id::text",
                           repositories[space], folders[space],
                           suffix + "000000000000000000000000", token_hash)[0][0]
                .as<std::string>();
        tx.commit();
        return id;
    };
    const std::string owned = insert(0, "owned-" + suffix);
    const std::string ambiguous = insert(1, "ambiguous-" + suffix);
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0048_drive_share_link_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0048_drive_share_link_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'drive_share_links' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(owned),
                  (std::pair<std::string, std::string>{"sole_workspace", workspaces[0]}));
        EXPECT_EQ(outcome(ambiguous).first, "needs_selection");
    }
    const auto route = store->drive_share_link_route("owned-" + suffix);
    ASSERT_TRUE(route) << route.error().message;
    EXPECT_EQ(route->link_id, owned);
    EXPECT_EQ(route->workspace_id, workspaces[0]);
    EXPECT_EQ(store->drive_share_link_route("ambiguous-" + suffix).error().code, "not_found")
        << "an unassigned link has no route until its workspace is chosen";
    {
        // The route is dispatch only: no path, role, expiry or creator.
        pqxx::work tx(connection);
        const auto columns = tx.exec(
            "SELECT column_name FROM information_schema.columns WHERE table_name = "
            "'drive_share_link_routes' AND table_schema = current_schema() ORDER BY column_name");
        std::vector<std::string> names;
        for (const auto& row : columns) {
            names.push_back(row[0].as<std::string>());
        }
        EXPECT_EQ(names, (std::vector<std::string>{"link_id", "recorded_at", "token_hash",
                                                   "workspace_id"}));
    }

    // A row written after the migration, and an operator's reviewed choice, are picked up by the
    // assignment each copy reruns.
    const std::string late = insert(0, "late-" + suffix);
    EXPECT_TRUE(outcome(late).first.empty());
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspace_assignments SET workspace_id = w.id FROM workspaces w "
                       "JOIN repositories r ON r.customer_id = w.customer_id WHERE "
                       "object_table = 'drive_share_links' AND object_id = $1::uuid AND "
                       "r.id = $2::uuid AND w.name = 'ws-1'",
                       ambiguous, repositories[1]);
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_drive_share_links());
    EXPECT_EQ(outcome(late).first, "sole_workspace");
    EXPECT_EQ(store->drive_share_link_route("late-" + suffix)->link_id, late);
    EXPECT_EQ(store->drive_share_link_route("ambiguous-" + suffix)->link_id, ambiguous);

    std::map<std::string, lake::control::LegacyDriveShareLink> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_drive_share_links(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            listed.emplace(row.link.id, row);
        }
        after = page->back().link.id;
    }
    ASSERT_TRUE(listed.contains(owned));
    EXPECT_EQ(listed.at(owned).workspace_id, workspaces[0]);
    EXPECT_EQ(listed.at(owned).link.expires_at, "2999-01-01T00:00:00.000000Z");
    EXPECT_EQ(listed.at(owned).link.token_hash, "owned-" + suffix);
    EXPECT_GT(listed.at(owned).link.at_us, 0);
    EXPECT_FALSE(listed.at(owned).customer_id.empty());
    EXPECT_FALSE(listed.at(ambiguous).workspace_id.empty());
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

TEST(PostgresForgeRecords, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user({"taap:1:ed25519:forge-migration-" + suffix, "", "Forge"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        store->create_customer({"individual", user->id, "", "forge-migration-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto live = store->create_workspace({customer->id, "live", "dev", ""});
    const auto doomed = store->create_workspace({customer->id, "doomed", "dev", ""});
    ASSERT_TRUE(live && doomed);
    const auto owned = store->create_repository({customer->id, "owned", "", "private", live->id});
    const auto orphan = store->create_repository({customer->id, "orphan", "", "private", live->id});
    const auto gone = store->create_repository({customer->id, "gone", "", "private", doomed->id});
    ASSERT_TRUE(owned && orphan && gone);
    pqxx::connection connection(uri);
    {
        // The orphan stands for a repository migration 0051 could not assign.
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = NULL WHERE id = $1::uuid",
                       orphan->id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.commit();
    }
    ASSERT_TRUE(
        store->delete_repository(gone->id, {"2026-10-01T00:00:00Z", "2027-10-01T00:00:00Z"}));
    ASSERT_TRUE(store->update_workspace(doomed->id, {.desired = "deleted"}));
    const auto insert = [&](const std::string& repository_id, const std::string& kind) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO forge_records (repository_id, kind, value) VALUES "
                           "($1::uuid, $2, '{\"title\":\"Migrated\"}'::jsonb) RETURNING id::text",
                           repository_id, kind)[0][0]
                .as<std::string>();
        tx.commit();
        return id;
    };
    const std::string issue = insert(owned->id, "issue");
    const std::string waiting = insert(orphan->id, "review");
    const std::string released = insert(gone->id, "release");
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0054_forge_record_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0054_forge_record_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'forge_records' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(issue),
                  (std::pair<std::string, std::string>{"repository_workspace", live->id}));
        EXPECT_EQ(outcome(waiting), (std::pair<std::string, std::string>{"needs_selection", ""}));
        EXPECT_EQ(outcome(released),
                  (std::pair<std::string, std::string>{"workspace_deleted", ""}));
    }

    // A row written after the migration, and a repository assigned after review, are picked up by
    // the assignment each copy reruns.
    const std::string late = insert(owned->id, "issue_comment");
    EXPECT_TRUE(outcome(late).first.empty());
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       orphan->id, live->id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_forge_records());
    EXPECT_EQ(outcome(late),
              (std::pair<std::string, std::string>{"repository_workspace", live->id}));
    EXPECT_EQ(outcome(waiting),
              (std::pair<std::string, std::string>{"repository_workspace", live->id}));
    EXPECT_EQ(outcome(released).first, "workspace_deleted");

    std::map<std::string, lake::control::LegacyForgeRecord> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_forge_records(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            listed.emplace(row.record.id, row);
        }
        after = page->back().record.id;
    }
    ASSERT_TRUE(listed.contains(issue));
    EXPECT_EQ(listed.at(issue).workspace_id, live->id);
    EXPECT_EQ(listed.at(issue).customer_id, customer->id);
    EXPECT_EQ(listed.at(issue).record.kind, "issue");
    EXPECT_EQ(listed.at(issue).record.value["title"], "Migrated");
    EXPECT_GT(listed.at(issue).record.at_us, 0);
    EXPECT_EQ(listed.at(released).outcome, "workspace_deleted");
    EXPECT_TRUE(listed.at(released).workspace_id.empty());
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

// Migration 0053 (lake#348, lake#349): metering rows of a space with one live workspace are owned
// by it, every other row is needs_selection; usage rows without transfer are not copied. A rerun
// changes nothing, the store's rerun picks up late rows, lake and SQL compute the same row ids,
// and a workspace deleted after assignment no longer receives its rows.
TEST(PostgresMetering, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "metering-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    for (int index = 0; index < 2; ++index) {
        const std::string name = "meter-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
    }
    // The first space has one live workspace (and a deleted one); the second has two.
    const auto sole = store->create_workspace({customers[0], "home", "dev", ""});
    ASSERT_TRUE(sole) << sole.error().message;
    const auto old = store->create_workspace({customers[0], "old", "dev", ""});
    ASSERT_TRUE(old);
    ASSERT_TRUE(store->update_workspace(old->id, {.desired = "deleted"}));
    ASSERT_TRUE(store->create_workspace({customers[1], "a", "dev", ""}));
    ASSERT_TRUE(store->create_workspace({customers[1], "b", "dev", ""}));
    pqxx::connection connection(uri);
    const auto usage = [&](const std::string& customer,
                           const std::string& hour,
                           std::uint64_t in,
                           std::uint64_t out) {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO metering_usage (customer_id, hour, stored_bytes, "
                       "transfer_in_bytes, transfer_out_bytes, compute_seconds) VALUES "
                       "($1::uuid, $2::timestamptz, 4096, $3, $4, 7)",
                       customer,
                       hour,
                       in,
                       out);
        tx.commit();
        return lake::control::legacy_metering_usage_id(customer, hour);
    };
    const auto build = [&](const std::string& customer, const std::string& result) {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO metering_build_records (customer_id, result_id, hour, "
                       "compute_seconds) VALUES ($1::uuid, $2, '2026-09-04T10:00:00Z', 3)",
                       customer,
                       result);
        tx.commit();
        return lake::control::legacy_metered_build_id(customer, result);
    };
    const std::string owned = usage(customers[0], "2026-09-04T10:00:00Z", 10, 20);
    const std::string gauge_only = usage(customers[0], "2026-09-04T11:00:00Z", 0, 0);
    const std::string ambiguous = usage(customers[1], "2026-09-04T10:00:00Z", 5, 0);
    const std::string owned_build = build(customers[0], "result-a-" + suffix);
    const std::string ambiguous_build = build(customers[1], "result-b-" + suffix);
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0053_metering_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0053_metering_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& table, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = $1 AND object_id = $2::uuid",
            table,
            id);
        return found.empty()
                   ? std::pair<std::string, std::string>{}
                   : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    using Outcome = std::pair<std::string, std::string>;
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome("metering_usage", owned), (Outcome{"sole_workspace", sole->id}));
        EXPECT_TRUE(outcome("metering_usage", gauge_only).first.empty())
            << "a row without transfer has nothing to copy";
        EXPECT_EQ(outcome("metering_usage", ambiguous), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome("metering_build_records", owned_build),
                  (Outcome{"sole_workspace", sole->id}));
        EXPECT_EQ(outcome("metering_build_records", ambiguous_build),
                  (Outcome{"needs_selection", ""}));
    }
    const std::string late = usage(customers[0], "2026-09-04T12:00:00Z", 1, 1);
    const std::string late_build = build(customers[0], "result-late-" + suffix);
    EXPECT_TRUE(outcome("metering_usage", late).first.empty());
    ASSERT_TRUE(store->assign_legacy_metering());
    EXPECT_EQ(outcome("metering_usage", late).first, "sole_workspace") << "a row written later";
    EXPECT_EQ(outcome("metering_build_records", late_build).first, "sole_workspace");

    std::map<std::string, lake::control::LegacyMeteringUsage> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_metering_usage(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            EXPECT_GT(row.id, after) << "pages are ordered by id";
            listed.emplace(row.id, row);
        }
        after = page->back().id;
    }
    ASSERT_TRUE(listed.contains(owned)) << "lake and the migration compute the same id";
    EXPECT_FALSE(listed.contains(gauge_only));
    EXPECT_EQ(listed.at(owned).workspace_id, sole->id);
    EXPECT_EQ(listed.at(owned).usage.hour, "2026-09-04T10:00:00Z");
    EXPECT_EQ(listed.at(owned).usage.transfer_in_bytes, 10U);
    EXPECT_EQ(listed.at(owned).usage.transfer_out_bytes, 20U);
    EXPECT_EQ(listed.at(ambiguous).outcome, "needs_selection");
    const auto builds = store->legacy_metering_build_records("", 5000);
    ASSERT_TRUE(builds) << builds.error().message;
    const auto found_build =
        std::ranges::find(*builds, owned_build, &lake::control::LegacyMeteredBuild::id);
    ASSERT_NE(found_build, builds->end());
    EXPECT_EQ(found_build->build.result_id, "result-a-" + suffix);
    EXPECT_EQ(found_build->build.hour, "2026-09-04T10:00:00Z");
    EXPECT_EQ(found_build->build.compute_seconds, 3U);

    // A workspace deleted after its rows were assigned no longer receives them.
    ASSERT_TRUE(store->update_workspace(sole->id, {.desired = "deleted"}));
    const auto relisted = store->legacy_metering_usage("", 5000);
    ASSERT_TRUE(relisted);
    const auto row = std::ranges::find(*relisted, owned, &lake::control::LegacyMeteringUsage::id);
    ASSERT_NE(row, relisted->end());
    EXPECT_EQ(row->outcome, "workspace_deleted");
    EXPECT_TRUE(row->workspace_id.empty());
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
        EXPECT_EQ(tx.exec_params("SELECT count(*) FROM secrets_mounts WHERE id = $1::uuid",
                                 mount_id)[0][0]
                      .as<int>(),
                  0);
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
            EXPECT_EQ(tx.exec_params("SELECT count(*) FROM " + table +
                                         " WHERE mount_id = $1::uuid",
                                     mount_id)[0][0]
                          .as<int>(),
                      0)
                << table;
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

// lake#328-#332: migration 0057 gives each legacy mount the only live workspace of its space and
// a route there, reports the rest, and drops the dynamic leases' foreign key to secrets_mounts;
// the mount's entries, versions, keys and data keys follow its route.
TEST(PostgresSecrets, MigrationRecordsEachMountsOwnerAndRoute) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    struct Space {
        std::string organization_id, customer_id;
        std::vector<std::string> workspaces;
    };
    const auto space = [&](const std::string& name, int workspaces) {
        const auto organization = store->create_organization(
            {"taap:1:ed25519:secret-migration-" + name + "-" + suffix, name, ""});
        EXPECT_TRUE(organization) << organization.error().message;
        const auto customer = store->create_customer(
            {"organization", "", organization->id, "secret-migration-" + name + "-" + suffix});
        EXPECT_TRUE(customer) << customer.error().message;
        Space created{organization->id, customer->id, {}};
        for (int index = 0; index < workspaces; ++index) {
            const auto workspace =
                store->create_workspace({customer->id, name + std::to_string(index), "dev", ""});
            EXPECT_TRUE(workspace) << workspace.error().message;
            created.workspaces.push_back(workspace->id);
        }
        return created;
    };
    const Space sole = space("sole", 1);
    const Space several = space("several", 2);
    const Space doomed = space("doomed", 1);
    pqxx::connection connection(uri);
    const auto mount = [&](const Space& owner, const std::string& name) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO secrets_mounts (customer_id, organization_id, name) "
                           "VALUES ($1::uuid, $2::uuid, $3) RETURNING id::text",
                           owner.customer_id, owner.organization_id, name)[0][0]
                .as<std::string>();
        tx.commit();
        return id;
    };
    const std::string owned = mount(sole, "owned");
    const std::string waiting = mount(several, "waiting");
    const std::string gone = mount(doomed, "gone");
    std::string entry, key;
    {
        pqxx::work tx(connection);
        entry = tx.exec_params("INSERT INTO secrets_entries (mount_id, name, current_version) "
                               "VALUES ($1::uuid, 'database', 1) RETURNING id::text",
                               owned)[0][0]
                    .as<std::string>();
        tx.exec_params("INSERT INTO secrets_versions (entry_id, version, ciphertext) VALUES "
                       "($1::uuid, 1, 'sealed')",
                       entry);
        key = tx.exec_params("INSERT INTO secrets_encryption_keys (mount_id, name) VALUES "
                             "($1::uuid, 'orders') RETURNING id::text",
                             owned)[0][0]
                  .as<std::string>();
        tx.exec_params("INSERT INTO secrets_data_keys (customer_id, owner_kind, owner_id, "
                       "key_version, wrapped_key) VALUES ($1::uuid, 'mount', $2::uuid, 1, 'w'), "
                       "($1::uuid, 'encryption', $3::uuid, 1, 'w')",
                       sole.customer_id, owned, key);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0057_secret_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0057_secret_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'secrets_mounts' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    const auto route = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT workspace_id::text FROM secret_mount_routes WHERE mount_id = $1::uuid", id);
        return found.empty() ? std::string() : found[0][0].as<std::string>();
    };
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(owned),
                  (std::pair<std::string, std::string>{"sole_workspace", sole.workspaces[0]}));
        EXPECT_EQ(outcome(waiting), (std::pair<std::string, std::string>{"needs_selection", ""}));
        EXPECT_EQ(route(owned), sole.workspaces[0]);
        EXPECT_EQ(route(gone), doomed.workspaces[0]);
        EXPECT_TRUE(route(waiting).empty());
    }
    {
        pqxx::work tx(connection);
        EXPECT_TRUE(tx.exec("SELECT 1 FROM information_schema.table_constraints WHERE "
                            "constraint_name = 'secrets_dynamic_leases_mount_id_fkey' AND "
                            "table_schema = current_schema()")
                        .empty());
        // A route names a live workspace of its own space and never moves.
        EXPECT_THROW(tx.exec_params("UPDATE secret_mount_routes SET workspace_id = $2::uuid "
                                    "WHERE mount_id = $1::uuid",
                                    owned, several.workspaces[0]),
                     pqxx::check_violation);
    }

    // The deleted workspace's mount went with it; a reviewed choice routes the waiting one; a
    // mount written after the migration is picked up by the assignment each copy reruns.
    ASSERT_TRUE(store->update_workspace(doomed.workspaces[0], {.desired = "deleted"}));
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE workspace_assignments SET workspace_id = $2::uuid WHERE "
                       "object_table = 'secrets_mounts' AND object_id = $1::uuid",
                       waiting, several.workspaces[1]);
        tx.commit();
    }
    const std::string late = mount(sole, "late");
    ASSERT_TRUE(store->assign_legacy_secrets());
    EXPECT_EQ(route(waiting), several.workspaces[1]);
    EXPECT_EQ(route(late), sole.workspaces[0]);

    std::map<std::string, lake::control::LegacySecretMount> mounts;
    std::string after;
    for (;;) {
        const auto page = store->legacy_secret_mounts(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            mounts.emplace(row.mount.id, row);
        }
        after = page->back().mount.id;
    }
    ASSERT_TRUE(mounts.contains(owned) && mounts.contains(waiting) && mounts.contains(gone));
    EXPECT_EQ(mounts.at(owned).owner.workspace_id, sole.workspaces[0]);
    EXPECT_EQ(mounts.at(owned).mount.name, "owned");
    EXPECT_EQ(mounts.at(waiting).owner.workspace_id, several.workspaces[1]);
    EXPECT_EQ(mounts.at(gone).owner.outcome, "workspace_deleted");
    EXPECT_TRUE(mounts.at(gone).owner.workspace_id.empty());

    const auto entries = store->legacy_secret_entries("", 5000);
    ASSERT_TRUE(entries) << entries.error().message;
    const auto found_entry = std::ranges::find(*entries, entry, &lake::control::LegacySecretEntry::id);
    ASSERT_NE(found_entry, entries->end());
    EXPECT_EQ(found_entry->owner.workspace_id, sole.workspaces[0]);
    EXPECT_EQ(found_entry->owner.mount_id, owned);
    const auto versions = store->legacy_secret_versions("", 5000);
    ASSERT_TRUE(versions) << versions.error().message;
    const auto found_version = std::ranges::find_if(*versions, [&](const auto& row) {
        return row.owner.mount_id == owned;
    });
    ASSERT_NE(found_version, versions->end());
    EXPECT_EQ(found_version->ciphertext, "sealed");
    EXPECT_EQ(found_version->owner.workspace_id, sole.workspaces[0]);
    const auto keys = store->legacy_encryption_keys("", 5000);
    ASSERT_TRUE(keys) << keys.error().message;
    EXPECT_TRUE(std::ranges::any_of(*keys, [&](const auto& row) {
        return row.key.id == key && row.owner.workspace_id == sole.workspaces[0];
    }));
    const auto data_keys = store->legacy_secret_data_keys("", 5000);
    ASSERT_TRUE(data_keys) << data_keys.error().message;
    EXPECT_EQ(std::ranges::count_if(*data_keys,
                                    [&](const auto& row) {
                                        return row.owner.mount_id == owned &&
                                               row.owner.workspace_id == sole.workspaces[0];
                                    }),
              2)
        << "a named key's data key follows its key's mount";
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
    // A legacy lease of a mount without a route is reported by the migration and left alone.
    {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO secrets_dynamic_leases (mount_id, role, encrypted_value, "
                       "expires_at) VALUES ($1::uuid, 'reader', 'sealed', now() + interval '1 "
                       "hour')",
                       lake::control::new_uuid_v4());
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
    // The legacy listing pages by lease id and names each lease's mount route as its owner.
    {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO secrets_dynamic_leases (id, mount_id, role, encrypted_value, "
                       "expires_at, revoked_at, renewals) VALUES ($1::uuid, $2::uuid, 'writer', "
                       "'sealed-legacy', now() + interval '1 hour', now(), 4)",
                       future.lease_id, mount);
        tx.commit();
    }
    std::map<std::string, lake::control::LegacyDynamicSecretLease> legacy;
    std::string after;
    for (;;) {
        const auto page = store->legacy_dynamic_secret_leases(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            legacy.emplace(row.id, row);
        }
        after = page->back().id;
    }
    ASSERT_TRUE(legacy.contains(future.lease_id));
    const auto& row = legacy.at(future.lease_id);
    EXPECT_EQ(row.owner.workspace_id, workspaces[0]);
    EXPECT_EQ(row.owner.customer_id, customer->id);
    EXPECT_EQ(row.encrypted_value, "sealed-legacy");
    EXPECT_EQ(row.renewals, 4);
    EXPECT_FALSE(row.revoked_at.empty());
    EXPECT_GT(row.revoked_us, 0);
    EXPECT_GT(row.expires_us, row.at_us);
}

// lake#358, lake#359: migration 0060 gives each environment's policy object the environment's
// derived id, records each platform_environments and platform_mirror row's owner, its repository's
// workspace, and the copy's rerun of it picks up late rows and repositories assigned after review.
TEST(PostgresPlatform, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user =
        store->create_user({"taap:1:ed25519:platform-migration-" + suffix, "", "Platform"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        store->create_customer({"individual", user->id, "", "platform-migration-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto live = store->create_workspace({customer->id, "live", "dev", ""});
    const auto doomed = store->create_workspace({customer->id, "doomed", "dev", ""});
    ASSERT_TRUE(live && doomed);
    const auto owned = store->create_repository({customer->id, "owned", "", "private", live->id});
    const auto orphan = store->create_repository({customer->id, "orphan", "", "private", live->id});
    const auto gone = store->create_repository({customer->id, "gone", "", "private", doomed->id});
    ASSERT_TRUE(owned && orphan && gone);
    pqxx::connection connection(uri);
    {
        // The orphan stands for a repository migration 0051 could not assign.
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = NULL WHERE id = $1::uuid",
                       orphan->id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.commit();
    }
    ASSERT_TRUE(
        store->delete_repository(gone->id, {"2026-10-01T00:00:00Z", "2027-10-01T00:00:00Z"}));
    ASSERT_TRUE(store->update_workspace(doomed->id, {.desired = "deleted"}));
    // An environment as an earlier lake made it: a random id, and a policy object named by it.
    const auto environment = [&](const lake::control::Repository& repository,
                                 const std::string& name) {
        const auto parent = store->find_policy_object("repository", repository.id);
        EXPECT_TRUE(parent) << parent.error().message;
        const std::string id = lake::control::new_uuid_v4();
        const auto object = store->create_policy_object(
            {"environment", id, parent->id, "admin", "platform-migration-" + id, true});
        EXPECT_TRUE(object) << object.error().message;
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO platform_environments (id, repository_id, name, object_id) "
                       "VALUES ($1::uuid, $2::uuid, $3, $4::uuid)",
                       id, repository.id, name, object->id);
        tx.commit();
        return std::pair{id, object->id};
    };
    const auto mirror = [&](const std::string& kind, const std::string& key,
                            const std::string& repository_id) {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO platform_mirror (kind, key, value, seen_at) VALUES ($1, $2, "
                       "$3::jsonb, now())",
                       kind, key,
                       nlohmann::json{{"repository", repository_id},
                                      {"environment", "production"},
                                      {"phase", "ready"}}
                           .dump());
        tx.commit();
        return lake::control::platform_mirror_id(kind, key);
    };
    const auto [production, production_object] = environment(*owned, "production");
    const auto [waiting, waiting_object] = environment(*orphan, "staging");
    const auto [released, released_object] = environment(*gone, "production");
    const std::string deployment = mirror("deployment", "deployment-" + suffix, owned->id);
    const std::string waiting_value =
        mirror("environment_env", orphan->id + "/staging/COLOR", orphan->id);
    const std::string stranger =
        mirror("domain", "stranger-" + suffix, lake::control::new_uuid_v4());
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0060_platform_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0060_platform_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& table, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = $1 AND object_id = $2::uuid",
            table, id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    using Owner = std::pair<std::string, std::string>;
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome("platform_environments", production),
                  (Owner{"repository_workspace", live->id}));
        EXPECT_EQ(outcome("platform_environments", waiting), (Owner{"needs_selection", ""}));
        EXPECT_EQ(outcome("platform_environments", released), (Owner{"workspace_deleted", ""}));
        EXPECT_EQ(outcome("platform_mirror", deployment),
                  (Owner{"repository_workspace", live->id}));
        EXPECT_EQ(outcome("platform_mirror", waiting_value), (Owner{"needs_selection", ""}));
        EXPECT_TRUE(outcome("platform_mirror", stranger).first.empty())
            << "a row naming no repository is reported, not assigned";
    }
    // The policy object keeps its id and grants and now answers to the environment's derived id,
    // which lake computes the same way.
    const auto renamed = store->find_policy_object(
        "environment", lake::control::platform_environment_id(owned->id, "production"));
    ASSERT_TRUE(renamed) << renamed.error().message;
    EXPECT_EQ(renamed->id, production_object);

    // A row written after the migration, and a repository assigned after review, are picked up by
    // the assignment each copy reruns.
    const auto [late, late_object] = environment(*owned, "preview");
    const std::string late_value =
        mirror("environment_env", owned->id + "/production/LATE", owned->id);
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE repositories DISABLE TRIGGER repository_workspace_routing");
        tx.exec_params("UPDATE repositories SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       orphan->id, live->id);
        tx.exec("ALTER TABLE repositories ENABLE TRIGGER repository_workspace_routing");
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_platform_environments());
    ASSERT_TRUE(store->assign_legacy_platform_mirror());
    EXPECT_EQ(outcome("platform_environments", late), (Owner{"repository_workspace", live->id}));
    EXPECT_EQ(outcome("platform_environments", waiting),
              (Owner{"repository_workspace", live->id}));
    EXPECT_EQ(outcome("platform_environments", released).first, "workspace_deleted");
    EXPECT_EQ(outcome("platform_mirror", late_value), (Owner{"repository_workspace", live->id}));
    EXPECT_EQ(outcome("platform_mirror", waiting_value),
              (Owner{"repository_workspace", live->id}));
    const auto preview = store->find_policy_object(
        "environment", lake::control::platform_environment_id(owned->id, "preview"));
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview->id, late_object);

    std::map<std::string, lake::control::LegacyPlatformEnvironment> environments;
    for (std::string after;;) {
        const auto page = store->legacy_platform_environments(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            environments.emplace(row.id, row);
        }
        after = page->back().id;
    }
    ASSERT_TRUE(environments.contains(production));
    EXPECT_EQ(environments.at(production).workspace_id, live->id);
    EXPECT_EQ(environments.at(production).customer_id, customer->id);
    EXPECT_EQ(environments.at(production).environment.name, "production");
    EXPECT_EQ(environments.at(production).environment.object_id, production_object);
    EXPECT_GT(environments.at(production).environment.at_us, 0);
    EXPECT_EQ(environments.at(released).outcome, "workspace_deleted");

    std::map<std::string, lake::control::LegacyPlatformMirrorRow> rows;
    for (std::string after;;) {
        const auto page = store->legacy_platform_mirror(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            rows.emplace(row.id, row);
        }
        after = page->back().id;
    }
    ASSERT_TRUE(rows.contains(deployment));
    EXPECT_EQ(rows.at(deployment).workspace_id, live->id);
    EXPECT_EQ(rows.at(deployment).repository_id, owned->id);
    EXPECT_EQ(rows.at(deployment).row.value["phase"], "ready");
    EXPECT_GT(rows.at(deployment).at_us, 0);
    ASSERT_TRUE(rows.contains(stranger));
    EXPECT_EQ(rows.at(stranger).outcome, "unknown_repository");
    EXPECT_TRUE(rows.at(stranger).workspace_id.empty());
}

} // namespace

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

// Migration 0046 (lake#376): each Opportunity's owner is the workspace its scan ran in, else the
// space's only live workspace; a deleted workspace's rows and ambiguous rows are only reported.
TEST(PostgresOpportunities, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "opportunity-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "opportunity-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        // The second space has three workspaces (two once one is deleted), so a scan that names
        // none cannot be assigned.
        for (int count = 0; count <= index * 2; ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            workspaces[index].push_back(workspace->id);
        }
    }
    pqxx::connection connection(uri);
    // A scan; an empty workspace is a task written before tasks named their workspace (0038).
    const auto scan = [&](const std::string& customer, const std::string& workspace) {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE agent_tasks DISABLE TRIGGER agent_task_workspace_routing");
        const std::string id =
            tx.exec_params("INSERT INTO agent_tasks (customer_id, agent, kind, scopes, "
                           "workspace_id) VALUES ($1::uuid, 'agent-autosuggest', 'discover', "
                           "ARRAY['list_datasets'], NULLIF($2, '')::uuid) RETURNING id::text",
                           customer, workspace)[0][0]
                .as<std::string>();
        tx.exec("ALTER TABLE agent_tasks ENABLE TRIGGER agent_task_workspace_routing");
        tx.commit();
        return id;
    };
    const auto insert = [&](const std::string& customer, const std::string& task) {
        pqxx::work tx(connection);
        const std::string id = tx.exec_params(
            "INSERT INTO opportunities (customer_id, task_id, rank, intent) VALUES ($1::uuid, "
            "$2::uuid, (SELECT count(*) + 1 FROM opportunities WHERE task_id = $2::uuid), "
            "'Predict churn') RETURNING id::text",
            customer, task)[0][0].as<std::string>();
        tx.commit();
        return id;
    };
    const std::string own_scan = insert(customers[1], scan(customers[1], workspaces[1][1]));
    const std::string sole = insert(customers[0], scan(customers[0], ""));
    const std::string ambiguous = insert(customers[1], scan(customers[1], ""));
    const std::string doomed_task = scan(customers[1], workspaces[1][0]);
    const std::string doomed = insert(customers[1], doomed_task);
    ASSERT_TRUE(store->update_workspace(workspaces[1][0], {.desired = "deleted"}));
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0046_opportunity_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0046_opportunity_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'opportunities' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    using Outcome = std::pair<std::string, std::string>;
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(own_scan), (Outcome{"task_workspace", workspaces[1][1]}))
            << "the scan's workspace owns it, though the space has several";
        EXPECT_EQ(outcome(sole), (Outcome{"sole_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome(ambiguous), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome(doomed), (Outcome{"deleted_workspace", ""}))
            << "a deleted workspace's rows are never moved into another";
    }
    const std::string late = insert(customers[0], scan(customers[0], workspaces[0][0]));
    EXPECT_TRUE(outcome(late).first.empty());
    ASSERT_TRUE(store->assign_legacy_opportunities());
    EXPECT_EQ(outcome(late), (Outcome{"task_workspace", workspaces[0][0]}))
        << "a row written after the migration";

    std::map<std::string, lake::control::LegacyOpportunity> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_opportunities(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            listed.emplace(row.opportunity.id, row);
        }
        after = page->back().opportunity.id;
    }
    ASSERT_TRUE(listed.contains(own_scan));
    EXPECT_EQ(listed.at(own_scan).workspace_id, workspaces[1][1]);
    EXPECT_EQ(listed.at(own_scan).opportunity.intent, "Predict churn");
    EXPECT_EQ(listed.at(own_scan).opportunity.status, "new");
    EXPECT_GT(listed.at(own_scan).opportunity.at_us, 0);
    EXPECT_EQ(listed.at(ambiguous).outcome, "needs_selection");
    EXPECT_EQ(listed.at(doomed).outcome, "deleted_workspace");
    EXPECT_TRUE(listed.at(doomed).workspace_id.empty());
}

// Migration 0047 (lake#379): each stream_owners row is recorded against the workspace it names,
// keyed by md5(customer_id, workspace_id, topic) as lake computes it.
TEST(PostgresStreamOwners, MigrationRecordsEachRowsWorkspaceForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "stream-owner-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers, workspaces;
    for (int index = 0; index < 2; ++index) {
        const std::string name = "stream-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        const auto workspace = store->create_workspace({customer->id, "ws", "dev", ""});
        ASSERT_TRUE(workspace) << workspace.error().message;
        workspaces.push_back(workspace->id);
    }
    const auto deleted = store->create_workspace({customers[0], "old", "dev", ""});
    ASSERT_TRUE(deleted);
    ASSERT_TRUE(store->update_workspace(deleted->id, {.desired = "deleted"}));
    pqxx::connection connection(uri);
    const auto insert = [&](const lake::control::StreamOwner& owner) {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO stream_owners (customer_id, workspace_id, topic, created_by) "
                       "VALUES ($1::uuid, $2, $3, $4)",
                       owner.customer_id, owner.workspace_id, owner.topic, owner.created_by);
        tx.commit();
        return lake::control::legacy_stream_owner_id(owner);
    };
    const std::string owned = insert({customers[0], workspaces[0], "orders", "taap:a"});
    const std::string other_space = insert({customers[1], workspaces[0], "orders", "taap:b"});
    const std::string not_uuid = insert({customers[1], "workspace-one", "orders", "taap:b"});
    const std::string gone = insert({customers[0], deleted->id, "orders", "taap:a"});
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0047_stream_owner_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0047_stream_owner_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'stream_owners' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(owned), (std::pair<std::string, std::string>{"recorded_workspace",
                                                                       workspaces[0]}));
        EXPECT_EQ(outcome(other_space), (std::pair<std::string, std::string>{"unknown_workspace", ""}))
            << "a workspace of another space never owns the row";
        EXPECT_EQ(outcome(not_uuid).first, "unknown_workspace");
        EXPECT_EQ(outcome(gone), (std::pair<std::string, std::string>{"workspace_deleted", ""}));
    }
    const std::string late = insert({customers[1], workspaces[1], "late", "taap:b"});
    EXPECT_TRUE(outcome(late).first.empty());
    ASSERT_TRUE(store->assign_legacy_stream_owners());
    EXPECT_EQ(outcome(late).first, "recorded_workspace") << "a row written after the migration";

    std::map<std::string, lake::control::LegacyStreamOwner> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_stream_owners(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            EXPECT_GT(row.id, after) << "pages are ordered by id";
            listed.emplace(row.id, row);
        }
        after = page->back().id;
    }
    ASSERT_TRUE(listed.contains(owned)) << "lake and the migration compute the same id";
    EXPECT_EQ(listed.at(owned).workspace_id, workspaces[0]);
    EXPECT_EQ(listed.at(owned).owner.created_by, "taap:a");
    EXPECT_EQ(listed.at(owned).owner.topic, "orders");
    EXPECT_EQ(listed.at(late).workspace_id, workspaces[1]);
    EXPECT_EQ(listed.at(gone).outcome, "workspace_deleted");
    EXPECT_TRUE(listed.at(not_uuid).workspace_id.empty());

    // A workspace deleted after its rows were assigned no longer receives them.
    ASSERT_TRUE(store->update_workspace(workspaces[1], {.desired = "deleted"}));
    const auto relisted = store->legacy_stream_owners("", 5000);
    ASSERT_TRUE(relisted);
    const auto row = std::ranges::find(*relisted, late, &lake::control::LegacyStreamOwner::id);
    ASSERT_NE(row, relisted->end());
    EXPECT_EQ(row->outcome, "workspace_deleted");
    EXPECT_TRUE(row->workspace_id.empty());
}

// Migration 0049 (lake#356, lake#357): an application in a space with one live workspace is
// assigned it, every other is needs_selection; each event row's owner is its application's
// workspace. Rerunning the migration changes nothing, the store's rerun of the assignment picks up
// a late row and resolves rows once their application is assigned, and the assignment happens
// once.
TEST(PostgresApplicationWebhooks, MigrationRecordsEachEventsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "application-webhook-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "app-webhook-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        // The second space has two workspaces, so its application cannot be assigned by itself.
        for (int count = 0; count <= index; ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            workspaces[index].push_back(workspace->id);
        }
    }
    pqxx::connection connection(uri);
    std::vector<std::string> applications;
    {
        // Applications as registered before 0049: no workspace.
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        for (const std::string& customer : customers) {
            const std::string id = lake::control::new_uuid_v4();
            tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                           "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'migration', "
                           "'[\"https://m.example.test\"]'::jsonb)",
                           id, customer, "app:" + id);
            applications.push_back(id);
        }
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        tx.commit();
    }
    const auto insert = [&](const std::string& application) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO auth_app_webhook_events (application_id, event, payload) "
                           "VALUES ($1::uuid, 'user.created', '{\"n\":1}'::jsonb) RETURNING id::text",
                           application)[0][0]
                .as<std::string>();
        tx.commit();
        return id;
    };
    const std::string owned = insert(applications[0]);
    const std::string ambiguous = insert(applications[1]);
    {
        pqxx::work tx(connection);
        const std::string hook =
            tx.exec_params("INSERT INTO auth_app_webhooks (application_id, url, events, "
                           "encrypted_secret) VALUES ($1::uuid, 'https://h.test', "
                           "ARRAY['user.created'], 'x') RETURNING id::text",
                           applications[0])[0][0]
                .as<std::string>();
        tx.exec_params("INSERT INTO auth_app_webhook_deliveries (event_id, webhook_id, attempt, "
                       "status, signature) VALUES ($1::uuid, $2::uuid, 1, 204, 'sig')",
                       owned, hook);
        tx.exec("DELETE FROM schema_migrations WHERE version = "
                "'0049_application_webhook_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0049_application_webhook_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& table, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = $1 AND object_id = $2::uuid",
            table, id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    const auto application_workspace = [&](const std::string& id) {
        pqxx::work tx(connection);
        return tx.exec_params("SELECT COALESCE(workspace_id::text, '') FROM auth_applications "
                              "WHERE id = $1::uuid",
                              id)[0][0]
            .as<std::string>();
    };
    // The schema section from 0049 on (later sections are idempotent).
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome("auth_applications", applications[0]),
                  (std::pair<std::string, std::string>{"sole_workspace", workspaces[0][0]}));
        EXPECT_EQ(application_workspace(applications[0]), workspaces[0][0]);
        EXPECT_EQ(outcome("auth_applications", applications[1]).first, "needs_selection");
        EXPECT_TRUE(application_workspace(applications[1]).empty());
        EXPECT_EQ(outcome("auth_app_webhook_events", owned),
                  (std::pair<std::string, std::string>{"application_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome("auth_app_webhook_events", ambiguous).first, "needs_selection");
    }
    const std::string late = insert(applications[0]);
    EXPECT_TRUE(outcome("auth_app_webhook_events", late).first.empty());
    ASSERT_TRUE(store->assign_legacy_application_webhook_events());
    EXPECT_EQ(outcome("auth_app_webhook_events", late).first, "application_workspace")
        << "a row written after the migration";

    // A reviewed assignment of the unassigned application resolves its events; after that its
    // workspace never changes, and the first application's never did.
    {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE "
                                    "id = $1::uuid",
                                    applications[1], workspaces[0][0]),
                     pqxx::check_violation)
            << "a workspace of another space";
    }
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       applications[1], workspaces[1][1]);
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_application_webhook_events());
    EXPECT_EQ(outcome("auth_app_webhook_events", ambiguous),
              (std::pair<std::string, std::string>{"application_workspace", workspaces[1][1]}));
    for (const auto& [application, workspace] :
         {std::pair{applications[0], workspaces[1][0]}, std::pair{applications[1], workspaces[1][0]}}) {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE "
                                    "id = $1::uuid",
                                    application, workspace),
                     pqxx::check_violation);
    }
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET name = 'renamed' WHERE id = $1::uuid",
                       applications[0]);
        tx.commit();
    }

    std::map<std::string, lake::control::LegacyApplicationWebhookEvent> events;
    std::string after;
    for (;;) {
        const auto page = store->legacy_application_webhook_events(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            events.emplace(row.event.id, row);
        }
        after = page->back().event.id;
    }
    ASSERT_TRUE(events.contains(owned));
    EXPECT_EQ(events.at(owned).workspace_id, workspaces[0][0]);
    EXPECT_EQ(events.at(owned).customer_id, customers[0]);
    EXPECT_EQ(events.at(owned).event.payload["n"], 1);
    EXPECT_GT(events.at(owned).event.at_us, 0);
    EXPECT_EQ(events.at(ambiguous).workspace_id, workspaces[1][1]);
    const auto deliveries = store->legacy_application_webhook_deliveries("", 5000);
    ASSERT_TRUE(deliveries) << deliveries.error().message;
    const auto delivered = std::ranges::find_if(
        *deliveries, [&](const auto& row) { return row.delivery.event_id == owned; });
    ASSERT_NE(delivered, deliveries->end());
    EXPECT_EQ(delivered->workspace_id, workspaces[0][0]) << "a delivery goes where its event goes";
    EXPECT_EQ(delivered->delivery.application_id, applications[0]);
    EXPECT_EQ(delivered->delivery.status, 204);
}

// Migration 0055 (lake#353, lake#354): each application user and organization row's owner is its
// application's workspace, needs_selection while the application has none, and workspace_deleted
// when that workspace is gone. A user row's id is md5(application, newline, user), the same id lake
// computes. Rerunning the migration changes nothing; the store's rerun of the assignment picks up
// a late row and resolves rows once their application is assigned.
TEST(PostgresApplicationUsers, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "application-user-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "app-user-migration-" + suffix + "-" + std::to_string(index);
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
    pqxx::connection connection(uri);
    // Three applications: one in the first space's workspace, one of the second space with none
    // yet (registered before 0049), one in a workspace of the second space that is then deleted.
    std::vector<std::string> applications;
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        for (const auto& [customer, workspace] : {std::pair{customers[0], workspaces[0][0]},
                                                  std::pair{customers[1], std::string()},
                                                  std::pair{customers[1], workspaces[1][1]}}) {
            const std::string id = lake::control::new_uuid_v4();
            tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                           "allowed_origins, workspace_id) VALUES ($1::uuid, $2::uuid, $3, "
                           "'migration', '[\"https://m.example.test\"]'::jsonb, "
                           "NULLIF($4, '')::uuid)",
                           id,
                           customer,
                           "app:" + id,
                           workspace);
            applications.push_back(id);
        }
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        tx.exec_params("UPDATE workspaces SET desired = 'deleted' WHERE id = $1::uuid",
                       workspaces[1][1]);
        tx.commit();
    }
    // A pool user of an application and an organization of it, as an earlier lake wrote them.
    const auto user_of = [&](const std::string& application) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO users (taap_id, email, display_name, pool_id) VALUES "
                           "($1, $2, 'Pool user', $3) RETURNING id::text",
                           "taap:pool-" + lake::control::new_uuid_v4(),
                           "pool-" + lake::control::new_uuid_v4() + "@example.test",
                           "app:" + application)[0][0]
                .as<std::string>();
        tx.exec_params("INSERT INTO auth_app_users (application_id, user_id) VALUES ($1::uuid, "
                       "$2::uuid)",
                       application,
                       id);
        tx.commit();
        return id;
    };
    const auto organization_of = [&](const std::string& application) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO auth_app_organizations (application_id, taap_id, name) "
                           "VALUES ($1::uuid, $2, $3) RETURNING id::text",
                           application,
                           "taap:org-" + lake::control::new_uuid_v4(),
                           "org-" + lake::control::new_uuid_v4())[0][0]
                .as<std::string>();
        tx.commit();
        return id;
    };
    std::vector<std::string> users, organizations;
    for (const std::string& application : applications) {
        users.push_back(lake::control::application_user_row_id(application, user_of(application)));
        organizations.push_back(organization_of(application));
    }
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0055_application_user_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0055_application_user_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& table, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = $1 AND object_id = $2::uuid",
            table,
            id);
        return found.empty()
                   ? std::pair<std::string, std::string>{}
                   : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    using Outcome = std::pair<std::string, std::string>;
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        for (const auto& [table, ids] :
             {std::pair{std::string("auth_app_users"), users},
              std::pair{std::string("auth_app_organizations"), organizations}}) {
            EXPECT_EQ(outcome(table, ids[0]), (Outcome{"application_workspace", workspaces[0][0]}))
                << table;
            EXPECT_EQ(outcome(table, ids[1]), (Outcome{"needs_selection", ""})) << table;
            EXPECT_EQ(outcome(table, ids[2]), (Outcome{"workspace_deleted", ""})) << table;
        }
    }

    // A row written after the migration is recorded by the store's rerun.
    const std::string late =
        lake::control::application_user_row_id(applications[0], user_of(applications[0]));
    EXPECT_TRUE(outcome("auth_app_users", late).first.empty());
    ASSERT_TRUE(store->assign_legacy_application_directory());
    EXPECT_EQ(outcome("auth_app_users", late),
              (Outcome{"application_workspace", workspaces[0][0]}));

    // A reviewed assignment of the unassigned application resolves its rows.
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       applications[1],
                       workspaces[1][0]);
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_application_directory());
    EXPECT_EQ(outcome("auth_app_users", users[1]),
              (Outcome{"application_workspace", workspaces[1][0]}));
    EXPECT_EQ(outcome("auth_app_organizations", organizations[1]),
              (Outcome{"application_workspace", workspaces[1][0]}));
    EXPECT_EQ(outcome("auth_app_users", users[2]).first, "workspace_deleted");

    // The legacy listing pages by row id and carries each row's owner and pool membership.
    std::map<std::string, lake::control::LegacyApplicationUser> listed;
    std::string after;
    for (;;) {
        const auto page = store->legacy_application_users(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        for (const auto& row : *page) {
            EXPECT_GT(row.id, after);
            listed.emplace(row.id, row);
        }
        if (page->size() < 2) {
            break;
        }
        after = page->back().id;
    }
    ASSERT_TRUE(listed.contains(users[0]));
    EXPECT_EQ(listed.at(users[0]).workspace_id, workspaces[0][0]);
    EXPECT_TRUE(listed.at(users[0]).user.own_pool);
    EXPECT_EQ(listed.at(users[0]).user.status, "active");
    EXPECT_EQ(listed.at(users[2]).outcome, "workspace_deleted");
    const auto orgs = store->legacy_application_organizations("", 5000);
    ASSERT_TRUE(orgs);
    EXPECT_EQ(std::ranges::count_if(*orgs,
                                    [&](const auto& row) {
                                        return std::ranges::find(organizations,
                                                                 row.organization.id) !=
                                               organizations.end();
                                    }),
              3);
}

// lake#352, lake#355: migration 0056 records the owner of each application's key ring and each
// webhook, the application's workspace, for the copy; the copy's assignment picks up later rows
// and resolves an application assigned after review.
TEST(PostgresApplicationCredentials, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "application-credential-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "app-credential-migration-" + suffix + "-" + std::to_string(index);
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
    const auto registered = [&](const std::string& customer, const std::string& workspace) {
        const std::string id = lake::control::new_uuid_v4();
        const auto created = store->create_application({.id = id,
                                                        .customer_id = customer,
                                                        .pool_id = "app:" + id,
                                                        .workspace_id = workspace});
        EXPECT_TRUE(created) << created.error().message;
        return id;
    };
    // An application of the first space, one of the second space's from before 0049 (no
    // workspace), and one whose workspace is deleted.
    const std::string owned = registered(customers[0], workspaces[0][0]);
    const std::string gone = registered(customers[1], workspaces[1][1]);
    std::string ambiguous = lake::control::new_uuid_v4();
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'migration', "
                       "'[\"https://m.example.test\"]'::jsonb)",
                       ambiguous, customers[1], "app:" + ambiguous);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        tx.commit();
    }
    ASSERT_TRUE(store->update_workspace(workspaces[1][1], {.desired = "deleted"}));
    const auto insert_key = [&](const std::string& application, int version, bool current) {
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO auth_app_signing_keys (application_id, kid, version, "
                       "encrypted_seed, public_key, current) VALUES ($1::uuid, $2, $3, 'envelope', "
                       "'public', $4)",
                       application, "app-" + application + "-" + std::to_string(version), version,
                       current);
        tx.commit();
    };
    const auto insert_webhook = [&](const std::string& application) {
        pqxx::work tx(connection);
        const std::string id =
            tx.exec_params("INSERT INTO auth_app_webhooks (application_id, url, events, "
                           "encrypted_secret) VALUES ($1::uuid, 'https://h.test', "
                           "ARRAY['user.created'], 'envelope') RETURNING id::text",
                           application)[0][0]
                .as<std::string>();
        tx.commit();
        return id;
    };
    for (const std::string& application : {owned, ambiguous, gone}) {
        insert_key(application, 1, false);
        insert_key(application, 2, true);
    }
    const std::string owned_hook = insert_webhook(owned);
    const std::string ambiguous_hook = insert_webhook(ambiguous);
    const std::string gone_hook = insert_webhook(gone);
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = "
                "'0056_application_credential_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0056_application_credential_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& table, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = $1 AND object_id = $2::uuid",
            table, id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    using Outcome = std::pair<std::string, std::string>;
    // The schema section from 0056 on, twice: the second run changes nothing.
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome("auth_app_signing_keys", owned),
                  (Outcome{"application_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome("auth_app_webhooks", owned_hook),
                  (Outcome{"application_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome("auth_app_signing_keys", ambiguous), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome("auth_app_webhooks", ambiguous_hook), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome("auth_app_signing_keys", gone), (Outcome{"workspace_deleted", ""}));
        EXPECT_EQ(outcome("auth_app_webhooks", gone_hook), (Outcome{"workspace_deleted", ""}));
    }
    const std::string late = insert_webhook(owned);
    EXPECT_TRUE(outcome("auth_app_webhooks", late).first.empty());
    ASSERT_TRUE(store->assign_legacy_application_credentials());
    EXPECT_EQ(outcome("auth_app_webhooks", late),
              (Outcome{"application_workspace", workspaces[0][0]}))
        << "a row written after the migration";

    // A reviewed assignment of the unassigned application resolves its rows.
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       ambiguous, workspaces[1][0]);
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_application_credentials());
    EXPECT_EQ(outcome("auth_app_signing_keys", ambiguous),
              (Outcome{"application_workspace", workspaces[1][0]}));
    EXPECT_EQ(outcome("auth_app_webhooks", ambiguous_hook),
              (Outcome{"application_workspace", workspaces[1][0]}));
    EXPECT_EQ(outcome("auth_app_signing_keys", gone), (Outcome{"workspace_deleted", ""}));

    // Key rings page by application, each with every key in version order.
    std::map<std::string, lake::control::LegacyApplicationSigningKeyRing> rings;
    std::string after;
    for (;;) {
        const auto page = store->legacy_application_signing_keys(after, 1);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        ASSERT_EQ(page->size(), 1U);
        rings.emplace(page->front().ring.application_id, page->front());
        after = page->back().ring.application_id;
    }
    ASSERT_TRUE(rings.contains(owned));
    const auto& ring = rings.at(owned);
    EXPECT_EQ(ring.workspace_id, workspaces[0][0]);
    EXPECT_EQ(ring.customer_id, customers[0]);
    ASSERT_EQ(ring.ring.keys.size(), 2U);
    EXPECT_EQ(ring.ring.keys[0].version, 1);
    EXPECT_FALSE(ring.ring.keys[0].current);
    EXPECT_TRUE(ring.ring.keys[1].current);
    EXPECT_EQ(ring.ring.keys[1].encrypted_seed, "envelope");
    EXPECT_GT(ring.ring.at_us, 0);
    EXPECT_EQ(rings.at(gone).outcome, "workspace_deleted");
    std::map<std::string, lake::control::LegacyApplicationWebhook> hooks;
    after.clear();
    for (;;) {
        const auto page = store->legacy_application_webhooks(after, 2);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            hooks.emplace(row.webhook.id, row);
        }
        after = page->back().webhook.id;
    }
    ASSERT_TRUE(hooks.contains(late));
    EXPECT_EQ(hooks.at(late).workspace_id, workspaces[0][0]);
    EXPECT_EQ(hooks.at(owned_hook).webhook.encrypted_secret, "envelope");
    EXPECT_EQ(hooks.at(owned_hook).webhook.events, std::vector<std::string>{"user.created"});
    EXPECT_GT(hooks.at(owned_hook).webhook.at_us, 0);
}

// Migration 0061 (lake#363) records the owner of each live (unused, unexpired) application
// authorization code under md5(code_hash): the application's workspace, needs_selection while it
// has none, workspace_deleted when that workspace is gone. Used and expired codes are not recorded
// or listed. The copy's assignment records a code written after it and resolves an application
// assigned after review; recording a code as copied keeps it from being written twice.
TEST(PostgresApplicationAuthorizationCodes, MigrationRecordsEachLiveCodesOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "application-code-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "app-code-migration-" + suffix + "-" + std::to_string(index);
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
    const auto person = store->create_user({"taap:code-person-" + suffix, "", "Person"});
    ASSERT_TRUE(person) << person.error().message;
    const auto registered = [&](const std::string& customer, const std::string& workspace) {
        const std::string id = lake::control::new_uuid_v4();
        const auto created = store->create_application({.id = id,
                                                        .customer_id = customer,
                                                        .pool_id = "app:" + id,
                                                        .workspace_id = workspace});
        EXPECT_TRUE(created) << created.error().message;
        return id;
    };
    const std::string owned = registered(customers[0], workspaces[0][0]);
    const std::string gone = registered(customers[1], workspaces[1][1]);
    const std::string ambiguous = lake::control::new_uuid_v4();
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins) VALUES ($1::uuid, $2::uuid, $3, 'migration', "
                       "'[\"https://m.example.test\"]'::jsonb)",
                       ambiguous, customers[1], "app:" + ambiguous);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        tx.commit();
    }
    ASSERT_TRUE(store->update_workspace(workspaces[1][1], {.desired = "deleted"}));
    // `state`: live, used or expired.
    const auto insert_code = [&](const std::string& application, const std::string& state) {
        const std::string hash = lake::control::identity_crypto::sha256_hex(
            "code-" + lake::control::new_uuid_v4());
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO auth_app_authorization_codes (code_hash, application_id, "
                       "user_id, redirect_uri, code_challenge, nonce, expires_at, used_at) VALUES "
                       "($1, $2::uuid, $3::uuid, 'https://m.example.test/cb', 'challenge', "
                       "'nonce', now() + CASE WHEN $4 = 'expired' THEN interval '-1 minute' ELSE "
                       "interval '4 minutes' END, CASE WHEN $4 = 'used' THEN now() END)",
                       hash, application, person->id, state);
        tx.commit();
        return hash;
    };
    const std::string owned_code = insert_code(owned, "live");
    const std::string used_code = insert_code(owned, "used");
    const std::string expired_code = insert_code(owned, "expired");
    const std::string ambiguous_code = insert_code(ambiguous, "live");
    const std::string gone_code = insert_code(gone, "live");
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = "
                "'0061_application_authorization_code_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0061_application_authorization_code_workspaces:");
    ASSERT_NE(start, std::string::npos);
    using Outcome = std::pair<std::string, std::string>;
    const auto outcome = [&](const std::string& hash) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'auth_app_authorization_codes' AND object_id = md5($1)::uuid",
            hash);
        return found.empty() ? Outcome{}
                             : Outcome{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    // The schema section from 0061 on, twice: the second run changes nothing.
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(owned_code), (Outcome{"application_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome(ambiguous_code), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome(gone_code), (Outcome{"workspace_deleted", ""}));
        EXPECT_TRUE(outcome(used_code).first.empty()) << "a used code is never copied";
        EXPECT_TRUE(outcome(expired_code).first.empty()) << "an expired code is never copied";
    }
    const std::string late = insert_code(owned, "live");
    EXPECT_TRUE(outcome(late).first.empty());
    ASSERT_TRUE(store->assign_legacy_application_authorization_codes());
    EXPECT_EQ(outcome(late), (Outcome{"application_workspace", workspaces[0][0]}))
        << "a code issued after the migration";

    // A reviewed assignment of the unassigned application resolves its code.
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       ambiguous, workspaces[1][0]);
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_application_authorization_codes());
    EXPECT_EQ(outcome(ambiguous_code), (Outcome{"application_workspace", workspaces[1][0]}));
    EXPECT_EQ(outcome(gone_code), (Outcome{"workspace_deleted", ""}));

    // Recording a copy needs a workspace; a copied code says so in its listing.
    ASSERT_TRUE(store->record_legacy_application_authorization_code_copied(owned_code));
    EXPECT_EQ(outcome(owned_code), (Outcome{"copied", workspaces[0][0]}));
    EXPECT_FALSE(store->record_legacy_application_authorization_code_copied(gone_code));
    ASSERT_TRUE(store->assign_legacy_application_authorization_codes());
    EXPECT_EQ(outcome(owned_code).first, "copied") << "a rerun keeps the record of the copy";

    // Live codes only, paged by hash.
    std::map<std::string, lake::control::LegacyApplicationAuthorizationCode> codes;
    std::string after;
    for (;;) {
        const auto page = store->legacy_application_authorization_codes(after, 1);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        ASSERT_EQ(page->size(), 1U);
        EXPECT_GT(page->front().code.code_hash, after);
        codes.emplace(page->front().code.code_hash, page->front());
        after = page->back().code.code_hash;
    }
    EXPECT_FALSE(codes.contains(used_code));
    EXPECT_FALSE(codes.contains(expired_code));
    ASSERT_TRUE(codes.contains(owned_code));
    ASSERT_TRUE(codes.contains(late));
    const auto& row = codes.at(owned_code).code;
    EXPECT_EQ(codes.at(owned_code).outcome, "copied");
    EXPECT_EQ(codes.at(owned_code).customer_id, customers[0]);
    EXPECT_EQ(row.application_id, owned);
    EXPECT_EQ(row.user_id, person->id);
    EXPECT_EQ(row.redirect_uri, "https://m.example.test/cb");
    EXPECT_EQ(row.at_us, row.expires_us - 5LL * 60 * 1'000'000);
    EXPECT_EQ(codes.at(gone_code).outcome, "workspace_deleted");
    EXPECT_TRUE(codes.at(gone_code).workspace_id.empty());
}

// Migration 0059 (lake#351) records the owner of each application's settings that the retired
// auth_applications columns still hold: the application's workspace, needs_selection while it has
// none, workspace_deleted when that workspace is gone. A routing row written after it holds no
// settings and is not recorded; the copy's assignment resolves an application assigned after review.
TEST(PostgresApplicationSettings, MigrationRecordsEachRowsOwnerForTheCopy) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "application-settings-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "app-settings-migration-" + suffix + "-" + std::to_string(index);
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
    pqxx::connection connection(uri);
    // Applications as registered before 0059, with their settings in the row: one of the first
    // space, one of the second space's from before 0049 (no workspace), one whose workspace is
    // deleted.
    const auto legacy = [&](const std::string& customer, const std::string& workspace) {
        const std::string id = lake::control::new_uuid_v4();
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE auth_applications DISABLE TRIGGER application_workspace_routing");
        tx.exec_params("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                       "allowed_origins, redirect_uris, theme, workspace_id, created_at) VALUES "
                       "($1::uuid, $2::uuid, $3, 'Legacy ' || $1, "
                       "'[\"https://legacy.example.test\"]'::jsonb, "
                       "'[\"https://legacy.example.test/callback\"]'::jsonb, "
                       "'{\"accent_color\":\"#123456\"}'::jsonb, NULLIF($4, '')::uuid, "
                       "'2026-09-01T10:00:00Z')",
                       id, customer, "app:" + id, workspace);
        tx.exec("ALTER TABLE auth_applications ENABLE TRIGGER application_workspace_routing");
        tx.commit();
        return id;
    };
    const std::string owned = legacy(customers[0], workspaces[0][0]);
    const std::string ambiguous = legacy(customers[1], "");
    const std::string gone = legacy(customers[1], workspaces[1][1]);
    ASSERT_TRUE(store->update_workspace(workspaces[1][1], {.desired = "deleted"}));
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = "
                "'0059_application_settings_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0059_application_settings_workspaces:");
    ASSERT_NE(start, std::string::npos);
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'auth_application_settings' AND object_id = $1::uuid",
            id);
        return found.empty() ? std::pair<std::string, std::string>{}
                             : std::pair{found[0][0].as<std::string>(), found[0][1].as<std::string>()};
    };
    using Outcome = std::pair<std::string, std::string>;
    // The schema section from 0059 on, twice: the second run changes nothing.
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(owned), (Outcome{"application_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome(ambiguous), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome(gone), (Outcome{"workspace_deleted", ""}));
    }

    // A new application's row is routing only: no settings, and nothing to copy.
    const std::string routed = lake::control::new_uuid_v4();
    ASSERT_TRUE(store->create_application({.id = routed,
                                           .customer_id = customers[0],
                                           .pool_id = "app:" + routed,
                                           .custom_sign_in_domain = "login-" + suffix + ".test",
                                           .workspace_id = workspaces[0][0]}));
    {
        pqxx::work tx(connection);
        const auto row = tx.exec_params(
            "SELECT name IS NULL AND allowed_origins IS NULL AND redirect_uris IS NULL AND theme "
            "IS NULL FROM auth_applications WHERE id = $1::uuid",
            routed);
        EXPECT_TRUE(row[0][0].as<bool>()) << "the routing row holds no settings";
        EXPECT_THROW(tx.exec_params("UPDATE auth_applications SET theme = '{}'::jsonb WHERE id = "
                                    "$1::uuid",
                                    routed),
                     pqxx::check_violation)
            << "settings without a name are refused";
    }
    ASSERT_TRUE(store->assign_legacy_application_settings());
    EXPECT_TRUE(outcome(routed).first.empty());

    // A reviewed assignment of the unassigned application resolves its settings.
    {
        pqxx::work tx(connection);
        tx.exec_params("UPDATE auth_applications SET workspace_id = $2::uuid WHERE id = $1::uuid",
                       ambiguous, workspaces[1][0]);
        tx.commit();
    }
    ASSERT_TRUE(store->assign_legacy_application_settings());
    EXPECT_EQ(outcome(ambiguous), (Outcome{"application_workspace", workspaces[1][0]}));
    EXPECT_EQ(outcome(gone), (Outcome{"workspace_deleted", ""}));

    // The settings page by application id, with the columns the rows held.
    std::map<std::string, lake::control::LegacyApplicationSettings> rows;
    std::string after;
    for (;;) {
        const auto page = store->legacy_application_settings(after, 1);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        ASSERT_EQ(page->size(), 1U);
        rows.emplace(page->front().settings.application_id, page->front());
        after = page->back().settings.application_id;
    }
    EXPECT_FALSE(rows.contains(routed));
    ASSERT_TRUE(rows.contains(owned));
    const auto& row = rows.at(owned);
    EXPECT_EQ(row.workspace_id, workspaces[0][0]);
    EXPECT_EQ(row.customer_id, customers[0]);
    EXPECT_EQ(row.settings.customer_id, customers[0]);
    EXPECT_EQ(row.settings.name, "Legacy " + owned);
    EXPECT_EQ(row.settings.allowed_origins,
              std::vector<std::string>{"https://legacy.example.test"});
    EXPECT_EQ(row.settings.redirect_uris,
              std::vector<std::string>{"https://legacy.example.test/callback"});
    EXPECT_EQ(row.settings.theme.at("accent_color"), "#123456");
    EXPECT_EQ(row.settings.at_us, 1'788'256'800'000'000);
    EXPECT_EQ(rows.at(gone).outcome, "workspace_deleted");
    EXPECT_EQ(rows.at(ambiguous).workspace_id, workspaces[1][0]);
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
    // secret is simply not active.
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
        store->create_token({user->id, "lake", "short", "test", {"list_customers"}, 1, false});
    ASSERT_TRUE(token) << token.error().message;
    ASSERT_TRUE(store->context_for(token->secret)) << "live before it expires";
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const auto expired = store->context_for(token->secret);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "expired");
    std::string wrong = token->secret;
    wrong.back() = wrong.back() == 'a' ? 'b' : 'a';
    const auto unknown = store->context_for(wrong);
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().code, "expired") << "a wrong secret never learns of the session";
}

// lake#374, lake#375: migration 0062 keeps agent tasks' dispatch rows and records who owns each
// task's content for the copy. The asking task and lake's own notices become columns; a task's
// body, message and answer are left for the copy, and a new task's rows hold none.
TEST(PostgresAgentTasks, MigrationRecordsEachTasksOwnerAndKeepsDispatchContentFree) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, "agent-task-migration-test");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    std::vector<std::string> customers, users;
    std::vector<std::vector<std::string>> workspaces(2);
    for (int index = 0; index < 2; ++index) {
        const std::string name = "agent-task-migration-" + suffix + "-" + std::to_string(index);
        const auto user = store->create_user({"taap:" + name, "", "Migration"});
        ASSERT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
        users.push_back(user->id);
        for (int count = 0; count <= index; ++count) {
            const auto workspace =
                store->create_workspace({customer->id, "ws-" + std::to_string(count), "dev", ""});
            ASSERT_TRUE(workspace) << workspace.error().message;
            workspaces[index].push_back(workspace->id);
        }
    }
    pqxx::connection connection(uri);
    // Tasks as lake wrote them before 0062, with the content in the rows.
    const auto legacy = [&](const std::string& customer, const std::string& workspace,
                            const nlohmann::json& body, const std::string& phase,
                            const std::string& message, const std::string& result) {
        const std::string id = lake::control::new_uuid_v4();
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE agent_tasks DISABLE TRIGGER agent_task_workspace_routing");
        tx.exec_params("INSERT INTO agent_tasks (id, customer_id, agent, kind, body, scopes, "
                       "workspace_id, created_at) VALUES ($1::uuid, $2::uuid, 'agent-project', "
                       "'api', $3::jsonb, ARRAY['get_agent_task'], NULLIF($4, '')::uuid, "
                       "'2026-09-01T10:00:00Z')",
                       id, customer, body.dump(), workspace);
        tx.exec("ALTER TABLE agent_tasks ENABLE TRIGGER agent_task_workspace_routing");
        if (!phase.empty() || !message.empty()) {
            tx.exec_params("INSERT INTO agent_task_status (task_id, phase, message, result) VALUES "
                           "($1::uuid, $2, $3, $4::jsonb)",
                           id, phase, message, result);
        }
        tx.commit();
        return id;
    };
    const std::string asking = legacy(customers[0], workspaces[0][0],
                                      {{"message", "plan it"}}, "Running", "Working on it", "null");
    const std::string owned = legacy(customers[0], workspaces[0][0],
                                     {{"message", "a private request"}, {"parent_task_id", asking}},
                                     "Succeeded", "", "{\"reply\":\"a private answer\"}");
    const std::string retried =
        legacy(customers[1], workspaces[1][0], {{"message", "retry me"}}, "",
               "the agent stopped responding; retrying (attempt 2 of 3)", "null");
    const std::string unpicked = legacy(customers[1], workspaces[1][0], {{"message", "nobody"}},
                                        "Failed", "No agent picked this up within 300 seconds.",
                                        "null");
    const std::string unassigned =
        legacy(customers[1], "", {{"message", "from before workspaces"}}, "", "", "null");
    const std::string gone = legacy(customers[1], workspaces[1][1], {{"message", "gone"}},
                                    "Succeeded", "", "{\"reply\":\"gone\"}");
    const std::string empty = legacy(customers[0], workspaces[0][0], nlohmann::json::object(), "",
                                     "", "null");
    ASSERT_TRUE(store->update_workspace(workspaces[1][1], {.desired = "deleted"}));
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0062_agent_task_workspaces'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto start = schema.find("-- 0062_agent_task_workspaces:");
    ASSERT_NE(start, std::string::npos);
    using Outcome = std::pair<std::string, std::string>;
    const auto outcome = [&](const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(
            "SELECT outcome, COALESCE(workspace_id::text, '') FROM workspace_assignments "
            "WHERE object_table = 'agent_task_content' AND object_id = $1::uuid",
            id);
        return found.empty() ? Outcome{} : Outcome{found[0][0].as<std::string>(),
                                                   found[0][1].as<std::string>()};
    };
    const auto column = [&](const std::string& sql, const std::string& id) {
        pqxx::work tx(connection);
        const auto found = tx.exec_params(sql, id);
        return found.empty() || found[0][0].is_null() ? std::string()
                                                      : found[0][0].as<std::string>();
    };
    // The schema section from 0062 on, twice: the second run changes nothing.
    for (int pass = 0; pass < 2; ++pass) {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema.substr(start));
        tx.commit();
        EXPECT_EQ(outcome(owned), (Outcome{"task_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome(asking), (Outcome{"task_workspace", workspaces[0][0]}));
        EXPECT_EQ(outcome(retried), (Outcome{"task_workspace", workspaces[1][0]}));
        EXPECT_EQ(outcome(unassigned), (Outcome{"needs_selection", ""}));
        EXPECT_EQ(outcome(gone), (Outcome{"workspace_deleted", ""}));
        EXPECT_EQ(outcome(empty), Outcome{}) << "a task with nothing written has nothing to copy";
        EXPECT_EQ(column("SELECT parent_task_id::text FROM agent_tasks WHERE id = $1::uuid", owned),
                  asking);
        EXPECT_EQ(column("SELECT notice FROM agent_task_status WHERE task_id = $1::uuid", retried),
                  "the agent stopped responding; retrying (attempt 2 of 3)");
        EXPECT_EQ(column("SELECT notice FROM agent_task_status WHERE task_id = $1::uuid", unpicked),
                  "No agent picked this up within 300 seconds.");
        EXPECT_EQ(column("SELECT notice FROM agent_task_status WHERE task_id = $1::uuid", asking),
                  "")
            << "what an agent wrote is never a notice";
    }
    // The control store answers the dispatch half only: the notice as its message, no content.
    const auto read = store->get_agent_task(owned);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(read->parent_task_id, asking);
    EXPECT_EQ(read->body, nlohmann::json::object());
    EXPECT_TRUE(read->result.is_null());
    EXPECT_EQ(read->message, "");
    EXPECT_EQ(store->get_agent_task(retried)->message,
              "the agent stopped responding; retrying (attempt 2 of 3)");

    // A new task's rows hold nothing written, through its claim and answer, and are not copied.
    const auto created = store->create_agent_task({.customer_id = customers[0],
                                                   .agent = "agent-project",
                                                   .kind = "api",
                                                   .workspace_id = workspaces[0][0],
                                                   .parent_task_id = asking,
                                                   .body = {{"message", "never stored here"}},
                                                   .scopes = {"get_agent_task"},
                                                   .requested_by_user_id = users[0]});
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(store->claim_agent_task(created->id, "agent-project", "sha"));
    ASSERT_TRUE(store->record_agent_task_event(created->id, "agent-project", "Succeeded"));
    EXPECT_EQ(column("SELECT body::text FROM agent_tasks WHERE id = $1::uuid", created->id), "{}");
    EXPECT_EQ(column("SELECT message || result::text FROM agent_task_status WHERE task_id = "
                     "$1::uuid",
                     created->id),
              "null");
    EXPECT_EQ(column("SELECT parent_task_id::text FROM agent_tasks WHERE id = $1::uuid",
                     created->id),
              asking);
    ASSERT_TRUE(store->assign_legacy_agent_tasks());
    EXPECT_EQ(outcome(created->id), Outcome{});
    EXPECT_EQ(store->create_agent_task({.id = created->id,
                                        .customer_id = customers[0],
                                        .agent = "agent-project",
                                        .workspace_id = workspaces[0][0],
                                        .scopes = {"get_agent_task"}})
                  .error()
                  .code,
              "conflict")
        << "a task id is used once";
    EXPECT_EQ(store->create_agent_task({.id = "not-a-uuid",
                                        .customer_id = customers[0],
                                        .agent = "agent-project",
                                        .workspace_id = workspaces[0][0],
                                        .scopes = {"get_agent_task"}})
                  .error()
                  .code,
              "invalid_input");

    // The content pages by task id, as the rows held it.
    std::map<std::string, lake::control::LegacyAgentTask> rows;
    std::string after;
    for (;;) {
        const auto page = store->legacy_agent_tasks(after, 50);
        ASSERT_TRUE(page) << page.error().message;
        if (page->empty()) {
            break;
        }
        for (const auto& row : *page) {
            rows.emplace(row.task.id, row);
        }
        after = page->back().task.id;
    }
    EXPECT_FALSE(rows.contains(created->id));
    EXPECT_FALSE(rows.contains(empty));
    ASSERT_TRUE(rows.contains(owned));
    const auto& row = rows.at(owned);
    EXPECT_EQ(row.workspace_id, workspaces[0][0]);
    EXPECT_EQ(row.outcome, "task_workspace");
    EXPECT_EQ(row.task.customer_id, customers[0]);
    EXPECT_EQ(row.task.body["message"], "a private request");
    EXPECT_EQ(row.task.result["reply"], "a private answer");
    EXPECT_EQ(row.task.phase, "Succeeded");
    EXPECT_EQ(row.at_us, 1'788'256'800'000'000);
    EXPECT_EQ(rows.at(asking).task.message, "Working on it");
    EXPECT_EQ(rows.at(gone).outcome, "workspace_deleted");
    EXPECT_EQ(rows.at(unassigned).outcome, "needs_selection");
}
