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
                                          "taap:1:ed25519:grantor",
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
                                                           owner->taap_id,
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
