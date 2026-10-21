// Deleting and revoking what Lake could only create before (lake#645, docs/api/deletion.md):
// organizations, invitations, grants and secret mounts in the control plane, and a workspace's
// datasets, models, runs, search indexes and Ask Orama conversations in its workspace Zetta. Every
// capability takes `dry_run: true`, which changes nothing and answers what would go. Each test
// runs over the in-memory store and, when LAKE_TEST_DB_URI is set, PostgreSQL.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/agent_task_records_api.hpp"
#include "api/workspace_records_api.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_task_records.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"
#include "postgres_fixture.hpp"
#include "workspace_fixture.hpp"

namespace {

using json = nlohmann::json;
using lake::api::ApiContext;
using lake::api::ApiResult;

constexpr std::string_view kek = "6465656c6574696f6e732d746573742d6b656b2d3634352d6c616b652d746573";

template <class Task> auto run(Task task) {
    return lake::control::settled(liquid::runtime::block_on(std::move(task)));
}

lake::control::ClerkIdentity person(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity(std::string(kek));
    return {subject,
            subject + "@example.test",
            "",
            "browser",
            identity->taap_id,
            identity->public_key,
            identity->wrapped_seed};
}

// People with personal spaces, their workspaces Ready in fake workspace Zettas, and the registry.
struct Lake {
    std::shared_ptr<lake::control::Store> store;
    lake::test::RecordWorkspaces workspaces;
    lake::api::Registry registry;

    struct Person {
        lake::control::Principal principal;
        std::string customer_id; // their personal space
        std::string secret;      // their session's bearer
    };

    Lake(std::shared_ptr<lake::control::Store> backing, std::string uri)
        : store(backing), workspaces(backing, std::move(uri)) {
        lake::api::register_core_capabilities(registry, "test");
    }

    Person person(const std::string& name) {
        const auto session =
            store->sign_in_clerk(::person(name + "-" + lake::control::new_uuid_v4()));
        EXPECT_TRUE(session) << session.error().message;
        Person someone;
        someone.principal = *store->context_for(session->secret);
        someone.secret = session->secret;
        someone.customer_id = store->list_customers(10, someone.principal.user_id)->front().id;
        return someone;
    }

    ApiContext context(const Person& someone) {
        ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.instance_kek = std::string(kek);
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = someone.principal.user_id;
        context.acting_identity = someone.principal.taap_id;
        context.credential_session_id = someone.principal.session_id;
        context.scopes = someone.principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        return context;
    }

    ApiContext operator_context() {
        ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.instance_kek = std::string(kek);
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.request_id = lake::control::new_uuid_v4();
        return context;
    }

    ApiResult call(ApiContext context, const std::string& capability, json input) {
        const auto completed =
            liquid::runtime::block_on(registry.call_async(capability, input, context));
        EXPECT_TRUE(completed);
        return completed ? *completed : ApiResult::failure("store_error", "incomplete");
    }
    ApiResult call(const Person& someone, const std::string& capability, json input) {
        return call(context(someone), capability, std::move(input));
    }
};

class Deletions : public testing::TestWithParam<bool> {
protected:
    std::shared_ptr<lake::control::Store> make_store() {
        if (!GetParam()) {
            return std::make_shared<lake::control::InMemoryStore>();
        }
        const char* uri = lake::test::postgres_uri();
        if (uri == nullptr || *uri == '\0') {
            return nullptr;
        }
        database_uri_ = uri;
        return lake::control::make_postgres_store(database_uri_, std::string(kek));
    }
    std::string database_uri_;
};

#define LAKE_STORE_OR_SKIP(store)                                                                  \
    auto store = make_store();                                                                     \
    if (!store) {                                                                                  \
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";                                      \
    }

std::string later() {
    return "2099-01-01T00:00:00Z";
}

} // namespace

TEST(Deletions, EachIsAuditedInTheLogItsSubjectBelongsTo) {
    using lake::api::AuditLog;
    for (const char* central : {"delete_organization", "revoke_invitation", "revoke_grant"}) {
        EXPECT_EQ(lake::api::audit_log_of(central), AuditLog::Central) << central;
    }
    for (const char* workspace : {"delete_dataset",
                                  "delete_model",
                                  "delete_run",
                                  "delete_index",
                                  "delete_ask_conversation",
                                  "delete_secret_mount"}) {
        EXPECT_EQ(lake::api::audit_log_of(workspace), AuditLog::Workspace) << workspace;
    }
}

TEST_P(Deletions, AnOrganizationIsDeletedOnlyOnceItOwnsNoWorkspace) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const auto colleague = lake.person("colleague");
    const auto space =
        store->create_organization_space({"Acme Analytics",
                                          "acme-" + lake::control::new_uuid_v4().substr(0, 8),
                                          owner.principal.user_id,
                                          owner.principal.taap_id,
                                          lake::control::new_uuid_v4()});
    ASSERT_TRUE(space) << space.error().message;
    const std::string organization = space->organization.id;
    const std::string handle = space->customer.name;
    ASSERT_TRUE(store->create_team({organization, "data", "Data", owner.principal.taap_id, ""}));
    ASSERT_TRUE(store->create_invitation({organization,
                                          "",
                                          "email",
                                          "new@example.test",
                                          "member",
                                          later(),
                                          owner.principal.taap_id,
                                          ""}));
    const std::string workspace = lake.workspaces.create(space->customer.id, "analytics");

    // The plan names the workspace in the way, and the deletion is refused while it lives.
    const auto plan = lake.call(
        owner, "delete_organization", {{"organization_id", organization}, {"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["dry_run"], true);
    EXPECT_EQ(plan.value["deleted"], false);
    EXPECT_EQ(plan.value["handle"], handle);
    ASSERT_EQ(plan.value["workspaces"].size(), 1U);
    EXPECT_EQ(plan.value["workspaces"][0]["name"], "analytics");
    EXPECT_EQ(plan.value["counts"]["teams"], 1);
    EXPECT_EQ(plan.value["counts"]["memberships"], 1);
    EXPECT_EQ(plan.value["counts"]["invitations"], 1);
    EXPECT_GE(plan.value["counts"]["grants"].get<int>(), 1);
    const auto blocked = lake.call(
        owner, "delete_organization", {{"organization_id", organization}, {"confirm", handle}});
    ASSERT_FALSE(blocked.ok);
    EXPECT_EQ(blocked.error.code, "conflict");
    EXPECT_NE(blocked.error.message.find("analytics"), std::string::npos)
        << "the refusal names the workspace: " << blocked.error.message;
    ASSERT_TRUE(store->get_organization(organization)) << "nothing changed";

    // A wrong confirmation, or someone who is not an owner, changes nothing either.
    ASSERT_TRUE(store->update_workspace(workspace, {.desired = "deleted"}));
    const auto unconfirmed = lake.call(
        owner, "delete_organization", {{"organization_id", organization}, {"confirm", "acme"}});
    EXPECT_EQ(unconfirmed.error.code, "invalid_input");
    EXPECT_EQ(unconfirmed.error.path, "/confirm");
    const auto stranger = lake.call(
        colleague, "delete_organization", {{"organization_id", organization}, {"confirm", handle}});
    EXPECT_EQ(stranger.error.code, "unauthorized");
    ASSERT_TRUE(store->get_organization(organization));

    const auto deleted = lake.call(
        owner, "delete_organization", {{"organization_id", organization}, {"confirm", handle}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["deleted"], true);
    EXPECT_FALSE(deleted.value["deleted_at"].is_null());
    EXPECT_TRUE(deleted.value["workspaces"].empty());

    // It and its space are gone from every read, its members and invitations with them, and
    // nobody holds access to the space any more.
    EXPECT_EQ(store->get_organization(organization).error().code, "not_found");
    EXPECT_EQ(store->organization_customer(organization).error().code, "not_found");
    EXPECT_EQ(store->get_customer(space->customer.id).error().code, "not_found");
    EXPECT_FALSE(store->find_policy_object("customer", space->customer.id));
    EXPECT_FALSE(store->find_policy_object("organization", organization));
    for (const auto& listed : *store->list_organizations(1000)) {
        EXPECT_NE(listed.id, organization);
    }
    for (const auto& listed : *store->list_customers(1000)) {
        EXPECT_NE(listed.id, space->customer.id);
    }
    EXPECT_TRUE(store->list_memberships(organization, "", 100)->empty());
    EXPECT_TRUE(store->list_invitations(organization, "", 100)->empty());
    EXPECT_TRUE(store->list_user_memberships(owner.principal.user_id, 100)->empty());
    const auto reread = lake.call(owner, "get_customer", {{"customer_id", space->customer.id}});
    EXPECT_FALSE(reread.ok);
    // The owner's own account is untouched.
    EXPECT_TRUE(store->get_customer(owner.customer_id));
    // A second delete finds nothing.
    EXPECT_EQ(lake.call(owner,
                        "delete_organization",
                        {{"organization_id", organization}, {"confirm", handle}})
                  .error.code,
              "not_found");
}

TEST_P(Deletions, APendingInvitationIsRevokedAndCannotBeAccepted) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const auto invited = lake.person("invited");
    const auto space = store->create_organization_space({"Revokers",
                                                         "",
                                                         owner.principal.user_id,
                                                         owner.principal.taap_id,
                                                         lake::control::new_uuid_v4()});
    ASSERT_TRUE(space) << space.error().message;
    const auto invitation = store->create_invitation({space->organization.id,
                                                      "",
                                                      "email",
                                                      "invited@example.test",
                                                      "member",
                                                      later(),
                                                      owner.principal.taap_id,
                                                      ""});
    ASSERT_TRUE(invitation) << invitation.error().message;

    const auto plan = lake.call(
        owner, "revoke_invitation", {{"invitation_id", invitation->id}, {"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["status"], "pending");
    EXPECT_EQ(plan.value["dry_run"], true);
    EXPECT_EQ(store->get_invitation(invitation->id)->status, "pending")
        << "a dry run changes nothing";

    EXPECT_EQ(
        lake.call(invited, "revoke_invitation", {{"invitation_id", invitation->id}}).error.code,
        "unauthorized")
        << "only an administrator of the organization revokes";
    const auto revoked = lake.call(owner, "revoke_invitation", {{"invitation_id", invitation->id}});
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_EQ(revoked.value["status"], "revoked");
    EXPECT_EQ(store->get_invitation(invitation->id)->status, "revoked");

    const auto again = lake.call(owner, "revoke_invitation", {{"invitation_id", invitation->id}});
    EXPECT_EQ(again.error.code, "conflict");
    const auto accepted = store->accept_invitation({invitation->id,
                                                    "user",
                                                    invited.principal.user_id,
                                                    invited.principal.taap_id,
                                                    "digest",
                                                    invited.principal.taap_id,
                                                    ""});
    EXPECT_FALSE(accepted) << "a revoked invitation cannot be accepted";
    EXPECT_EQ(
        lake.call(owner, "revoke_invitation", {{"invitation_id", lake::control::new_uuid_v4()}})
            .error.path,
        "/invitation_id");
}

TEST_P(Deletions, AGrantIsRevokedUnlessAMembershipOrTheLastAdminNeedsIt) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const auto reader = lake.person("reader");
    const auto space_object = store->find_policy_object("customer", owner.customer_id);
    ASSERT_TRUE(space_object);
    const auto granted = lake.call(owner,
                                   "create_grant",
                                   {{"principal_taap_id", reader.principal.taap_id},
                                    {"object_id", space_object->id},
                                    {"role", "read"}});
    ASSERT_TRUE(granted.ok) << granted.error.message;
    const std::string grant = granted.value["id"];
    ASSERT_TRUE(lake.call(reader, "get_customer", {{"customer_id", owner.customer_id}}).ok);

    const auto plan = lake.call(owner, "revoke_grant", {{"grant_id", grant}, {"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["role"], "read");
    EXPECT_TRUE(store->get_grant(grant)) << "a dry run changes nothing";
    EXPECT_EQ(lake.call(reader, "revoke_grant", {{"grant_id", grant}}).error.code, "unauthorized")
        << "revoking needs admin on the grant's object";
    ASSERT_TRUE(lake.call(owner, "revoke_grant", {{"grant_id", grant}}).ok);
    EXPECT_EQ(store->get_grant(grant).error().code, "not_found");
    EXPECT_FALSE(lake.call(reader, "get_customer", {{"customer_id", owner.customer_id}}).ok)
        << "the reader lost access with the grant";

    // The space's only admin grant is kept, so the space is never left without an administrator.
    const auto admins = store->list_grants({space_object->id}, owner.principal.taap_id, 10);
    ASSERT_TRUE(admins);
    const auto admin = std::ranges::find(*admins, "admin", &lake::control::Grant::role);
    ASSERT_NE(admin, admins->end());
    const auto last = lake.call(owner, "revoke_grant", {{"grant_id", admin->id}});
    EXPECT_EQ(last.error.code, "conflict");
    EXPECT_TRUE(store->get_grant(admin->id));

    // A grant that comes with a membership goes with the membership, not on its own.
    const auto organization = store->create_organization_space({"Members",
                                                                "",
                                                                owner.principal.user_id,
                                                                owner.principal.taap_id,
                                                                lake::control::new_uuid_v4()});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto org_object = store->find_policy_object("customer", organization->customer.id);
    const auto derived = store->list_grants({org_object->id}, owner.principal.taap_id, 10);
    ASSERT_TRUE(derived);
    ASSERT_FALSE(derived->empty());
    const auto membership_grant = std::ranges::find_if(*derived, [&](const auto& value) {
        return value.attestation_digest == organization->owner.attestation_digest;
    });
    ASSERT_NE(membership_grant, derived->end());
    const auto refused = lake.call(owner, "revoke_grant", {{"grant_id", membership_grant->id}});
    EXPECT_EQ(refused.error.code, "conflict");
    EXPECT_NE(refused.error.message.find("member"), std::string::npos) << refused.error.message;
}

TEST_P(Deletions, ASecretMountLeavesNothingInItsWorkspace) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const std::string workspace = lake.workspaces.create(owner.customer_id, "vault");
    const std::string other = lake.workspaces.create(owner.customer_id, "other");
    const auto admin = [&](const std::string& capability, json input) {
        return lake.call(lake.operator_context(), capability, std::move(input));
    };
    std::vector<std::string> mounts;
    for (const auto& where : {workspace, workspace, other}) {
        const auto mount = admin("create_secret_mount",
                                 {{"customer_id", owner.customer_id},
                                  {"workspace_id", where},
                                  {"name", "vault-" + std::to_string(mounts.size())}});
        ASSERT_TRUE(mount.ok) << mount.error.message;
        mounts.push_back(mount.value["id"]);
        ASSERT_TRUE(admin("put_secret",
                          {{"mount_id", mounts.back()},
                           {"name", "database"},
                           {"value", {{"password", "first"}}}})
                        .ok);
        ASSERT_TRUE(admin("put_secret",
                          {{"mount_id", mounts.back()},
                           {"name", "database"},
                           {"value", {{"password", "second"}}}})
                        .ok);
        ASSERT_TRUE(
            admin("create_encryption_key", {{"mount_id", mounts.back()}, {"name", "orders"}}).ok);
    }
    auto& tables = lake.workspaces.records(workspace);
    const auto size = [&](const char* collection) { return tables.rows(collection).size(); };
    ASSERT_EQ(size("secrets_mounts"), 4U) << "two mounts and their name claims";
    ASSERT_EQ(size("secrets_versions"), 4U);

    const auto plan =
        lake.call(owner, "delete_secret_mount", {{"mount_id", mounts[0]}, {"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["name"], "vault-0");
    EXPECT_EQ(plan.value["counts"]["secrets"], 1);
    EXPECT_EQ(plan.value["counts"]["versions"], 2);
    EXPECT_EQ(plan.value["counts"]["encryption_keys"], 1);
    EXPECT_EQ(plan.value["counts"]["data_keys"], 2) << "the mount's and its key's";
    EXPECT_EQ(plan.value["secrets"], json::array({"database"}));
    EXPECT_EQ(size("secrets_versions"), 4U) << "a dry run changes nothing";

    const auto unconfirmed =
        lake.call(owner, "delete_secret_mount", {{"mount_id", mounts[0]}, {"confirm", "x"}});
    EXPECT_EQ(unconfirmed.error.code, "invalid_input");
    EXPECT_EQ(unconfirmed.error.path, "/confirm");
    const auto deleted =
        lake.call(owner, "delete_secret_mount", {{"mount_id", mounts[0]}, {"confirm", "vault-0"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["deleted"], true);

    // Only the other mount's records are left in the workspace; the other workspace is untouched.
    EXPECT_EQ(size("secrets_mounts"), 2U);
    EXPECT_EQ(size("secrets_entries"), 1U);
    EXPECT_EQ(size("secrets_versions"), 2U);
    EXPECT_EQ(size("secrets_encryption_keys"), 1U);
    EXPECT_EQ(size("secrets_data_keys"), 2U);
    EXPECT_EQ(lake.workspaces.records(other).rows("secrets_versions").size(), 2U);
    EXPECT_EQ(store->get_secret_mount_route(mounts[0]).error().code, "not_found");
    EXPECT_FALSE(store->find_policy_object("secret_mount", mounts[0]));
    EXPECT_EQ(admin("get_secret_mount", {{"mount_id", mounts[0]}}).error.code, "not_found");
    ASSERT_TRUE(admin("read_secret", {{"mount_id", mounts[1]}, {"name", "database"}}).ok)
        << "the mount beside it still opens";

    // Its name is free again in that workspace.
    EXPECT_TRUE(
        admin(
            "create_secret_mount",
            {{"customer_id", owner.customer_id}, {"workspace_id", workspace}, {"name", "vault-0"}})
            .ok);
}

TEST_P(Deletions, AnAskConversationIsRemovedFromItsWorkspaceForItsPersonOnly) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const std::string workspace = lake.workspaces.create(owner.customer_id, "home");
    const std::string conversation = lake::control::new_uuid_v4();
    const std::string kept = lake::control::new_uuid_v4();
    {
        auto context = lake.context(owner);
        auto records = lake::api::workspace_records(context, owner.customer_id, workspace);
        ASSERT_TRUE(records) << records.error().message;
        lake::control::AskMessages messages(*records);
        for (const auto& [id, text] : std::vector<std::pair<std::string, std::string>>{
                 {conversation, "what churns?"}, {conversation, "the answer"}, {kept, "other"}}) {
            ASSERT_TRUE(run(messages.append(
                {"", id, owner.customer_id, owner.principal.user_id, "user", text, nullptr, ""})));
        }
    }
    const json in{{"customer_id", owner.customer_id},
                  {"workspace_id", workspace},
                  {"conversation_id", conversation}};
    json dry = in;
    dry["dry_run"] = true;
    const auto plan = lake.call(owner, "delete_ask_conversation", dry);
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["messages"], 2);
    EXPECT_EQ(plan.value["first_message"], "what churns?");
    EXPECT_EQ(lake.workspaces.records(workspace).rows("ask_messages").size(), 3U);

    const auto deleted = lake.call(owner, "delete_ask_conversation", in);
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["deleted"], true);
    EXPECT_EQ(lake.workspaces.records(workspace).rows("ask_messages").size(), 1U)
        << "only the other conversation is left in the workspace Zetta";
    EXPECT_TRUE(lake.call(owner, "get_ask_conversation", in).value["messages"].empty());
    EXPECT_EQ(lake.call(owner, "delete_ask_conversation", in).error.code, "not_found");

    // Someone else cannot reach it: another person's conversation id finds nothing of theirs.
    const auto stranger = lake.person("stranger");
    json theirs = in;
    theirs["conversation_id"] = kept;
    EXPECT_EQ(lake.call(stranger, "delete_ask_conversation", theirs).error.code, "unauthorized");
    EXPECT_EQ(lake.workspaces.records(workspace).rows("ask_messages").size(), 1U);
}

// lake#673: the agent tasks a conversation started go with it: each task's record (its request
// repeats the person's message) from the workspace Zetta and its dispatch row from the control
// store, with the task its turn asked for. Other conversations' tasks, and a task whose record is
// already gone, still answer get_agent_task.
TEST_P(Deletions, AnAskConversationTakesTheAgentTasksItStartedWithIt) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const std::string workspace = lake.workspaces.create(owner.customer_id, "home");
    const std::string conversation = lake::control::new_uuid_v4();
    const std::string kept = lake::control::new_uuid_v4();
    auto context = lake.context(owner);
    {
        auto records = lake::api::workspace_records(context, owner.customer_id, workspace);
        ASSERT_TRUE(records) << records.error().message;
        lake::control::AskMessages messages(*records);
        for (const auto& [id, text] : std::vector<std::pair<std::string, std::string>>{
                 {conversation, "which customers churn?"}, {kept, "other"}}) {
            ASSERT_TRUE(run(messages.append(
                {"", id, owner.customer_id, owner.principal.user_id, "user", text, nullptr, ""})));
        }
    }
    auto task = [&](const std::string& conversation_id, const std::string& kind,
                    const std::string& agent, const std::string& parent) {
        lake::control::AgentTask asked{.id = lake::control::new_uuid_v4(),
                                       .customer_id = owner.customer_id,
                                       .agent = agent,
                                       .kind = kind,
                                       .conversation_id = conversation_id,
                                       .workspace_id = workspace,
                                       .parent_task_id = parent,
                                       .body = {{"message", "which customers churn?"}},
                                       .scopes = {"workspace_query"},
                                       .requested_by_user_id = owner.principal.user_id,
                                       .requested_by_taap_id = owner.principal.taap_id};
        EXPECT_TRUE(run(lake::api::write_agent_task_request(context, asked)));
        const auto created = store->create_agent_task(asked);
        EXPECT_TRUE(created) << created.error().message;
        return asked.id;
    };
    const std::string turn = task(conversation, "ask", "agent-ask-orama", "");
    const std::string delegated = task("", "api", "agent-autosuggest", turn);
    const std::string second = task(conversation, "ask", "agent-ask-orama", "");
    const std::string other = task(kept, "ask", "agent-ask-orama", "");
    const std::string build = task(conversation, "build:churn", "agent-project", "");
    ASSERT_EQ(lake.workspaces.records(workspace).rows("agent_tasks").size(), 5U);

    const json in{{"customer_id", owner.customer_id},
                  {"workspace_id", workspace},
                  {"conversation_id", conversation}};
    json dry = in;
    dry["dry_run"] = true;
    const auto plan = lake.call(owner, "delete_ask_conversation", dry);
    ASSERT_TRUE(plan.ok) << plan.error.message;
    std::vector<std::string> listed;
    for (const json& item : plan.value["agent_tasks"]) {
        listed.push_back(item["task_id"]);
        EXPECT_EQ(item["status"], "pending");
    }
    std::ranges::sort(listed);
    std::vector<std::string> expected{turn, delegated, second};
    std::ranges::sort(expected);
    EXPECT_EQ(listed, expected) << plan.value.dump();
    EXPECT_EQ(lake.workspaces.records(workspace).rows("agent_tasks").size(), 5U)
        << "a dry run removes nothing";
    EXPECT_TRUE(store->get_agent_task(turn));

    const auto deleted = lake.call(owner, "delete_ask_conversation", in);
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["agent_tasks"].size(), 3U);
    EXPECT_EQ(lake.workspaces.records(workspace).rows("agent_tasks").size(), 2U)
        << "only the other conversation's turn and the project build keep their records";
    for (const std::string& id : expected) {
        EXPECT_EQ(store->get_agent_task(id).error().code, "not_found") << id;
        EXPECT_EQ(lake.call(owner, "get_agent_task",
                            {{"customer_id", owner.customer_id},
                             {"workspace_id", workspace},
                             {"task_id", id}})
                      .error.code,
                  "not_found");
    }
    EXPECT_TRUE(store->get_agent_task(build)) << "a task of another kind is not the conversation's";
    EXPECT_EQ(lake.call(owner, "delete_ask_conversation", in).error.code, "not_found");

    // A task whose record is gone still answers in Agent Tasks, with nothing to show.
    {
        auto records = lake::api::workspace_records(context, owner.customer_id, workspace);
        ASSERT_TRUE(records);
        ASSERT_TRUE(run(records->remove(lake::control::AgentTaskContentCodec::collection, other)));
    }
    const auto followed = lake.call(owner, "get_agent_task",
                                    {{"customer_id", owner.customer_id},
                                     {"workspace_id", workspace},
                                     {"task_id", other}});
    ASSERT_TRUE(followed.ok) << followed.error.message;
    EXPECT_EQ(followed.value["status"], "pending");
}

TEST_P(Deletions, EngineEntitiesAreDeletedThroughTheirNamedCapabilities) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto owner = lake.person("owner");
    const std::string workspace = lake.workspaces.create(owner.customer_id, "train");
    json seen = json::array();
    lake.workspaces.zetta->responder =
        [&](const std::string& capability,
            const json& input) -> std::optional<lake::control::ZettaControlResult<json>> {
        if (!capability.starts_with("delete_")) {
            return std::nullopt;
        }
        seen.push_back({{"capability", capability}, {"input", input}});
        if (input.contains("model_id") && input["model_id"] == "missing") {
            return lake::control::ZettaControlResult<json>(std::unexpected(
                lake::control::ZettaControlError{"not_found", "no model 'missing'"}));
        }
        return json{{"dry_run", input.value("dry_run", false)}, {"removed", {{"rows", 12}}}};
    };
    const auto call = [&](const std::string& capability, json extra) {
        json input{{"customer_id", owner.customer_id}, {"workspace_id", workspace}};
        input.update(extra);
        return lake.call(owner, capability, input);
    };

    const auto dataset = call("delete_dataset", {{"id", "main/orders"}, {"dry_run", true}});
    ASSERT_TRUE(dataset.ok) << dataset.error.message;
    EXPECT_EQ(dataset.value["dry_run"], true);
    EXPECT_EQ(dataset.value["deleted"], false);
    EXPECT_EQ(dataset.value["removed"]["rows"], 12);
    EXPECT_EQ(seen.back()["input"],
              json({{"dataset", "orders"}, {"database", "main"}, {"dry_run", true}}));

    ASSERT_TRUE(call("delete_model", {{"model_id", "m-1"}}).ok) << "model_id is accepted too";
    EXPECT_EQ(seen.back()["input"], json({{"model_id", "m-1"}, {"dry_run", false}}));
    ASSERT_TRUE(call("delete_run", {{"id", "r-1"}}).ok);
    EXPECT_EQ(seen.back()["input"]["run_id"], "r-1");
    const auto index = call("delete_index", {{"id", "i-1"}});
    ASSERT_TRUE(index.ok);
    EXPECT_EQ(index.value["deleted"], true);
    EXPECT_EQ(seen.back()["input"]["index_id"], "i-1");

    const auto missing = call("delete_model", {{"id", "missing"}});
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_EQ(missing.error.path, "/id");
    EXPECT_EQ(call("delete_dataset", {{"id", ""}}).error.code, "invalid_input");

    // An engine older than these capabilities says so; nothing is deleted.
    lake.workspaces.zetta->responder =
        [](const std::string& capability,
           const json&) -> std::optional<lake::control::ZettaControlResult<json>> {
        if (!capability.starts_with("delete_")) {
            return std::nullopt;
        }
        return lake::control::ZettaControlResult<json>(
            std::unexpected(lake::control::ZettaControlError{
                "unknown_capability", "no capability named '" + capability + "'"}));
    };
    const auto older = call("delete_run", {{"id", "r-1"}});
    EXPECT_EQ(older.error.code, "not_supported");
    EXPECT_NE(older.error.message.find("runs"), std::string::npos) << older.error.message;

    // Another space's workspace is refused before the engine is called.
    const auto stranger = lake.person("stranger");
    const auto trespass = lake.call(
        stranger,
        "delete_dataset",
        {{"customer_id", owner.customer_id}, {"workspace_id", workspace}, {"id", "main/orders"}});
    EXPECT_EQ(trespass.error.code, "unauthorized");
}

INSTANTIATE_TEST_SUITE_P(Stores,
                         Deletions,
                         testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

// ---- deleting an account (lake#810, docs/api/identity.md#deleting-an-account) ----

namespace {

// `who` joins `organization` with `role`, organization-wide.
void join(lake::control::Store& store,
          const std::string& organization,
          const lake::control::Principal& who,
          const std::string& role,
          const lake::control::Principal& by) {
    const auto joined = store.create_membership({organization,
                                                 {},
                                                 "user",
                                                 who.user_id,
                                                 who.taap_id,
                                                 role,
                                                 "sha512-256:" + role + "-" + who.user_id,
                                                 by.taap_id,
                                                 lake::control::new_uuid_v4()});
    ASSERT_TRUE(joined) << joined.error().message;
}

lake::control::OrganizationSpace organization_of(lake::control::Store& store,
                                                 const std::string& name,
                                                 const lake::control::Principal& owner) {
    const auto space = store.create_organization_space(
        {name,
         name + "-" + lake::control::new_uuid_v4().substr(0, 8),
         owner.user_id,
         owner.taap_id,
         lake::control::new_uuid_v4()});
    EXPECT_TRUE(space) << space.error().message;
    return *space;
}

std::string handle_of(lake::control::Store& store, const std::string& customer_id) {
    return store.get_customer(customer_id)->name;
}

bool live(lake::control::Store& store, const std::string& workspace_id) {
    const auto workspace = store.get_workspace(workspace_id);
    return workspace && workspace->desired != "deleted";
}

} // namespace

TEST(Deletions, AnAccountDeletionIsAuditedCentrally) {
    EXPECT_EQ(lake::api::audit_log_of("delete_account"), lake::api::AuditLog::Central);
}

TEST_P(Deletions, OneAccountCannotDeleteAnother) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto alice = lake.person("alice");
    const auto bob = lake.person("bob");
    const std::string bobs = handle_of(*store, bob.customer_id);

    for (const bool dry_run : {true, false}) {
        const auto refused = lake.call(
            alice,
            "delete_account",
            {{"user_id", bob.principal.user_id}, {"confirm", bobs}, {"dry_run", dry_run}});
        ASSERT_FALSE(refused.ok);
        EXPECT_EQ(refused.error.code, "unauthorized") << refused.error.message;
    }
    EXPECT_TRUE(store->get_user(bob.principal.user_id));
    EXPECT_TRUE(store->get_customer(bob.customer_id));
    EXPECT_TRUE(store->context_for(bob.secret));

    // A token, however new, is not a sign-in: it cannot delete its own account either.
    const auto token = store->create_token({alice.principal.user_id,
                                            "lake",
                                            "cleanup",
                                            "",
                                            {"*"},
                                            3600,
                                            false,
                                            alice.principal.session_id});
    ASSERT_TRUE(token) << token.error().message;
    auto with_token = lake.context(alice);
    with_token.credential_session_id = token->session.id;
    const auto by_token = lake.call(with_token,
                                    "delete_account",
                                    {{"confirm", handle_of(*store, alice.customer_id)}});
    ASSERT_FALSE(by_token.ok);
    EXPECT_EQ(by_token.error.code, "unauthorized");
    EXPECT_TRUE(store->get_user(alice.principal.user_id));
    // Nor does a wrong MFA code stand in for a sign-in.
    const auto wrong_code = lake.call(with_token,
                                      "delete_account",
                                      {{"confirm", handle_of(*store, alice.customer_id)},
                                       {"code", "000000"}});
    ASSERT_FALSE(wrong_code.ok);
    EXPECT_EQ(wrong_code.error.path, "/code");
    EXPECT_TRUE(store->get_user(alice.principal.user_id));
}

TEST_P(Deletions, AnAccountDeletionRemovesExactlyWhatItShould) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto alice = lake.person("alice");
    const auto bob = lake.person("bob");
    const std::string handle = handle_of(*store, alice.customer_id);

    // Alice: a workspace of her own, an organization she alone owns (with a workspace), one she
    // owns with Bob, and a seat in Bob's organization. Bob: his own workspace and organization.
    const std::string own_workspace = lake.workspaces.create(alice.customer_id, "alice-dev");
    const auto solo = organization_of(*store, "solo", alice.principal);
    const std::string solo_workspace = lake.workspaces.create(solo.customer.id, "solo-dev");
    const auto shared = organization_of(*store, "shared", alice.principal);
    join(*store, shared.organization.id, bob.principal, "owner", alice.principal);
    const std::string shared_workspace = lake.workspaces.create(shared.customer.id, "shared-dev");
    const auto bobs_org = organization_of(*store, "bobs", bob.principal);
    join(*store, bobs_org.organization.id, alice.principal, "member", bob.principal);
    const std::string bob_workspace = lake.workspaces.create(bob.customer_id, "bob-dev");
    ASSERT_TRUE(store->set_billing_contact(
        {.customer_id = alice.customer_id, .email = "billing@alice.example.test"}));
    ASSERT_TRUE(store->set_billing_contact(
        {.customer_id = bob.customer_id, .email = "billing@bob.example.test"}));

    // The dry run lists what goes and changes nothing.
    const auto plan = lake.call(alice, "delete_account", {{"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value["deleted"], false);
    EXPECT_EQ(plan.value["handle"], handle);
    ASSERT_EQ(plan.value["workspaces"].size(), 1U);
    EXPECT_EQ(plan.value["workspaces"][0]["name"], "alice-dev");
    std::map<std::string, std::string> actions;
    for (const json& organization : plan.value["organizations"]) {
        actions[organization["organization_id"]] = organization["action"];
    }
    EXPECT_EQ(actions[solo.organization.id], "delete");
    EXPECT_EQ(actions[shared.organization.id], "leave");
    EXPECT_EQ(actions[bobs_org.organization.id], "leave");
    EXPECT_EQ(plan.value["counts"]["memberships"], 3);
    EXPECT_EQ(plan.value["counts"]["billing_contacts"], 1);
    EXPECT_GE(plan.value["counts"]["sessions"].get<int>(), 1);
    EXPECT_TRUE(live(*store, own_workspace));
    EXPECT_TRUE(store->get_user(alice.principal.user_id));

    // A wrong confirmation changes nothing.
    EXPECT_EQ(lake.call(alice, "delete_account", {{"confirm", "alice"}}).error.path, "/confirm");
    EXPECT_TRUE(live(*store, own_workspace));

    const auto deleted = lake.call(alice, "delete_account", {{"confirm", handle}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["deleted"], true);
    EXPECT_FALSE(deleted.value["deleted_at"].is_null());
    EXPECT_EQ(deleted.value["deleted_workspaces"].size(), 2U);
    ASSERT_EQ(deleted.value["deleted_organizations"].size(), 1U);
    EXPECT_EQ(deleted.value["deleted_organizations"][0]["organization_id"], solo.organization.id);

    // Alice's account, space, sessions, memberships and grants are gone; her workspaces are
    // deleted through the operator's desired state, and her organization with its workspace.
    EXPECT_EQ(store->get_user(alice.principal.user_id).error().code, "not_found");
    for (const auto& user : *store->list_users(1000)) {
        EXPECT_NE(user.id, alice.principal.user_id);
    }
    EXPECT_EQ(store->get_customer(alice.customer_id).error().code, "not_found");
    EXPECT_FALSE(store->find_policy_object("customer", alice.customer_id));
    EXPECT_FALSE(store->context_for(alice.secret));
    EXPECT_TRUE(store->list_sessions(alice.principal.user_id, "lake")->empty());
    EXPECT_TRUE(store->list_user_memberships(alice.principal.user_id, 100)->empty());
    EXPECT_FALSE(live(*store, own_workspace));
    EXPECT_FALSE(live(*store, solo_workspace));
    EXPECT_EQ(store->get_organization(solo.organization.id).error().code, "not_found");
    EXPECT_FALSE(store->billing_contact(alice.customer_id) &&
                 store->billing_contact(alice.customer_id)->has_value());
    EXPECT_TRUE(store->principal_names({alice.principal.taap_id})->empty());

    // What is shared is left to its other owners, and Bob is untouched.
    ASSERT_TRUE(store->get_organization(shared.organization.id));
    EXPECT_TRUE(live(*store, shared_workspace));
    const auto shared_members = store->list_memberships(shared.organization.id, "", 100);
    ASSERT_EQ(shared_members->size(), 1U);
    EXPECT_EQ(shared_members->front().principal_taap_id, bob.principal.taap_id);
    EXPECT_EQ(shared_members->front().role, "owner");
    ASSERT_TRUE(store->get_organization(bobs_org.organization.id));
    EXPECT_EQ(store->list_memberships(bobs_org.organization.id, "", 100)->size(), 1U);
    EXPECT_TRUE(store->get_user(bob.principal.user_id));
    EXPECT_TRUE(store->get_customer(bob.customer_id));
    EXPECT_TRUE(store->context_for(bob.secret));
    EXPECT_TRUE(live(*store, bob_workspace));
    EXPECT_TRUE(store->billing_contact(bob.customer_id)->has_value());
    const auto bob_still = lake.call(
        bob, "list_workspaces", {{"customer_id", shared.customer.id}});
    EXPECT_TRUE(bob_still.ok) << bob_still.error.message;

    // The central log keeps one event for the deletion, naming the account by its id only.
    const auto events = store->list_audit_events(
        lake::audit::Filter{.object = "user/" + alice.principal.user_id, .limit = 10});
    ASSERT_TRUE(events);
    const auto event = std::ranges::find_if(*events, [](const lake::audit::Event& value) {
        return value.draft.kind == "account.delete" && value.draft.outcome == "success";
    });
    ASSERT_NE(event, events->end());
    EXPECT_EQ(event->draft.customer_id, alice.customer_id);
    EXPECT_EQ(event->draft.details.dump().find("@example.test"), std::string::npos);

    // A second delete finds nothing.
    EXPECT_EQ(lake.call(lake.operator_context(),
                        "delete_account",
                        {{"user_id", alice.principal.user_id}, {"confirm", handle}})
                  .error.code,
              "not_found");
}

TEST_P(Deletions, AnOrganizationIsNeverLeftWithoutAnOwner) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto alice = lake.person("alice");
    const auto bob = lake.person("bob");
    const std::string handle = handle_of(*store, alice.customer_id);
    const std::string own_workspace = lake.workspaces.create(alice.customer_id, "alice-dev");
    const auto acme = organization_of(*store, "acme", alice.principal);
    join(*store, acme.organization.id, bob.principal, "member", alice.principal);
    const std::string acme_workspace = lake.workspaces.create(acme.customer.id, "acme-dev");

    // Alice is Acme's only owner and Bob belongs to it: refused, naming it, and nothing changed.
    const auto plan = lake.call(alice, "delete_account", {{"dry_run", true}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    ASSERT_EQ(plan.value["organizations"].size(), 1U);
    EXPECT_EQ(plan.value["organizations"][0]["action"], "blocked");
    EXPECT_EQ(plan.value["organizations"][0]["other_members"], 1);
    const auto refused = lake.call(alice, "delete_account", {{"confirm", handle}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "conflict");
    EXPECT_NE(refused.error.message.find("acme"), std::string::npos) << refused.error.message;
    EXPECT_TRUE(store->get_user(alice.principal.user_id));
    EXPECT_TRUE(live(*store, own_workspace)) << "nothing is deleted before the refusal";
    EXPECT_TRUE(live(*store, acme_workspace));
    EXPECT_EQ(store->list_memberships(acme.organization.id, "", 100)->size(), 2U);

    // The store refuses too, whoever asks it: the last owner is never removed on its own.
    ASSERT_TRUE(store->update_workspace(own_workspace, {.desired = "deleted"}));
    const auto direct = store->delete_account(alice.principal.user_id, false);
    ASSERT_FALSE(direct);
    EXPECT_EQ(direct.error().code, "conflict");
    EXPECT_TRUE(store->get_user(alice.principal.user_id));

    // Once Bob is an owner too, Alice can go and Acme stays with him.
    for (const auto& membership : *store->list_memberships(acme.organization.id, "", 100)) {
        if (membership.principal_taap_id == bob.principal.taap_id) {
            ASSERT_TRUE(store->set_member_role(membership.id, "owner", alice.principal.taap_id));
        }
    }
    const auto after_promotion = lake.call(alice, "delete_account", {{"dry_run", true}});
    ASSERT_TRUE(after_promotion.ok);
    EXPECT_EQ(after_promotion.value["organizations"][0]["action"], "leave");
}

TEST_P(Deletions, TheOperatorDeletesAnAccountAndItsSoleOrganizationsWhenAsked) {
    LAKE_STORE_OR_SKIP(store);
    Lake lake(store, database_uri_);
    const auto alice = lake.person("alice");
    const auto bob = lake.person("bob");
    const std::string handle = handle_of(*store, alice.customer_id);
    const auto acme = organization_of(*store, "acme", alice.principal);
    join(*store, acme.organization.id, bob.principal, "member", alice.principal);
    const std::string acme_workspace = lake.workspaces.create(acme.customer.id, "acme-dev");

    const auto unnamed = lake.call(lake.operator_context(), "delete_account", {{"confirm", handle}});
    EXPECT_EQ(unnamed.error.code, "invalid_input");
    EXPECT_EQ(lake.call(lake.operator_context(),
                        "delete_account",
                        {{"user_id", alice.principal.user_id}, {"confirm", handle}})
                  .error.code,
              "conflict");
    const auto deleted = lake.call(lake.operator_context(),
                                   "delete_account",
                                   {{"user_id", alice.principal.user_id},
                                    {"confirm", handle},
                                    {"delete_organizations", true}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(store->get_user(alice.principal.user_id).error().code, "not_found");
    EXPECT_EQ(store->get_organization(acme.organization.id).error().code, "not_found");
    EXPECT_FALSE(live(*store, acme_workspace));
    EXPECT_TRUE(store->list_user_memberships(bob.principal.user_id, 100)->empty());
    EXPECT_TRUE(store->get_user(bob.principal.user_id));
    EXPECT_TRUE(store->get_customer(bob.customer_id));
}
