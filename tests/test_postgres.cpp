#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

TEST(PostgresCapabilities, OrganizationAndPolicyRoundTrip) {
  const char *uri = std::getenv("LAKE_TEST_DB_URI");
  if (uri == nullptr || std::string(uri).empty()) {
    GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
  }

  auto store = lake::control::make_postgres_store(uri);
  ASSERT_TRUE(store->ping()) << store->ping().error().message;
  Registry registry;
  lake::api::register_core_capabilities(registry, "test");
  ApiContext context;
  context.store = store.get();
  context.authorized = true;
  context.acting_identity = "admin";
  context.request_id = "postgres-integration";
  context.instance_kek = "postgres-test-instance-kek";
  auto call = [&](const std::string &name, json input = json::object()) {
    return registry.call(name, input, context);
  };
  const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);

  ASSERT_TRUE(call("server_info").ok);
  const ApiResult first_user =
      call("create_user", {{"taap_id", "taap:1:ed25519:first-" + suffix},
                           {"email", "first-" + suffix + "@example.com"},
                           {"display_name", "First"}});
  const ApiResult second_user =
      call("create_user", {{"taap_id", "taap:1:ed25519:second-" + suffix},
                           {"email", "second-" + suffix + "@example.com"},
                           {"display_name", "Second"}});
  ASSERT_TRUE(first_user.ok) << first_user.error.message;
  ASSERT_TRUE(second_user.ok) << second_user.error.message;
  ASSERT_TRUE(call("list_users").ok);
  ASSERT_TRUE(call("get_user", {{"id", first_user.value.at("id")}}).ok);

  const ApiResult organization = call(
      "create_organization", {{"taap_id", "taap:1:ed25519:org-" + suffix},
                              {"display_name", "Integration organization"}});
  ASSERT_TRUE(organization.ok) << organization.error.message;
  ASSERT_TRUE(call("list_organizations").ok);
  ASSERT_TRUE(
      call("get_organization", {{"id", organization.value.at("id")}}).ok);

  const ApiResult customer =
      call("create_customer", {{"customer_type", "organization"},
                               {"organization_id", organization.value.at("id")},
                               {"name", "org-" + suffix}});
  ASSERT_TRUE(customer.ok) << customer.error.message;
  ASSERT_TRUE(call("list_customers").ok);
  ASSERT_TRUE(call("get_customer", {{"id", customer.value.at("id")}}).ok);

  const ApiResult repository =
      call("create_repository", {{"customer_id", customer.value.at("id")},
                                 {"name", "repo-" + suffix}});
  ASSERT_TRUE(repository.ok) << repository.error.message;
  ASSERT_TRUE(
      call("list_repositories", {{"customer_id", customer.value.at("id")}}).ok);
  ASSERT_TRUE(call("get_repository", {{"id", repository.value.at("id")}}).ok);

  const ApiResult destination_user =
      call("create_user", {{"display_name", "Repository destination"}});
  ASSERT_TRUE(destination_user.ok) << destination_user.error.message;
  const ApiResult destination =
      call("create_customer", {{"customer_type", "individual"},
                               {"user_id", destination_user.value.at("id")},
                               {"name", "destination-" + suffix}});
  ASSERT_TRUE(destination.ok) << destination.error.message;
  ASSERT_TRUE(
      call("set_repository_visibility",
           {{"id", repository.value.at("id")}, {"visibility", "organization"}})
          .ok);
  ASSERT_TRUE(call("rename_repository", {{"id", repository.value.at("id")},
                                         {"name", "renamed-" + suffix}})
                  .ok);
  const ApiResult transferred = call(
      "transfer_repository", {{"id", repository.value.at("id")},
                              {"customer_id", destination.value.at("id")}});
  ASSERT_TRUE(transferred.ok) << transferred.error.message;
  EXPECT_EQ(transferred.value.at("customer_id"), destination.value.at("id"));
  const ApiResult deleted =
      call("delete_repository", {{"id", repository.value.at("id")}});
  ASSERT_TRUE(deleted.ok) << deleted.error.message;
  EXPECT_FALSE(deleted.value.at("deleted_at").is_null());
  EXPECT_EQ(call("rename_repository", {{"id", repository.value.at("id")},
                                       {"name", "again-" + suffix}})
                .error.code,
            "conflict");
  const ApiResult audit = call("audit", {{"limit", 1000}});
  ASSERT_TRUE(audit.ok) << audit.error.message;
  EXPECT_TRUE(
      std::ranges::any_of(audit.value.at("items"), [](const json &event) {
        return event.at("action") == "delete_repository" &&
               event.at("outcome") == "success";
      }));

  const ApiResult team =
      call("create_team", {{"organization_id", organization.value.at("id")},
                           {"name", "team-" + suffix},
                           {"display_name", "Integration team"}});
  ASSERT_TRUE(team.ok) << team.error.message;
  ASSERT_TRUE(
      call("list_teams", {{"organization_id", organization.value.at("id")}})
          .ok);
  ASSERT_TRUE(call("get_team", {{"id", team.value.at("id")}}).ok);

  const ApiResult membership =
      call("create_membership",
           {{"organization_id", organization.value.at("id")},
            {"principal_type", "user"},
            {"principal_id", first_user.value.at("id")},
            {"principal_taap_id", first_user.value.at("taap_id")},
            {"role", "owner"},
            {"attestation_digest", "sha512-256:owner-" + suffix}});
  ASSERT_TRUE(membership.ok) << membership.error.message;
  ASSERT_TRUE(call("list_memberships",
                   {{"organization_id", organization.value.at("id")}})
                  .ok);

  const ApiResult invitation = call(
      "create_invitation", {{"organization_id", organization.value.at("id")},
                            {"team_id", team.value.at("id")},
                            {"invitee_type", "email"},
                            {"invitee", "second-" + suffix + "@example.com"},
                            {"role", "member"},
                            {"expires_at", "2999-01-01T00:00:00Z"}});
  ASSERT_TRUE(invitation.ok) << invitation.error.message;
  ASSERT_TRUE(call("list_invitations",
                   {{"organization_id", organization.value.at("id")},
                    {"team_id", team.value.at("id")}})
                  .ok);
  const ApiResult accepted =
      call("accept_invitation",
           {{"invitation_id", invitation.value.at("id")},
            {"principal_type", "user"},
            {"principal_id", second_user.value.at("id")},
            {"principal_taap_id", second_user.value.at("taap_id")},
            {"attestation_digest", "sha512-256:accepted-" + suffix}});
  ASSERT_TRUE(accepted.ok) << accepted.error.message;

  const auto repository_object =
      store->find_policy_object("repository", repository.value.at("id"));
  ASSERT_TRUE(repository_object) << repository_object.error().message;
  const ApiResult path =
      call("create_object", {{"object_type", "path"},
                             {"external_id", "node-" + suffix},
                             {"parent_id", repository_object->id},
                             {"inherit", true}});
  ASSERT_TRUE(path.ok) << path.error.message;
  ASSERT_TRUE(call("get_object", {{"id", path.value.at("id")}}).ok);
  ASSERT_TRUE(call("list_objects", {{"parent_id", repository_object->id}}).ok);

  const ApiResult grant = call(
      "create_grant", {{"principal_taap_id", "taap:1:ed25519:reader-" + suffix},
                       {"object_id", repository_object->id},
                       {"role", "read"},
                       {"attestation_digest", "sha512-256:grant-" + suffix}});
  ASSERT_TRUE(grant.ok) << grant.error.message;
  ASSERT_TRUE(call("list_grants", {{"object_id", repository_object->id}}).ok);
  const ApiResult access =
      call("effective_access", {{"object_id", path.value.at("id")}});
  ASSERT_TRUE(access.ok) << access.error.message;
  EXPECT_FALSE(access.value.at("items").empty());
}

} // namespace
