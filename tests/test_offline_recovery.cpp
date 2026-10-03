#include "personal_customer_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/memory_store.hpp"
#include "repository_test_support.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using lake::control::InMemoryStore;
using nlohmann::json;

struct Account {
    lake::control::IssuedSession issued;
    lake::control::Principal principal;
};

Account create_account(InMemoryStore& store, const std::string& label) {
    const auto issued = store.sign_up_password({"lake",
                                                label + "@example.test",
                                                "correct horse battery staple",
                                                label,
                                                label + " device",
                                                "{}"});
    EXPECT_TRUE(issued) << issued.error().message;
    if (!issued) {
        return {};
    }
    const auto principal = store.context_for(issued->secret);
    EXPECT_TRUE(principal) << principal.error().message;
    return {*issued, principal ? *principal : lake::control::Principal{}};
}

ApiContext user_context(InMemoryStore& store, const Account& account) {
    ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.session_credential = true;
    context.acting_user_id = account.principal.user_id;
    context.acting_identity = account.principal.taap_id;
    context.credential_device_id = account.principal.device_id;
    context.scopes = account.principal.scopes;
    context.instance_kek = "offline-recovery-test-kek";
    return context;
}

ApiContext operator_context(InMemoryStore& store) {
    ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "offline-recovery-test-kek";
    return context;
}

TEST(OfflineGrant, CachedReadSurvivesRevocationUntilExplicitExpiry) {
    InMemoryStore store;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const Account account = create_account(store, "offline-reader");
    const auto customer = lake::test::personal_customer(store, account.principal.user_id);
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        store.create_repository({customer->id, "offline-project", "", "private",
                                 lake::test::repository_workspace(store, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto object = store.find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(store.create_grant({account.principal.taap_id,
                                    object->id,
                                    "write",
                                    "offline-test-attestation",
                                    "admin",
                                    "offline-test"}));

    ApiContext context = user_context(store, account);
    const std::string scope = "repository:" + repository->id + ":read";
    const ApiResult issued = registry.call("issue_offline_grant",
                                           {{"device_id", account.principal.device_id},
                                            {"scopes", json::array({scope})},
                                            {"duration_seconds", 3600}},
                                           context);
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["credential"]["attestation_profile"], "offline_grant/1");

    ApiContext anonymous;
    anonymous.store = &store;
    anonymous.instance_kek = "offline-recovery-test-kek";
    const ApiResult cached =
        registry.call("check_cached_read",
                      {{"credential", issued.value["credential"]}, {"scope", scope}},
                      anonymous);
    ASSERT_TRUE(cached.ok) << cached.error.message;
    EXPECT_TRUE(cached.value["allowed"]);

    const ApiResult revoked =
        registry.call("revoke_offline_grant", {{"id", issued.value["id"]}}, context);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_FALSE(revoked.value["revoked_at"].is_null());
    const ApiResult still_cached =
        registry.call("check_cached_read",
                      {{"credential", issued.value["credential"]}, {"scope", scope}},
                      anonymous);
    EXPECT_TRUE(still_cached.ok) << still_cached.error.message;

    const std::int64_t expires =
        issued.value["credential"]["asserted_attributes"]["expires_unix"].get<std::int64_t>();
    const auto expired =
        lake::audit::verify_offline_grant(store, issued.value["credential"], scope, expires);
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().message, "offline grant has expired");
    const auto wrong_scope = lake::audit::verify_offline_grant(
        store, issued.value["credential"], "repository:another:read", expires - 1);
    ASSERT_FALSE(wrong_scope);
    EXPECT_EQ(wrong_scope.error().message, "offline grant does not include the scope");
    json tampered = issued.value["credential"];
    tampered["asserted_attributes"]["device_id"] = "another-device";
    EXPECT_FALSE(lake::audit::verify_offline_grant(store, tampered, scope, expires - 1));
}

TEST(AccountRecovery, CodesCoolingAndTwoAdministratorApprovalRestoreSignIn) {
    InMemoryStore store;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const Account target = create_account(store, "recovery-target");
    const Account first_admin = create_account(store, "recovery-admin-one");
    const Account second_admin = create_account(store, "recovery-admin-two");

    const auto enrollment = store.enroll_mfa(target.principal.user_id, "lake");
    ASSERT_TRUE(enrollment) << enrollment.error().message;
    ASSERT_FALSE(enrollment->recovery_codes.empty());
    ApiContext anonymous;
    anonymous.store = &store;
    anonymous.instance_kek = "offline-recovery-test-kek";
    const ApiResult code_recovery = registry.call("recover_with_code",
                                                  {{"email", "recovery-target@example.test"},
                                                   {"code", enrollment->recovery_codes.front()},
                                                   {"device_name", "code recovery"}},
                                                  anonymous);
    ASSERT_TRUE(code_recovery.ok) << code_recovery.error.message;
    EXPECT_TRUE(store.context_for(code_recovery.value["secret"]));
    const ApiResult reused_code = registry.call(
        "recover_with_code",
        {{"email", "recovery-target@example.test"}, {"code", enrollment->recovery_codes.front()}},
        anonymous);
    EXPECT_EQ(reused_code.error.code, "unauthorized");

    const ApiResult email = registry.call(
        "begin_email_recovery", {{"email", "recovery-target@example.test"}}, anonymous);
    ASSERT_TRUE(email.ok) << email.error.message;
    EXPECT_EQ(email.value["status"], "cooling");
    const auto email_deliveries = store.claim_recovery_deliveries(10);
    ASSERT_TRUE(email_deliveries) << email_deliveries.error().message;
    ASSERT_EQ(email_deliveries->size(), 1U);
    const auto early =
        store.complete_recovery(email.value["id"], email_deliveries->front().token, "too early");
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().message, "recovery cooling period has not ended");

    const auto organization =
        store.create_organization({"taap:1:ed25519:recovery-org", "Recovery org", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto add_member = [&](const Account& account, const std::string& role) {
        return store.create_membership({organization->id,
                                        "",
                                        "user",
                                        account.principal.user_id,
                                        account.principal.taap_id,
                                        role,
                                        "recovery-membership-attestation",
                                        "admin",
                                        "recovery-test"});
    };
    ASSERT_TRUE(add_member(target, "member"));
    ASSERT_TRUE(add_member(first_admin, "admin"));
    ASSERT_TRUE(add_member(second_admin, "admin"));

    ApiContext first_context = user_context(store, first_admin);
    const ApiResult started = registry.call(
        "begin_admin_recovery",
        {{"organization_id", organization->id}, {"user_id", target.principal.user_id}},
        first_context);
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["approval_count"], 1);
    const ApiResult duplicate = registry.call(
        "approve_admin_recovery", {{"recovery_id", started.value["id"]}}, first_context);
    EXPECT_EQ(duplicate.error.code, "conflict");
    ApiContext second_context = user_context(store, second_admin);
    const ApiResult approved = registry.call(
        "approve_admin_recovery", {{"recovery_id", started.value["id"]}}, second_context);
    ASSERT_TRUE(approved.ok) << approved.error.message;
    EXPECT_EQ(approved.value["approval_count"], 2);

    const auto deliveries = store.claim_recovery_deliveries(10);
    ASSERT_TRUE(deliveries) << deliveries.error().message;
    ASSERT_EQ(deliveries->size(), 1U);
    const auto restored = store.complete_recovery(
        started.value["id"], deliveries->front().token, "administrator recovery");
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_FALSE(store.context_for(target.issued.secret));
    EXPECT_TRUE(store.context_for(restored->secret));
    const auto notifications = store.list_recovery_notifications(target.principal.user_id);
    ASSERT_TRUE(notifications) << notifications.error().message;
    EXPECT_GE(notifications->size(), 4U);
}

TEST(RepositoryRecovery, MissingContactIsPlainAndWrappedKeyIsContactOnly) {
    InMemoryStore store;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const Account owner = create_account(store, "repository-owner");
    const Account contact = create_account(store, "repository-contact");
    const auto customer = lake::test::personal_customer(store, owner.principal.user_id);
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        store.create_repository({customer->id, "encrypted-project", "", "private",
                                 lake::test::repository_workspace(store, customer->id)});
    ASSERT_TRUE(repository) << repository.error().message;

    ApiContext admin = operator_context(store);
    const ApiResult missing =
        registry.call("repository_recovery_status", {{"repository_id", repository->id}}, admin);
    ASSERT_TRUE(missing.ok) << missing.error.message;
    EXPECT_FALSE(missing.value["recoverable"]);
    EXPECT_NE(missing.value["reason"].get<std::string>().find("unrecoverable"), std::string::npos);
    const ApiResult impossible =
        registry.call("recover_repository_key", {{"repository_id", repository->id}}, admin);
    EXPECT_EQ(impossible.error.code, "unrecoverable");
    EXPECT_EQ(lake::api::rest_status(impossible), 409);

    const ApiResult configured = registry.call("configure_recovery_contact",
                                               {{"repository_id", repository->id},
                                                {"contact_taap_id", contact.principal.taap_id},
                                                {"wrapped_key", "client-wrapped-trust-domain-key"}},
                                               admin);
    ASSERT_TRUE(configured.ok) << configured.error.message;
    EXPECT_FALSE(configured.value.contains("wrapped_key"));
    const ApiResult status =
        registry.call("repository_recovery_status", {{"repository_id", repository->id}}, admin);
    ASSERT_TRUE(status.ok) << status.error.message;
    EXPECT_TRUE(status.value["recoverable"]);

    ApiContext contact_context = user_context(store, contact);
    const ApiResult recovered = registry.call(
        "recover_repository_key", {{"repository_id", repository->id}}, contact_context);
    ASSERT_TRUE(recovered.ok) << recovered.error.message;
    EXPECT_EQ(recovered.value["wrapped_key"], "client-wrapped-trust-domain-key");
    ApiContext owner_context = user_context(store, owner);
    const ApiResult refused =
        registry.call("recover_repository_key", {{"repository_id", repository->id}}, owner_context);
    EXPECT_EQ(refused.error.code, "unauthorized");
}

} // namespace
