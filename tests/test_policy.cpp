// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/policy.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using lake::control::AuthorizationCache;
using lake::control::InMemoryStore;
using lake::control::NewGrant;
using lake::control::NewPolicyObject;
using nlohmann::json;

struct PolicyGraph {
    InMemoryStore store;
    std::string customer;
    std::string organization;
    std::string team;
    std::string repository;
    std::string cut;
    std::string below_cut;

    PolicyGraph() {
        customer = store
                       .create_policy_object(
                           NewPolicyObject{"customer", "customer-1", {}, "admin", "r1", true})
                       ->id;
        organization = store
                           .create_policy_object(NewPolicyObject{
                               "organization", "organization-1", customer, "admin", "r2", true})
                           ->id;
        team = store
                   .create_policy_object(
                       NewPolicyObject{"team", "team-1", organization, "admin", "r3", true})
                   ->id;
        repository = store
                         .create_policy_object(NewPolicyObject{
                             "repository", "repository-1", team, "admin", "r4", true})
                         ->id;
        cut = store
                  .create_policy_object(
                      NewPolicyObject{"path", "cut", repository, "admin", "r5", false})
                  ->id;
        below_cut =
            store.create_policy_object(NewPolicyObject{"path", "below", cut, "admin", "r6", true})
                ->id;
    }

    void grant(std::string principal, std::string object, std::string role) {
        ASSERT_TRUE(store.create_grant(NewGrant{std::move(principal),
                                                std::move(object),
                                                std::move(role),
                                                "sha512-256:test",
                                                "admin",
                                                "r7"}));
    }
};

} // namespace

TEST(Policy, InheritsDownTheGraphAndStopsAtACut) {
    PolicyGraph graph;
    graph.grant("taap:1:ed25519:reader", graph.customer, "read");

    AuthorizationCache cache;
    auto on_team =
        lake::control::authorize(graph.store, "taap:1:ed25519:reader", graph.team, "read", cache);
    ASSERT_TRUE(on_team);
    EXPECT_TRUE(on_team->allowed);
    EXPECT_TRUE(on_team->inherited);
    EXPECT_EQ(on_team->source_object_id, graph.customer);

    auto below_cut = lake::control::authorize(
        graph.store, "taap:1:ed25519:reader", graph.below_cut, "read", cache);
    ASSERT_TRUE(below_cut);
    EXPECT_FALSE(below_cut->allowed);

    graph.grant("taap:1:ed25519:local", graph.cut, "write");
    AuthorizationCache local_cache;
    auto local = lake::control::authorize(
        graph.store, "taap:1:ed25519:local", graph.below_cut, "write", local_cache);
    ASSERT_TRUE(local);
    EXPECT_TRUE(local->allowed);
    EXPECT_TRUE(local->inherited);
    EXPECT_EQ(local->source_object_id, graph.cut);
}

// lake#985: a decision made for an agent's credential is confined to its account and
// workspace, whatever the person it acts for may reach elsewhere.
TEST(Policy, AConfinedDecisionAllowsOnlyObjectsInsideItsAccountAndWorkspace) {
    PolicyGraph graph;
    const std::string reader = "taap:1:ed25519:reader";
    graph.grant(reader, graph.customer, "read");
    graph.grant(reader, graph.cut, "read");
    const auto other_customer = graph.store.create_policy_object(
        NewPolicyObject{"customer", "customer-2", {}, "admin", "r8", true});
    ASSERT_TRUE(other_customer);
    graph.grant(reader, other_customer->id, "read");

    AuthorizationCache unconfined;
    auto elsewhere =
        lake::control::authorize(graph.store, reader, other_customer->id, "read", unconfined);
    ASSERT_TRUE(elsewhere);
    EXPECT_TRUE(elsewhere->allowed) << "the person may read the other account";

    AuthorizationCache confined;
    confined.confinement = {.active = true, .customer_id = "customer-1"};
    auto own = lake::control::authorize(graph.store, reader, graph.team, "read", confined);
    ASSERT_TRUE(own);
    EXPECT_TRUE(own->allowed);
    // Below an inheritance cut, the chain is still followed to the account.
    auto below = lake::control::authorize(graph.store, reader, graph.below_cut, "read", confined);
    ASSERT_TRUE(below);
    EXPECT_TRUE(below->allowed);
    auto other =
        lake::control::authorize(graph.store, reader, other_customer->id, "read", confined);
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->allowed) << "an agent credential never reaches another account";
    auto batch = lake::control::authorize_batch(
        graph.store, reader, {graph.team, other_customer->id}, "read", confined);
    ASSERT_TRUE(batch);
    EXPECT_TRUE(batch->at(graph.team).allowed);
    EXPECT_FALSE(batch->at(other_customer->id).allowed);
    // A fresh cache for a re-check keeps the confinement.
    AuthorizationCache fresh = lake::control::fresh_cache_like(confined);
    auto rechecked =
        lake::control::authorize(graph.store, reader, other_customer->id, "read", fresh);
    ASSERT_TRUE(rechecked);
    EXPECT_FALSE(rechecked->allowed);

    // Bound to a workspace, a repository that cannot be shown to belong to it is outside.
    AuthorizationCache in_workspace;
    in_workspace.confinement = {
        .active = true, .customer_id = "customer-1", .workspace_id = "workspace-1"};
    auto unproven =
        lake::control::authorize(graph.store, reader, graph.repository, "read", in_workspace);
    ASSERT_TRUE(unproven);
    EXPECT_FALSE(unproven->allowed);
    auto account = lake::control::authorize(graph.store, reader, graph.team, "read", in_workspace);
    ASSERT_TRUE(account);
    EXPECT_TRUE(account->allowed) << "objects above any repository are the account's";

    // A credential whose row could not be read is confined to nothing.
    AuthorizationCache nothing;
    nothing.confinement = {.active = true};
    auto none = lake::control::authorize(graph.store, reader, graph.team, "read", nothing);
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->allowed);
}

// lake#985: the reads a library agent's task may carry leave out secrets, credentials
// and identity material, and keep the workspace's data.
TEST(Policy, LibraryAgentReadsLeaveOutSecretsAndCredentials) {
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    std::set<std::string> allowed;
    for (const auto& capability : registry.list()) {
        if (lake::api::library_agent_may_read(*capability)) {
            allowed.insert(capability->name);
            EXPECT_TRUE(capability->kind == lake::api::CapabilityKind::Read ||
                        capability->action == "read")
                << capability->name << " is not a read";
        }
    }
    for (const char* withheld : {"read_secret",
                                 "list_secrets",
                                 "secret_metadata",
                                 "list_secret_mounts",
                                 "list_sessions",
                                 "list_devices",
                                 "list_sso_providers",
                                 "scim_list_users",
                                 "create_token"}) {
        EXPECT_FALSE(allowed.contains(withheld)) << withheld;
    }
    for (const char* kept : {"get_repository", "repository_file", "repository_tree"}) {
        if (registry.find(kept)) {
            EXPECT_TRUE(allowed.contains(kept)) << kept;
        }
    }
}

// lake#929: a moved drive folder's object takes its new parent's grants and loses its old one's;
// only a path object moves, and never under itself.
TEST(Policy, APathObjectMovesUnderAnotherParent) {
    PolicyGraph graph;
    const std::string shared =
        graph.store.create_policy_object({"path", "shared", graph.repository, "admin", "r8", true})
            ->id;
    const std::string folder =
        graph.store.create_policy_object({"path", "folder", shared, "admin", "r9", true})->id;
    graph.grant("taap:1:ed25519:reader", shared, "read");
    const auto reads = [&] {
        AuthorizationCache cache;
        const auto decision =
            lake::control::authorize(graph.store, "taap:1:ed25519:reader", folder, "read", cache);
        return decision && decision->allowed;
    };
    EXPECT_TRUE(reads());

    const auto moved = graph.store.set_policy_object_parent(folder, graph.below_cut);
    ASSERT_TRUE(moved) << moved.error().message;
    EXPECT_EQ(moved->parent_id, graph.below_cut);
    EXPECT_FALSE(reads());

    EXPECT_EQ(graph.store.set_policy_object_parent(graph.below_cut, folder).error().code,
              "invalid_input");
    EXPECT_EQ(graph.store.set_policy_object_parent(folder, folder).error().code, "invalid_input");
    EXPECT_EQ(graph.store.set_policy_object_parent(graph.repository, graph.customer).error().code,
              "invalid_input");
    EXPECT_EQ(graph.store.set_policy_object_parent(folder, graph.team).error().code,
              "invalid_input");
    EXPECT_EQ(graph.store.set_policy_object_parent(folder, "missing").error().code,
              "invalid_input");
}

TEST(Policy, BatchedAuthorizationUsesTheSameRules) {
    PolicyGraph graph;
    graph.grant("taap:1:ed25519:writer", graph.organization, "write");
    AuthorizationCache cache;
    auto decisions =
        lake::control::authorize_batch(graph.store,
                                       "taap:1:ed25519:writer",
                                       {graph.organization, graph.team, graph.below_cut},
                                       "write",
                                       cache);
    ASSERT_TRUE(decisions);
    EXPECT_TRUE(decisions->at(graph.organization).allowed);
    EXPECT_TRUE(decisions->at(graph.team).allowed);
    EXPECT_FALSE(decisions->at(graph.below_cut).allowed);
    EXPECT_EQ(cache.paths.size(), 3u);
}

TEST(Policy, ObjectListsFilterCandidatesWithBatchedAuthorization) {
    InMemoryStore store;
    const auto customer = store.create_policy_object(
        NewPolicyObject{"customer", "customer-list", {}, "admin", "r1", true});
    ASSERT_TRUE(customer);
    const auto hidden = store.create_policy_object(
        NewPolicyObject{"repository", "hidden", customer->id, "admin", "r2", false});
    ASSERT_TRUE(hidden);
    ASSERT_TRUE(store.create_grant(
        {"taap:1:ed25519:lister", customer->id, "admin", "sha512-256:list", "admin", "r3"}));

    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.acting_identity = "taap:1:ed25519:lister";
    context.request_id = "list-request";

    ApiResult result = registry.call("list_objects", {{"parent_id", customer->id}}, context);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.at("items").empty());
}

TEST(Policy, DelegatedAdministratorCannotLeaveItsObjectScope) {
    InMemoryStore store;
    const auto first = store.create_organization({"taap:1:ed25519:org-a", "A"});
    const auto second = store.create_organization({"taap:1:ed25519:org-b", "B"});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto first_object = store.find_policy_object("organization", first->id);
    ASSERT_TRUE(first_object);
    ASSERT_TRUE(store.create_grant({"taap:1:ed25519:delegated",
                                    first_object->id,
                                    "admin",
                                    "sha512-256:grant",
                                    "admin",
                                    "request"}));

    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.acting_identity = "taap:1:ed25519:delegated";
    context.request_id = "request";

    ApiResult allowed = registry.call(
        "create_team", {{"organization_id", first->id}, {"name", "allowed"}}, context);
    ASSERT_TRUE(allowed.ok) << allowed.error.message;
    ApiResult denied = registry.call(
        "create_team", {{"organization_id", second->id}, {"name", "denied"}}, context);
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
}

TEST(Policy, EveryCapabilityNamesAnObjectAndAction) {
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    for (const auto& capability : registry.list()) {
        EXPECT_FALSE(capability->object.empty()) << capability->name;
        EXPECT_FALSE(capability->action.empty()) << capability->name;
        EXPECT_TRUE(static_cast<bool>(capability->resolve_object)) << capability->name;
    }
}

TEST(Policy, EffectiveAccessMatchesAuthorization) {
    PolicyGraph graph;
    graph.grant("taap:1:ed25519:sharer", graph.customer, "share");
    graph.grant("taap:1:ed25519:writer", graph.team, "write");
    AuthorizationCache cache;
    auto entries = lake::control::effective_access(graph.store, graph.team, cache);
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries->size(), 2u);

    for (const auto& entry : *entries) {
        for (const auto& action : entry.actions) {
            AuthorizationCache authorization_cache;
            auto decision = lake::control::authorize(
                graph.store, entry.principal_taap_id, graph.team, action, authorization_cache);
            ASSERT_TRUE(decision);
            EXPECT_TRUE(decision->allowed) << entry.principal_taap_id << " " << action;
            EXPECT_EQ(decision->inherited, entry.inherited);
        }
    }
}

TEST(Organizations, InvitationsBecomeAttestedMemberships) {
    InMemoryStore store;
    const auto first_user =
        store.create_user({"taap:1:ed25519:first", "first@example.com", "First"});
    const auto second_user =
        store.create_user({"taap:1:ed25519:second", "second@example.com", "Second"});
    const auto organization = store.create_organization({"taap:1:ed25519:org", "Orama"});
    ASSERT_TRUE(first_user);
    ASSERT_TRUE(second_user);
    ASSERT_TRUE(organization);

    auto owner = store.create_membership({organization->id,
                                          {},
                                          "user",
                                          first_user->id,
                                          first_user->taap_id,
                                          "owner",
                                          "sha512-256:owner",
                                          "admin",
                                          "request-owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    auto invitation = store.create_invitation({organization->id,
                                               {},
                                               "email",
                                               "SECOND@EXAMPLE.COM",
                                               "member",
                                               "2999-01-01T00:00:00Z",
                                               first_user->taap_id,
                                               "request-invite"});
    ASSERT_TRUE(invitation) << invitation.error().message;
    EXPECT_EQ(invitation->invitee, "second@example.com");

    auto membership = store.accept_invitation({invitation->id,
                                               "user",
                                               second_user->id,
                                               second_user->taap_id,
                                               "sha512-256:accepted",
                                               first_user->taap_id,
                                               "request-accept"});
    ASSERT_TRUE(membership) << membership.error().message;
    EXPECT_EQ(membership->role, "member");
    EXPECT_EQ(membership->attestation_digest, "sha512-256:accepted");
    EXPECT_EQ(membership->granted_by_taap_id, first_user->taap_id);
    EXPECT_EQ(store.get_invitation(invitation->id)->status, "accepted");

    const auto object = store.find_policy_object("organization", organization->id);
    ASSERT_TRUE(object);
    AuthorizationCache cache;
    auto access = lake::control::authorize(store, second_user->taap_id, object->id, "read", cache);
    ASSERT_TRUE(access);
    EXPECT_TRUE(access->allowed);
    EXPECT_FALSE(access->inherited);
}

TEST(Organizations, MemberRoleAndRemovalPreserveAnOwnerAndDerivedAccess) {
    InMemoryStore store;
    const auto owner = store.create_user({"taap:1:ed25519:owner", "owner@example.com", "Owner"});
    const auto colleague =
        store.create_user({"taap:1:ed25519:colleague", "colleague@example.com", "Colleague"});
    const auto organization = store.create_organization({"taap:1:ed25519:organization", "Orama"});
    ASSERT_TRUE(owner);
    ASSERT_TRUE(colleague);
    ASSERT_TRUE(organization);
    const auto owner_membership = store.create_membership({organization->id,
                                                           {},
                                                           "user",
                                                           owner->id,
                                                           owner->taap_id,
                                                           "owner",
                                                           "sha512-256:owner",
                                                           "admin",
                                                           "owner-membership"});
    ASSERT_TRUE(owner_membership);
    EXPECT_FALSE(store.set_member_role(owner_membership->id, "admin", owner->taap_id));
    EXPECT_FALSE(store.remove_member(owner_membership->id, owner->taap_id));

    const auto colleague_membership = store.create_membership({organization->id,
                                                               {},
                                                               "user",
                                                               colleague->id,
                                                               colleague->taap_id,
                                                               "owner",
                                                               "sha512-256:colleague",
                                                               owner->taap_id,
                                                               "colleague-membership"});
    ASSERT_TRUE(colleague_membership);
    const auto updated = store.set_member_role(owner_membership->id, "admin", owner->taap_id);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(updated->role, "admin");

    const auto object = store.find_policy_object("organization", organization->id);
    ASSERT_TRUE(object);
    AuthorizationCache cache;
    EXPECT_TRUE(
        lake::control::authorize(store, owner->taap_id, object->id, "admin", cache)->allowed);
    ASSERT_TRUE(store.remove_member(owner_membership->id, colleague->taap_id));
    AuthorizationCache removed_cache;
    EXPECT_FALSE(lake::control::authorize(store, owner->taap_id, object->id, "read", removed_cache)
                     ->allowed);
}

namespace {

ApiContext person_context(InMemoryStore& store,
                          const lake::control::User& user,
                          const std::string& request_id) {
    ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.acting_identity = user.taap_id;
    context.acting_user_id = user.id;
    context.request_id = request_id;
    return context;
}

bool allowed(InMemoryStore& store,
             const std::string& principal,
             const std::string& customer_id,
             const std::string& action) {
    const auto object = store.find_policy_object("customer", customer_id);
    if (!object) {
        return false;
    }
    AuthorizationCache cache;
    const auto decision = lake::control::authorize(store, principal, object->id, action, cache);
    return decision && decision->allowed;
}

} // namespace

TEST(Organizations, APersonCreatesAnOrganizationInvitesAndAColleagueJoinsItsSpace) {
    InMemoryStore store;
    const auto owner = store.create_user({"taap:1:ed25519:founder", "founder@example.com", "F"});
    const auto colleague =
        store.create_user({"taap:1:ed25519:colleague", "Colleague@Example.com", "C"});
    const auto stranger = store.create_user({"taap:1:ed25519:stranger", "s@example.com", "S"});
    ASSERT_TRUE(owner && colleague && stranger);
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");

    ApiContext founder = person_context(store, *owner, "create");
    const ApiResult created = registry.call(
        "create_organization_space", {{"display_name", " Acme "}, {"name", "acme"}}, founder);
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string organization_id = created.value.at("organization").at("id");
    const std::string space_id = created.value.at("customer").at("id");
    EXPECT_EQ(created.value.at("organization").at("display_name"), "Acme");
    EXPECT_EQ(created.value.at("customer").at("customer_type"), "organization");
    EXPECT_EQ(created.value.at("customer").at("name"), "acme");
    EXPECT_EQ(created.value.at("membership").at("role"), "owner");
    EXPECT_FALSE(created.value.contains("personal_customer"));
    EXPECT_TRUE(allowed(store, owner->taap_id, space_id, "admin"));
    const ApiResult taken = registry.call(
        "create_organization_space", {{"display_name", "Again"}, {"name", "acme"}}, founder);
    EXPECT_EQ(taken.error.code, "conflict");

    founder.request_id = "invite";
    const ApiResult invitation = registry.call("create_invitation",
                                               {{"organization_id", organization_id},
                                                {"invitee_type", "email"},
                                                {"invitee", "colleague@example.com"},
                                                {"role", "admin"},
                                                {"expires_at", "2999-01-01T00:00:00Z"}},
                                               founder);
    ASSERT_TRUE(invitation.ok) << invitation.error.message;
    const std::string invitation_id = invitation.value.at("id");

    ApiContext invitee = person_context(store, *colleague, "list");
    const ApiResult mine = registry.call("list_invitations", json::object(), invitee);
    ASSERT_TRUE(mine.ok) << mine.error.message;
    ASSERT_EQ(mine.value.at("items").size(), 1U);
    EXPECT_EQ(mine.value.at("items").at(0).at("organization_name"), "Acme");

    ApiContext outsider = person_context(store, *stranger, "steal");
    EXPECT_TRUE(registry.call("list_invitations", json::object(), outsider)
                    .value.at("items")
                    .empty());
    const ApiResult stolen =
        registry.call("join_organization", {{"invitation_id", invitation_id}}, outsider);
    EXPECT_EQ(stolen.error.code, "unauthorized");

    invitee.request_id = "join";
    const ApiResult joined =
        registry.call("join_organization", {{"invitation_id", invitation_id}}, invitee);
    ASSERT_TRUE(joined.ok) << joined.error.message;
    EXPECT_EQ(joined.value.at("role"), "admin");
    EXPECT_EQ(joined.value.at("attestation_digest"), "invitation/" + invitation_id);
    EXPECT_EQ(joined.value.at("granted_by_taap_id"), owner->taap_id);
    EXPECT_EQ(registry.call("join_organization", {{"invitation_id", invitation_id}}, invitee)
                  .error.code,
              "conflict");
    const ApiResult memberships = registry.call("list_memberships", json::object(), invitee);
    ASSERT_TRUE(memberships.ok) << memberships.error.message;
    ASSERT_EQ(memberships.value.at("items").size(), 1U);
    EXPECT_TRUE(allowed(store, colleague->taap_id, space_id, "admin"));

    const std::string membership_id = joined.value.at("id");
    ASSERT_TRUE(registry.call("set_member_role", {{"id", membership_id}, {"role", "member"}}, founder)
                    .ok);
    EXPECT_TRUE(allowed(store, colleague->taap_id, space_id, "read"));
    EXPECT_FALSE(allowed(store, colleague->taap_id, space_id, "admin"));
    ASSERT_TRUE(registry.call("remove_member", {{"id", membership_id}}, founder).ok);
    EXPECT_FALSE(allowed(store, colleague->taap_id, space_id, "read"));

    ApiContext operator_context;
    operator_context.store = &store;
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    EXPECT_EQ(registry.call("list_memberships", json::object(), operator_context).error.code,
              "invalid_input");
    EXPECT_EQ(registry.call("create_organization_space", {{"display_name", "Ops"}}, operator_context)
                  .error.code,
              "unauthorized");
}

TEST(Organizations, APersonalSpaceBecomesAnOrganizationAndItsOwnerGetsAFreshOne) {
    InMemoryStore store;
    const auto person = store.create_user({"taap:1:ed25519:solo", "solo@example.com", "Solo"});
    const auto other = store.create_user({"taap:1:ed25519:other", "other@example.com", "Other"});
    ASSERT_TRUE(person && other);
    const auto personal = store.create_customer({"individual", person->id, "", "solo-space"});
    ASSERT_TRUE(personal);
    const auto object = store.find_policy_object("customer", personal->id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store.create_grant(
        {person->taap_id, object->id, "admin", "sign_up_password/test", person->taap_id, "r"}));
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");

    ApiContext intruder = person_context(store, *other, "intrude");
    EXPECT_EQ(registry
                  .call("convert_personal_space",
                        {{"customer_id", personal->id}, {"display_name", "Taken"}},
                        intruder)
                  .error.code,
              "unauthorized");

    ApiContext owner = person_context(store, *person, "convert");
    const ApiResult converted = registry.call(
        "convert_personal_space", {{"customer_id", personal->id}, {"display_name", "Solo Labs"}},
        owner);
    ASSERT_TRUE(converted.ok) << converted.error.message;
    EXPECT_EQ(converted.value.at("customer").at("id"), personal->id);
    EXPECT_EQ(converted.value.at("customer").at("name"), "solo-space");
    EXPECT_EQ(converted.value.at("customer").at("customer_type"), "organization");
    EXPECT_EQ(converted.value.at("membership").at("role"), "owner");
    const std::string fresh = converted.value.at("personal_customer").at("id");
    EXPECT_NE(fresh, personal->id);
    EXPECT_EQ(converted.value.at("personal_customer").at("customer_type"), "individual");
    EXPECT_TRUE(allowed(store, person->taap_id, personal->id, "admin"));
    EXPECT_TRUE(allowed(store, person->taap_id, fresh, "admin"));
    const auto personal_spaces = store.list_customers(10, person->id);
    ASSERT_TRUE(personal_spaces);
    ASSERT_EQ(personal_spaces->size(), 1U);
    EXPECT_EQ(personal_spaces->front().id, fresh);

    owner.request_id = "again";
    EXPECT_EQ(registry
                  .call("convert_personal_space",
                        {{"customer_id", personal->id}, {"display_name", "Twice"}},
                        owner)
                  .error.code,
              "conflict");
    const ApiResult by_customer_id =
        registry.call("get_customer", {{"customer_id", personal->id}}, owner);
    ASSERT_TRUE(by_customer_id.ok) << by_customer_id.error.message;
    EXPECT_EQ(by_customer_id.value.at("organization_id"),
              converted.value.at("organization").at("id"));
    EXPECT_EQ(registry.call("get_customer", json::object(), owner).error.code, "invalid_input");
}
