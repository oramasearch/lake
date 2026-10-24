#include "identity_proof_test_support.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/telemetry/metrics.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"

#include <barrier>
#include <future>
#include <memory>
#include <pqxx/pqxx>

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

    const auto enrollment =
        store.enroll_mfa(target.principal.user_id,
                         "lake",
                         {target.issued.session.id, "correct horse battery staple"});
    ASSERT_TRUE(enrollment) << enrollment.error().message;
    ASSERT_FALSE(enrollment->recovery_codes.empty());
    ApiContext anonymous;
    anonymous.store = &store;
    anonymous.instance_kek = "offline-recovery-test-kek";
    const ApiResult code_recovery = *liquid::runtime::block_on(
        registry.call_async("recover_with_code",
                            {{"email", "recovery-target@example.test"},
                             {"code", enrollment->recovery_codes.front()},
                             {"device_name", "code recovery"}},
                            anonymous));
    ASSERT_TRUE(code_recovery.ok) << code_recovery.error.message;
    EXPECT_TRUE(store.context_for(code_recovery.value["secret"]));
    const ApiResult reused_code = *liquid::runtime::block_on(registry.call_async(
        "recover_with_code",
        {{"email", "recovery-target@example.test"}, {"code", enrollment->recovery_codes.front()}},
        anonymous));
    EXPECT_FALSE(reused_code.ok);
    EXPECT_EQ(reused_code.error.code, "unauthenticated");

    const ApiResult email = registry.call(
        "begin_email_recovery", {{"email", "recovery-target@example.test"}}, anonymous);
    ASSERT_TRUE(email.ok) << email.error.message;
    // lake#774: one answer for any address, so a caller cannot tell whether it has an account,
    // and nothing about the attempt; its id reaches the owner in the recovery email.
    const ApiResult unknown =
        registry.call("begin_email_recovery", {{"email", "nobody-here@example.test"}}, anonymous);
    ASSERT_TRUE(unknown.ok) << unknown.error.message;
    EXPECT_EQ(email.value, unknown.value);
    EXPECT_EQ(email.value["status"], "requested");
    EXPECT_FALSE(email.value.contains("id"));
    EXPECT_FALSE(email.value.contains("user_id"));
    const auto email_deliveries = store.claim_recovery_deliveries(10);
    ASSERT_TRUE(email_deliveries) << email_deliveries.error().message;
    ASSERT_EQ(email_deliveries->size(), 1U) << "only the real account's attempt is delivered";
    const std::string email_recovery_id = email_deliveries->front().recovery_id;
    const auto early =
        store.complete_recovery(email_recovery_id, email_deliveries->front().token, "too early");
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

// One contract suite runs against both stores; each PostgreSQL case has its own database.
class EmailRecoveryLimits : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            const char* configured = lake::test::postgres_uri();
            if (!configured) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            uri = configured;
            store = lake::control::make_postgres_store(
                uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
        } else {
            store = std::make_unique<InMemoryStore>();
        }
        store = lake::telemetry::instrument_store(std::move(store), metrics);
        lake::api::register_core_capabilities(registry, "test");
        target = account("recovery-limits");
        anonymous.store = store.get();
        anonymous.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        anonymous.recovery_client_address = "192.0.2.1";
    }
    Account account(const std::string& label) {
        lake::control::NewPasswordUser input{"lake",
                                             label + "@example.test",
                                             "correct horse battery staple",
                                             label,
                                             label + " device",
                                             ""};
        if (GetParam()) {
            auto identity = lake::test::test_identity(label);
            input.identity_document = identity.document;
            input.identity_proof = lake::test::prove_identity(*store, "lake", identity);
        }
        const auto issued = store->sign_up_password(input);
        EXPECT_TRUE(issued) << (issued ? "" : issued.error().message);
        if (!issued)
            return {};
        const auto principal = store->context_for(issued->secret);
        EXPECT_TRUE(principal);
        return {*issued, principal ? *principal : lake::control::Principal{}};
    }
    ApiContext context(const Account& owner) {
        ApiContext ctx;
        ctx.store = store.get();
        ctx.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
        ctx.authorized = true;
        ctx.session_credential = true;
        ctx.acting_identity = owner.principal.taap_id;
        ctx.acting_user_id = owner.principal.user_id;
        ctx.credential_kind = owner.principal.kind;
        ctx.scopes = owner.principal.scopes;
        return ctx;
    }
    ApiResult begin(const std::string& email = "recovery-limits@example.test") {
        return registry.call("begin_email_recovery", {{"email", email}}, anonymous);
    }
    lake::telemetry::Metrics metrics;
    std::shared_ptr<lake::control::Store> store;
    Registry registry;
    Account target;
    ApiContext anonymous;
    std::string uri;
};

TEST_P(EmailRecoveryLimits, RepeatedAndConcurrentStartsKeepOneDeliveryAndNotice) {
    // Store concurrency avoids coupling this invariant to the audit appender's retry budget.
    // The API checks stay sequential and compare the public accepted body and status.
    const auto first = begin("first-unknown@example.test");
    ASSERT_TRUE(first.ok) << first.error.message;
    const auto unknown = begin("unknown@example.test");
    ASSERT_TRUE(unknown.ok);
    EXPECT_EQ(lake::api::rest_status(first), lake::api::rest_status(unknown));
    EXPECT_EQ(first.value, unknown.value);
    std::barrier start(9);
    std::vector<std::future<std::string>> calls;
    for (int i = 0; i < 8; ++i) {
        calls.push_back(std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            const auto result = store->begin_email_recovery("lake",
                                                            "RECOVERY-LIMITS@example.test",
                                                            24 * 60 * 60,
                                                            anonymous.recovery_client_address);
            EXPECT_TRUE(result) << (result ? "" : result.error().message);
            return result ? result->id : std::string();
        }));
    }
    start.arrive_and_wait();
    std::string recovery_id;
    for (auto& call : calls) {
        const auto result = call.get();
        ASSERT_FALSE(result.empty());
        if (recovery_id.empty())
            recovery_id = result;
        EXPECT_EQ(result, recovery_id);
    }
    const auto known = begin();
    ASSERT_TRUE(known.ok) << known.error.message;
    EXPECT_EQ(known.value, first.value);
    EXPECT_EQ(lake::api::rest_status(known), lake::api::rest_status(first));
    const auto deliveries = store->claim_recovery_deliveries(100);
    ASSERT_TRUE(deliveries);
    ASSERT_EQ(deliveries->size(), 1U);
    const auto attempt = store->get_recovery_attempt(deliveries->front().recovery_id);
    ASSERT_TRUE(attempt);
    const auto repeated = store->begin_email_recovery("lake", "recovery-limits@example.test", 3600);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->id, attempt->id);
    EXPECT_EQ(repeated->ready_at, attempt->ready_at);
    EXPECT_EQ(repeated->expires_at, attempt->expires_at);
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
    const auto notices = store->list_recovery_notifications(target.principal.user_id);
    ASSERT_TRUE(notices);
    ASSERT_EQ(notices->size(), 1U);
    EXPECT_EQ(lake::control::to_json(notices->front())["cancel_action"]["input"]["recovery_id"],
              attempt->id);
}

TEST_P(EmailRecoveryLimits, CancellationIsOwnerOnlyAndKeepsTheAccountBudget) {
    ASSERT_TRUE(begin().ok);
    const auto deliveries = store->claim_recovery_deliveries(10);
    ASSERT_TRUE(deliveries);
    ASSERT_EQ(deliveries->size(), 1U);
    const auto delivery = deliveries->front();
    const auto other = account("another-recovery-holder");
    auto other_context = context(other);
    const json input = {{"recovery_id", delivery.recovery_id}};
    EXPECT_FALSE(registry.call("cancel_recovery", input, anonymous).ok);
    EXPECT_FALSE(registry.call("cancel_recovery", input, other_context).ok);
    EXPECT_FALSE(store->cancel_recovery(other.principal.user_id, delivery.recovery_id));
    auto owner = context(target);
    owner.application_session =
        target.principal; // exercise the application-pool admission path too
    EXPECT_TRUE(registry.call("list_recovery_notifications", json::object(), owner).ok);
    const auto cancelled = registry.call("cancel_recovery", input, owner);
    ASSERT_TRUE(cancelled.ok) << cancelled.error.message;
    EXPECT_EQ(cancelled.value["status"], "cancelled");
    if (GetParam()) {
        // Once the cooling time passes, the previously delivered token must still fail.
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("UPDATE offline_recovery_attempts SET ready_at = now() - interval '1 second' "
                "WHERE id = $1::uuid",
                pqxx::params{delivery.recovery_id});
        tx.commit();
    }
    const auto completion =
        store->complete_recovery(delivery.recovery_id, delivery.token, "cancel bypass");
    ASSERT_FALSE(completion);
    EXPECT_EQ(completion.error().code, "unauthenticated");
    EXPECT_TRUE(store->context_for(target.issued.secret));
    anonymous.recovery_client_address = "192.0.2.2";
    const auto suppressed = begin();
    EXPECT_TRUE(suppressed.ok);
    EXPECT_EQ(suppressed.value, begin("unknown@example.test").value);
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
    EXPECT_EQ(store->list_recovery_notifications(target.principal.user_id)->size(), 1U);
    if (GetParam()) {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("UPDATE offline_recovery_attempts SET created_at = now() - interval '25 hours' "
                "WHERE id = $1::uuid",
                pqxx::params{delivery.recovery_id});
        tx.commit();
        ASSERT_TRUE(begin().ok);
        const auto later = store->claim_recovery_deliveries(100);
        ASSERT_TRUE(later);
        ASSERT_EQ(later->size(), 1U);
        EXPECT_NE(later->front().recovery_id, delivery.recovery_id);
        EXPECT_FALSE(store->complete_recovery(delivery.recovery_id, delivery.token, "old token"));
    }
}

TEST_P(EmailRecoveryLimits, CancellationRemovesUnclaimedMail) {
    const auto attempt = store->begin_email_recovery("lake", "recovery-limits@example.test", 3600);
    ASSERT_TRUE(attempt);
    ASSERT_FALSE(attempt->id.empty());
    ASSERT_TRUE(store->cancel_recovery(target.principal.user_id, attempt->id));
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
}

TEST_P(EmailRecoveryLimits, CancelledAdminRecoveryCannotBeApprovedAgain) {
    const auto first = account("cancel-admin-one");
    const auto second = account("cancel-admin-two");
    const auto organization =
        store->create_organization({"taap:1:ed25519:cancel-org", "Cancel org", ""});
    ASSERT_TRUE(organization);
    const auto add = [&](const Account& who, const std::string& role) {
        return store->create_membership({organization->id,
                                         "",
                                         "user",
                                         who.principal.user_id,
                                         who.principal.taap_id,
                                         role,
                                         "recovery-membership-attestation",
                                         "admin",
                                         "recovery-test"});
    };
    ASSERT_TRUE(add(target, "member"));
    ASSERT_TRUE(add(first, "admin"));
    ASSERT_TRUE(add(second, "admin"));
    const auto pending = store->begin_admin_recovery(
        organization->id, target.principal.user_id, first.principal.taap_id);
    ASSERT_TRUE(pending);
    ASSERT_TRUE(store->cancel_recovery(target.principal.user_id, pending->id));
    EXPECT_FALSE(store->approve_admin_recovery(pending->id, second.principal.taap_id));
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
}

TEST_P(EmailRecoveryLimits, UnknownAddressesSpendClientAndGlobalBudgets) {
    const auto accepted = begin("unknown@example.test");
    ASSERT_TRUE(accepted.ok);
    for (int i = 1; i < 30; ++i) {
        EXPECT_EQ(begin("unknown@example.test").value, accepted.value);
    }
    EXPECT_EQ(begin().value, accepted.value);
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
    anonymous.recovery_client_address = "192.0.2.2";
    EXPECT_EQ(begin().value, accepted.value);
    ASSERT_EQ(store->claim_recovery_deliveries(100)->size(), 1U);
    // The throttled 31st request of 192.0.2.1 spent no global budget: 31 are spent so far,
    // and 269 more distinct clients spend the rest of the 300.
    const auto second = account("global-recovery-target");
    for (int i = 31; i < 300; ++i) {
        anonymous.recovery_client_address = "client-" + std::to_string(i);
        EXPECT_EQ(begin("unknown@example.test").value, accepted.value);
    }
    anonymous.recovery_client_address = "fresh-client";
    EXPECT_EQ(begin("global-recovery-target@example.test").value, accepted.value);
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
    if (GetParam()) {
        // Budgets survive a new store instance, then recover when their hour expires.
        store = lake::control::make_postgres_store(
            uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
        store = lake::telemetry::instrument_store(std::move(store), metrics);
        anonymous.store = store.get();
        EXPECT_EQ(begin("global-recovery-target@example.test").value, accepted.value);
        EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec("UPDATE offline_recovery_budgets SET started_at = now() - interval '2 hours'");
        tx.commit();
        ASSERT_TRUE(begin("global-recovery-target@example.test").ok);
        EXPECT_EQ(store->claim_recovery_deliveries(100)->size(), 1U);
    }
}

TEST_P(EmailRecoveryLimits, OneClientExhaustingItsBudgetLeavesAnotherClientAbleToRecover) {
    // One client sends more than the whole instance's hourly budget. Only the 30 requests its
    // own budget admits spend the global budget, so another client can still start recovery.
    const auto accepted = begin("unknown@example.test");
    ASSERT_TRUE(accepted.ok) << accepted.error.message;
    for (int i = 1; i < 400; ++i) {
        EXPECT_EQ(begin("unknown@example.test").value, accepted.value);
    }
    EXPECT_EQ(begin().value, accepted.value);
    EXPECT_TRUE(store->claim_recovery_deliveries(100)->empty());
    anonymous.recovery_client_address = "198.51.100.7";
    const auto other = begin();
    ASSERT_TRUE(other.ok) << other.error.message;
    EXPECT_EQ(other.value, accepted.value);
    const auto deliveries = store->claim_recovery_deliveries(100);
    ASSERT_TRUE(deliveries);
    ASSERT_EQ(deliveries->size(), 1U);
    const auto attempt = store->get_recovery_attempt(deliveries->front().recovery_id);
    ASSERT_TRUE(attempt);
    EXPECT_EQ(attempt->user_id, target.principal.user_id);
}

INSTANTIATE_TEST_SUITE_P(Stores, EmailRecoveryLimits, testing::Bool(), [](const auto& info) {
    return info.param ? "Postgres" : "Memory";
});

} // namespace
