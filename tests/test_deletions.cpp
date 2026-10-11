// Deleting and revoking what Lake could only create before (lake#645, docs/api/deletion.md):
// organizations, invitations, grants and secret mounts in the control plane, and a workspace's
// datasets, models, runs, search indexes and Ask Orama conversations in its workspace Zetta. Every
// capability takes `dry_run: true`, which changes nothing and answers what would go. Each test
// runs over the in-memory store and, when LAKE_TEST_DB_URI is set, PostgreSQL.

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/workspace_records_api.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
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
