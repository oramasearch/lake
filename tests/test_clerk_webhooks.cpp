// Clerk webhooks (lake#765, docs/api/clerk.md#webhooks): Svix verification and the user events
// that create, update and disable Lake accounts.
#include "audit_test_support.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "lake/api/clerk_webhooks.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::clerk_webhooks::Delivery;
using nlohmann::json;

// base64("lake clerk webhook test secret!!") — what Clerk shows as whsec_<base64>.
constexpr std::string_view secret = "whsec_bGFrZSBjbGVyayB3ZWJob29rIHRlc3Qgc2VjcmV0ISE=";
constexpr std::string_view other_secret = "whsec_c29tZW9uZSBlbHNlJ3Mgc2VjcmV0IGtleSEhIQ==";
constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Delivery delivery_of(const std::string& id,
                     const json& event,
                     std::string_view key = secret,
                     std::int64_t at = now()) {
    Delivery delivery{id, std::to_string(at), "", event.dump()};
    delivery.signature = "v1," + lake::api::clerk_webhooks::sign(key, delivery);
    return delivery;
}

json user(const std::string& id,
          const std::string& email,
          const std::string& first,
          bool verified = true) {
    static std::int64_t sequence = 0;
    return {
        {"updated_at", now() * 1000 + ++sequence},
        {"banned", false},
        {"locked", false},
        {"id", id},
        {"object", "user"},
        {"first_name", first},
        {"last_name", "Person"},
        {"primary_email_address_id", "idn_1"},
        {"email_addresses",
         json::array({{{"id", "idn_1"},
                       {"email_address", email},
                       {"verification", {{"status", verified ? "verified" : "unverified"}}}}})}};
}

json event(const std::string& type, const json& data) {
    return {
        {"type", type}, {"object", "event"}, {"timestamp", now() * 1000 + 10000}, {"data", data}};
}

lake::api::clerk_webhooks::Outcome deliver(lake::control::Store& store, const Delivery& delivery) {
    ApiContext context;
    context.store = &store;
    context.instance_kek = std::string(instance_kek);
    context.request_id = lake::control::new_uuid_v4();
    return lake::api::clerk_webhooks::receive(context, secret, delivery);
}

lake::control::ClerkIdentity sign_in(const std::string& subject, const std::string& email) {
    const auto minted = lake::audit::mint_custodial_identity(instance_kek);
    EXPECT_TRUE(minted);
    return {subject,
            email,
            "",
            "browser",
            minted->taap_id,
            minted->public_key,
            minted->wrapped_seed,
            now()};
}

void expect_webhook_contract(lake::control::Store& store, const std::string& suffix) {
    const std::string subject = "user_hook_" + suffix;
    const std::string email = "hook-" + suffix + "@example.test";

    // user.created: the account exists before any sign-in, with a managed key and its space.
    const auto created = deliver(
        store, delivery_of("msg_a_" + suffix, event("user.created", user(subject, email, "Ada"))));
    ASSERT_EQ(created.status, 200) << created.body.dump();
    ASSERT_EQ(created.body.at("applied"), true) << created.body.dump();
    EXPECT_EQ(created.body.at("created"), true);
    const std::string user_id = created.body.at("user_id");
    const auto identity = store.account_identity(user_id);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity->key, "managed");
    EXPECT_TRUE(lake::test::personal_customer(store, user_id));
    const auto users = store.list_users(1000);
    ASSERT_TRUE(users);
    const auto found =
        std::ranges::find_if(*users, [&](const auto& row) { return row.id == user_id; });
    ASSERT_NE(found, users->end());
    EXPECT_EQ(found->email, email);
    EXPECT_EQ(found->display_name, "Ada Person");

    // The same delivery again is not applied; the same event in a new delivery changes nothing.
    const auto replayed = deliver(
        store, delivery_of("msg_a_" + suffix, event("user.created", user(subject, email, "Ada"))));
    EXPECT_EQ(replayed.status, 200);
    EXPECT_EQ(replayed.body.at("applied"), false);
    EXPECT_EQ(replayed.body.at("duplicate"), true);
    const auto repeated = deliver(
        store, delivery_of("msg_b_" + suffix, event("user.created", user(subject, email, "Ada"))));
    EXPECT_EQ(repeated.body.at("applied"), true);
    EXPECT_EQ(repeated.body.at("created"), false);
    EXPECT_EQ(repeated.body.at("user_id"), user_id);
    const auto customers = store.list_customers(10, user_id);
    ASSERT_TRUE(customers);
    EXPECT_EQ(customers->size(), 1U);

    // A later sign-in reaches the same account.
    const auto session = store.sign_in_clerk(sign_in(subject, email));
    ASSERT_TRUE(session) << session.error().message;
    const auto principal = store.context_for(session->secret);
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->user_id, user_id);

    // user.updated: email and name.
    const std::string new_email = "hook-new-" + suffix + "@example.test";
    const auto updated = deliver(
        store,
        delivery_of("msg_c_" + suffix, event("user.updated", user(subject, new_email, "Grace"))));
    EXPECT_EQ(updated.body.at("applied"), true) << updated.body.dump();
    const auto after = store.list_users(1000);
    const auto changed =
        std::ranges::find_if(*after, [&](const auto& row) { return row.id == user_id; });
    EXPECT_EQ(changed->email, new_email);
    EXPECT_EQ(changed->display_name, "Grace Person");
    // An unverified address is not taken.
    deliver(store,
            delivery_of("msg_d_" + suffix,
                        event("user.updated", user(subject, "x-" + new_email, "Grace", false))));
    const auto unverified = store.list_users(1000);
    EXPECT_EQ(std::ranges::find_if(*unverified, [&](const auto& row) { return row.id == user_id; })
                  ->email,
              new_email);

    // user.deleted: disabled, sessions stop working, sign-in refused, nothing erased.
    const auto deleted =
        deliver(store,
                delivery_of("msg_e_" + suffix,
                            event("user.deleted", {{"id", subject}, {"deleted", true}})));
    EXPECT_EQ(deleted.body.at("applied"), true) << deleted.body.dump();
    EXPECT_EQ(deleted.body.at("disabled"), true);
    EXPECT_FALSE(store.context_for(session->secret)) << "the session stops working";
    EXPECT_FALSE(store.sign_in_clerk(sign_in(subject, new_email)));
    EXPECT_TRUE(lake::test::personal_customer(store, user_id)) << "nothing is erased";

    // An event for no linked account is acknowledged and changes nothing.
    const auto unknown = deliver(
        store,
        delivery_of("msg_f_" + suffix, event("user.deleted", {{"id", "user_nobody_" + suffix}})));
    EXPECT_EQ(unknown.status, 200);
    EXPECT_EQ(unknown.body.at("applied"), false);

    // A refused signature is 401 and changes nothing.
    const auto forged =
        deliver(store,
                delivery_of("msg_g_" + suffix,
                            event("user.created", user("user_forged_" + suffix, "f-" + email, "F")),
                            other_secret));
    EXPECT_EQ(forged.status, 401);

    const auto events = lake::test::audit_events(store, {.limit = 1000});
    ASSERT_TRUE(events);
    const auto count = [&](const std::string& action, const std::string& outcome) {
        return std::ranges::count_if(*events, [&](const auto& e) {
            return e.draft.object == "clerk" && e.draft.action == action &&
                   e.draft.outcome == outcome &&
                   e.draft.details.value("svix_id", "").ends_with(suffix);
        });
    };
    EXPECT_EQ(count("webhook.user.created", "success"), 2);
    EXPECT_EQ(count("webhook.user.updated", "success"), 2);
    EXPECT_EQ(count("webhook.user.deleted", "success"), 1);
    EXPECT_EQ(count("webhook.user.deleted", "failure"), 1);
    EXPECT_EQ(count("webhook", "denied"), 2) << "the replay and the forged delivery";
}

} // namespace

TEST(ClerkWebhooks, SvixSignaturesAreVerified) {
    const json body = event("user.created", user("user_1", "a@example.test", "A"));
    const auto valid = delivery_of("msg_1", body);
    EXPECT_TRUE(lake::api::clerk_webhooks::verify(secret, valid));
    // Several signatures (a rotated secret): any one matching is enough.
    Delivery several = valid;
    several.signature = "v1,AAAA " + valid.signature;
    EXPECT_TRUE(lake::api::clerk_webhooks::verify(secret, several));
    EXPECT_FALSE(lake::api::clerk_webhooks::verify(other_secret, valid)) << "wrong secret";
    Delivery tampered = valid;
    tampered.body[tampered.body.size() - 2] ^= 1;
    EXPECT_FALSE(lake::api::clerk_webhooks::verify(secret, tampered)) << "tampered body";
    Delivery renamed = valid;
    renamed.id = "msg_2";
    EXPECT_FALSE(lake::api::clerk_webhooks::verify(secret, renamed)) << "the id is signed";
    EXPECT_FALSE(
        lake::api::clerk_webhooks::verify(secret, delivery_of("msg_1", body, secret, now() - 301)))
        << "too old";
    EXPECT_FALSE(
        lake::api::clerk_webhooks::verify(secret, delivery_of("msg_1", body, secret, now() + 301)))
        << "too far ahead";
    EXPECT_TRUE(
        lake::api::clerk_webhooks::verify(secret, delivery_of("msg_1", body, secret, now() - 290)));
    Delivery missing = valid;
    missing.signature.clear();
    EXPECT_FALSE(lake::api::clerk_webhooks::verify(secret, missing));
    // A known vector: Svix's documented example.
    const Delivery svix{
        "msg_p5jXN8AQM9LWM0D4loKWxJek", "1614265330", "", R"({"test": 2432232314})"};
    EXPECT_EQ(lake::api::clerk_webhooks::sign("whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw", svix),
              "g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE=");
}

TEST(ClerkWebhooks, MemoryStoreCreatesUpdatesAndDisablesAccounts) {
    lake::control::InMemoryStore store;
    expect_webhook_contract(store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(ClerkWebhooks, PostgresStoreCreatesUpdatesAndDisablesAccounts) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    expect_webhook_contract(*store, suffix);
    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    const auto disabled = tx.exec_params(
        "SELECT u.disabled_at IS NOT NULL, (SELECT count(*) FROM sessions s WHERE s.user_id = u.id "
        "AND s.revoked_at IS NULL) FROM users u JOIN credentials c ON c.user_id = u.id "
        "WHERE c.subject = $1",
        "clerk:user_hook_" + suffix);
    ASSERT_EQ(disabled.size(), 1U);
    EXPECT_TRUE(disabled[0][0].as<bool>());
    EXPECT_EQ(disabled[0][1].as<int>(), 0);
    const auto deliveries = tx.exec_params(
        "SELECT count(*) FROM clerk_webhook_deliveries WHERE svix_id LIKE $1", "%" + suffix);
    EXPECT_EQ(deliveries[0][0].as<int>(), 5) << "each applied delivery, once";
}

TEST(ClerkWebhooks, AUserCreatedWithAnExistingVerifiedEmailLinksToThatAccount) {
    lake::control::InMemoryStore store;
    const auto first = store.sign_in_clerk(sign_in("user_original", "same@example.test"));
    ASSERT_TRUE(first);
    const auto principal = store.context_for(first->secret);
    const auto linked =
        deliver(store,
                delivery_of("msg_link",
                            event("user.created", user("user_second", "same@example.test", "S"))));
    EXPECT_EQ(linked.body.at("linked"), true);
    EXPECT_EQ(linked.body.at("user_id"), principal->user_id);
    // An unverified address never links: a new account without that email.
    const auto unlinked = deliver(
        store,
        delivery_of("msg_unverified",
                    event("user.created", user("user_third", "same@example.test", "T", false))));
    EXPECT_EQ(unlinked.body.at("created"), true);
    EXPECT_NE(unlinked.body.at("user_id"), principal->user_id);
}

namespace {

void expect_upstream_revocation(lake::control::Store& store) {
    auto identity = sign_in("user_recovered", "recovered@example.test");
    const auto first = store.sign_in_clerk(identity);
    const auto second_exchange = store.sign_in_clerk(identity);
    const auto other_device = store.sign_in_clerk(identity);
    const auto unrelated = store.sign_in_clerk(sign_in("user_unrelated", "other@example.test"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second_exchange);
    ASSERT_TRUE(other_device);
    ASSERT_TRUE(unrelated);
    const auto principal = store.context_for(first->secret);
    ASSERT_TRUE(principal);
    const auto child = store.create_token(
        {principal->user_id, "lake", "child", "", {"*"}, 3600, false, first->session.id});
    ASSERT_TRUE(child) << child.error().message;
    const auto grandchild = store.create_token(
        {principal->user_id, "lake", "grandchild", "", {"*"}, 3600, false, child->session.id});
    ASSERT_TRUE(grandchild);
    ASSERT_TRUE(store.context_for(grandchild->secret));

    const auto revoked_event =
        event("session.revoked", {{"id", "sess_compromised"}, {"user_id", "user_recovered"}});
    // Only correctly signed deliveries may affect access.
    EXPECT_EQ(deliver(store, delivery_of("msg_forged_revoke", revoked_event, other_secret)).status,
              401);
    EXPECT_TRUE(store.context_for(first->secret));
    const auto revoked = deliver(store, delivery_of("msg_revoke", revoked_event));
    ASSERT_EQ(revoked.status, 200) << revoked.body;
    EXPECT_EQ(revoked.body.value("applied", false), true);
    EXPECT_FALSE(store.context_for(first->secret));
    EXPECT_FALSE(store.context_for(second_exchange->secret));
    EXPECT_FALSE(store.context_for(child->secret));
    EXPECT_FALSE(store.context_for(grandchild->secret));
    EXPECT_FALSE(store.context_for(other_device->secret)) << "revocation is account-wide";
    EXPECT_TRUE(store.context_for(unrelated->secret));
    EXPECT_FALSE(store.sign_in_clerk(identity))
        << "an unexpired JWT cannot be exchanged after revocation";
    EXPECT_FALSE(store.create_token(
        {principal->user_id, "lake", "after", "", {"*"}, 3600, false, first->session.id}));
    EXPECT_EQ(
        deliver(store, delivery_of("msg_revoke", revoked_event)).body.value("duplicate", false),
        true);
    identity.issued_at = revoked_event.at("timestamp").get<std::int64_t>() / 1000 + 1;
    const auto recovered = store.sign_in_clerk(identity);
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_TRUE(store.context_for(recovered->secret));
    auto older_event = revoked_event;
    older_event["timestamp"] = revoked_event.at("timestamp").get<std::int64_t>() - 1;
    deliver(store, delivery_of("msg_old_revoke", older_event));
    EXPECT_TRUE(store.context_for(recovered->secret))
        << "stale revocation cannot revoke a newer sign-in";

    // Revocation is retained when it precedes every exchange and even provisioning.
    for (const std::string type : {"session.revoked", "session.ended", "session.removed"}) {
        const std::string sid = "sess_early_" + type;
        EXPECT_EQ(deliver(store,
                          delivery_of("msg_" + type,
                                      event(type, {{"id", sid}, {"user_id", "user_future"}})))
                      .status,
                  200);
        auto future = sign_in("user_future", "future@example.test");
        EXPECT_FALSE(store.sign_in_clerk(future));
    }
    auto missing = identity;
    missing.issued_at = 0;
    EXPECT_FALSE(store.sign_in_clerk(missing));
    EXPECT_FALSE(store.sign_in_oauth(
        {"lake", "clerk", "user_recovered", "recovered@example.test", "", "browser", ""}))
        << "generic OAuth cannot skip the Clerk exchange";
    EXPECT_EQ(deliver(store, delivery_of("msg_bad_revoke", event("session.revoked", {{"id", ""}})))
                  .status,
              400);
}

void expect_upstream_suspension(lake::control::Store& store) {
    auto identity = sign_in("user_suspended", "suspended@example.test");
    const auto browser = store.sign_in_clerk(identity);
    const auto other_browser = store.sign_in_clerk(identity);
    const auto unrelated = store.sign_in_clerk(sign_in("user_ok", "ok@example.test"));
    ASSERT_TRUE(browser);
    ASSERT_TRUE(other_browser);
    ASSERT_TRUE(unrelated);
    const auto principal = store.context_for(browser->secret);
    ASSERT_TRUE(principal);
    const auto token = store.create_token(
        {principal->user_id, "lake", "delegated", "", {"*"}, 3600, false, browser->session.id});
    const auto independent = store.create_token(
        {principal->user_id, "lake", "independent", "static test credential", {"*"}, 0, true});
    ASSERT_TRUE(token);
    ASSERT_TRUE(independent);
    auto update = [&](const std::string& id,
                      std::int64_t timestamp,
                      bool banned,
                      bool locked,
                      const std::string& email = "suspended@example.test") {
        auto data = user("user_suspended", email, "Suspended");
        data["updated_at"] = timestamp;
        data["banned"] = banned;
        data["locked"] = locked;
        return deliver(store, delivery_of(id, event("user.updated", data)));
    };
    // A profile conflict cannot prevent the security state from being applied.
    EXPECT_EQ(update("msg_ban", 100, true, false, "ok@example.test").status, 200);
    EXPECT_FALSE(store.context_for(browser->secret));
    EXPECT_FALSE(store.context_for(other_browser->secret));
    EXPECT_FALSE(store.context_for(token->secret));
    EXPECT_FALSE(store.context_for(independent->secret));
    EXPECT_FALSE(store.sign_in_clerk(identity));
    EXPECT_TRUE(store.context_for(unrelated->secret));
    EXPECT_EQ(update("msg_old_unban", 99, false, false).status, 200);
    EXPECT_EQ(update("msg_equal_unban", 100, false, false).status, 200);
    EXPECT_FALSE(store.sign_in_clerk(identity));
    EXPECT_EQ(update("msg_unban", 101, false, false).status, 200);
    const auto restored = store.sign_in_clerk(identity);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(store.context_for(restored->secret));
    EXPECT_FALSE(store.context_for(browser->secret));
    EXPECT_FALSE(store.context_for(token->secret));
    EXPECT_FALSE(store.context_for(independent->secret));
    EXPECT_EQ(update("msg_old_ban", 100, true, false).status, 200);
    EXPECT_TRUE(store.context_for(restored->secret));
    EXPECT_EQ(update("msg_equal_ban", 101, true, false).status, 200);
    EXPECT_FALSE(store.context_for(restored->secret)) << "equal-timestamp restriction wins";
    EXPECT_EQ(update("msg_equal_restore", 101, false, false).status, 200);
    EXPECT_FALSE(store.sign_in_clerk(identity));
    EXPECT_EQ(update("msg_lock", 102, false, true).status, 200);
    EXPECT_FALSE(store.context_for(restored->secret));
    EXPECT_FALSE(store.sign_in_clerk(identity));
    EXPECT_EQ(update("msg_unlock", 103, false, false).status, 200);
    EXPECT_TRUE(store.sign_in_clerk(identity));
    EXPECT_FALSE(store.context_for(restored->secret));

    // A malformed or incomplete update cannot unban the account.
    EXPECT_EQ(update("msg_reban", 104, true, false).status, 200);
    auto malformed = user("user_suspended", "suspended@example.test", "Bad");
    malformed.erase("banned");
    EXPECT_EQ(deliver(store, delivery_of("msg_bad_state", event("user.updated", malformed))).status,
              400);
    EXPECT_FALSE(store.sign_in_clerk(identity));

    // The state exists even before a user has ever exchanged a token.
    auto early = user("user_early_ban", "early@example.test", "Early");
    early["banned"] = true;
    early["updated_at"] = 200;
    EXPECT_EQ(deliver(store, delivery_of("msg_early_ban", event("user.updated", early))).status,
              200);
    auto future = sign_in("user_early_ban", "early@example.test");
    EXPECT_FALSE(store.sign_in_clerk(future));
    early["banned"] = false;
    early["updated_at"] = 199;
    deliver(store, delivery_of("msg_early_stale", event("user.updated", early)));
    EXPECT_FALSE(store.sign_in_clerk(future));
    early["updated_at"] = 201;
    deliver(store, delivery_of("msg_early_unban", event("user.updated", early)));
    EXPECT_TRUE(store.sign_in_clerk(future));

    auto stale_deletion = event("user.deleted", {{"id", "user_early_ban"}});
    stale_deletion["timestamp"] = 200;
    deliver(store, delivery_of("msg_old_delete", stale_deletion));
    EXPECT_TRUE(store.sign_in_clerk(future));

    // Deletion stays terminal even after a newer unsuspended profile arrives.
    EXPECT_EQ(
        deliver(store,
                delivery_of("msg_delete_state", event("user.deleted", {{"id", identity.subject}})))
            .status,
        200);
    EXPECT_EQ(update("msg_post_delete", now() * 1000 + 20000, false, false).status, 200);
    EXPECT_FALSE(store.sign_in_clerk(identity));
}

class ClerkRevocation : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            const char* uri = lake::test::postgres_uri();
            if (uri == nullptr || *uri == '\0')
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            store = lake::control::make_postgres_store(uri, std::string(instance_kek));
        } else {
            store = std::make_shared<lake::control::InMemoryStore>();
        }
    }
    std::shared_ptr<lake::control::Store> store;
};

TEST_P(ClerkRevocation, SessionRevocationStopsExchangesAndDescendants) {
    expect_upstream_revocation(*store);
}
TEST_P(ClerkRevocation, OrderedBansAndLocksOnlyRestoreNewSignIns) {
    expect_upstream_suspension(*store);
}
TEST_P(ClerkRevocation, SuspensionAlsoRefusesLinkedPasswordAuthentication) {
    const auto identity = sign_in("user_native_alias", "native-alias@example.test");
    const std::string password = "long-native-test-password";
    lake::control::NewPasswordUser input{.pool_id = "lake",
                                         .email = identity.email,
                                         .password = password,
                                         .custodial_taap_id = identity.custodial_taap_id,
                                         .custodial_public_key = identity.custodial_public_key,
                                         .custodial_wrapped_seed = identity.custodial_wrapped_seed};
    const auto native = store->sign_up_password(input);
    ASSERT_TRUE(native);
    ASSERT_TRUE(store->sign_in_clerk(identity));
    ASSERT_TRUE(store->apply_clerk_user_state({identity.subject, 100, true, false}));
    EXPECT_FALSE(store->sign_in_password({"lake", identity.email, password, "", "test"}));
    EXPECT_FALSE(store->context_for(native->secret));
    ASSERT_TRUE(store->apply_clerk_user_state({identity.subject, 101, false, false}));
    EXPECT_TRUE(store->sign_in_password({"lake", identity.email, password, "", "test"}));
    EXPECT_FALSE(store->context_for(native->secret));
}
TEST_P(ClerkRevocation, LinkedSubjectsShareSuspensionAndRevocation) {
    auto first = sign_in("user_alias_a", "alias@example.test");
    auto second = sign_in("user_alias_b", "alias@example.test");
    const auto a = store->sign_in_clerk(first);
    const auto b = store->sign_in_clerk(second);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    const auto principal = store->context_for(a->secret);
    ASSERT_TRUE(principal);
    EXPECT_EQ(store->context_for(b->secret)->user_id, principal->user_id);
    ASSERT_TRUE(store->apply_clerk_user_state({first.subject, 100, true}));
    EXPECT_FALSE(store->context_for(a->secret));
    EXPECT_FALSE(store->context_for(b->secret));
    EXPECT_FALSE(store->sign_in_clerk(second));
    ASSERT_TRUE(store->apply_clerk_user_state({second.subject, 99, false}));
    const auto stale = store->apply_clerk_user_state({second.subject, 98, false});
    ASSERT_TRUE(stale);
    EXPECT_FALSE(*stale);
    // Another subject's fresh normal profile cannot clear the suspended subject.
    ASSERT_TRUE(store->apply_clerk_user_state({second.subject, 101, false}));
    EXPECT_FALSE(store->sign_in_clerk(second));
    ASSERT_TRUE(store->apply_clerk_user_state({first.subject, 102, false}));
    ASSERT_TRUE(store->sign_in_clerk(second));
    const auto revoked_at = now() * 1000;
    ASSERT_TRUE(store->revoke_clerk_sessions(first.subject, revoked_at));
    EXPECT_FALSE(store->sign_in_clerk(second))
        << "an alias cannot exchange a pre-revocation template";
    second.issued_at = revoked_at / 1000 + 1;
    EXPECT_TRUE(store->sign_in_clerk(second));
    EXPECT_FALSE(store->context_for(a->secret));
    EXPECT_FALSE(store->context_for(b->secret));
}
TEST_P(ClerkRevocation, ACurrentStateRetryCanFinishProvisioning) {
    auto data = user("user_retry", "retry@example.test", "Retry");
    const auto timestamp = data.at("updated_at").get<std::int64_t>();
    ASSERT_TRUE(store->apply_clerk_user_state({"user_retry", timestamp, false}));
    // Simulate a failure after persisting state but before provisioning or recording svix-id.
    const auto created = deliver(*store, delivery_of("msg_retry", event("user.created", data)));
    ASSERT_EQ(created.status, 200);
    ASSERT_TRUE(created.body.contains("user_id")) << created.body;
    EXPECT_EQ(created.body.at("created"), true);
    const auto repeated =
        deliver(*store, delivery_of("msg_retry_again", event("user.created", data)));
    EXPECT_EQ(repeated.body.at("created"), false);
    EXPECT_EQ(repeated.body.at("user_id"), created.body.at("user_id"));
}
TEST_P(ClerkRevocation, ProvisioningASuspendedAliasPermanentlyRevokesExistingCredentials) {
    auto identity = sign_in("user_existing", "existing@example.test");
    const auto original = store->sign_in_clerk(identity);
    ASSERT_TRUE(original);
    const auto principal = store->context_for(original->secret);
    ASSERT_TRUE(principal);
    auto data = user("user_suspended_alias", "existing@example.test", "Alias");
    data["banned"] = true;
    data["updated_at"] = 100;
    const auto linked =
        deliver(*store, delivery_of("msg_suspended_alias", event("user.created", data)));
    ASSERT_EQ(linked.status, 200);
    ASSERT_TRUE(linked.body.contains("user_id")) << linked.body;
    EXPECT_EQ(linked.body.at("user_id"), principal->user_id);
    EXPECT_FALSE(store->context_for(original->secret));
    EXPECT_FALSE(store->sign_in_clerk(identity));
    data["banned"] = false;
    data["updated_at"] = 101;
    deliver(*store, delivery_of("msg_alias_unban", event("user.updated", data)));
    EXPECT_TRUE(store->sign_in_clerk(identity));
    EXPECT_FALSE(store->context_for(original->secret))
        << "unban cannot revive credentials from before linking";
}
TEST_P(ClerkRevocation, AnotherSubjectsNewerProfileCannotHideABan) {
    auto first = sign_in("user_delayed_ban", "ordered-alias@example.test");
    auto second = sign_in("user_recent_profile", "ordered-alias@example.test");
    const auto a = store->sign_in_clerk(first);
    const auto b = store->sign_in_clerk(second);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    auto newer = user(second.subject, second.email, "Recent");
    newer["updated_at"] = 101;
    ASSERT_EQ(
        deliver(*store, delivery_of("msg_recent_profile", event("user.updated", newer))).status,
        200);
    auto banned = user(first.subject, first.email, "Banned");
    banned["updated_at"] = 100;
    banned["banned"] = true;
    ASSERT_EQ(deliver(*store, delivery_of("msg_delayed_ban", event("user.updated", banned))).status,
              200);
    EXPECT_FALSE(store->context_for(a->secret));
    EXPECT_FALSE(store->context_for(b->secret));
    EXPECT_FALSE(store->sign_in_clerk(first));
    banned["banned"] = false;
    banned["updated_at"] = 99;
    deliver(*store, delivery_of("msg_stale_alias_unban", event("user.updated", banned)));
    EXPECT_FALSE(store->sign_in_clerk(first));
    banned["updated_at"] = 100;
    deliver(*store, delivery_of("msg_equal_alias_unban", event("user.updated", banned)));
    EXPECT_FALSE(store->sign_in_clerk(first));
    banned["updated_at"] = 101;
    deliver(*store, delivery_of("msg_new_alias_unban", event("user.updated", banned)));
    EXPECT_TRUE(store->sign_in_clerk(first));
    EXPECT_FALSE(store->context_for(a->secret));
    EXPECT_FALSE(store->context_for(b->secret));
}
TEST_P(ClerkRevocation, DirectorySyncDeprovisioningSuspendsExistingAndNewCredentials) {
    auto identity = sign_in("user_directory", "directory@example.test");
    const auto original = store->sign_in_clerk(identity);
    ASSERT_TRUE(original);
    auto data = user(identity.subject, identity.email, "Directory");
    data["deprovisioned"] = true;
    data["updated_at"] = 100;
    ASSERT_EQ(deliver(*store, delivery_of("msg_deprovision", event("user.updated", data))).status,
              200);
    EXPECT_FALSE(store->context_for(original->secret));
    EXPECT_FALSE(store->sign_in_clerk(identity));
    data["deprovisioned"] = "false";
    data["updated_at"] = 101;
    EXPECT_EQ(deliver(*store, delivery_of("msg_bad_provision", event("user.updated", data))).status,
              400);
    EXPECT_FALSE(store->sign_in_clerk(identity));
    data["deprovisioned"] = false;
    data["updated_at"] = 100;
    deliver(*store, delivery_of("msg_equal_provision", event("user.updated", data)));
    EXPECT_FALSE(store->sign_in_clerk(identity));
    data["updated_at"] = 101;
    deliver(*store, delivery_of("msg_provision", event("user.updated", data)));
    EXPECT_TRUE(store->sign_in_clerk(identity));
    EXPECT_FALSE(store->context_for(original->secret));
}
INSTANTIATE_TEST_SUITE_P(Stores, ClerkRevocation, testing::Bool(), [](const auto& info) {
    return info.param ? "Postgres" : "Memory";
});

} // namespace

TEST(ClerkWebhooks, MigrationRevokesLegacyCredentialsOnceAndRejectsOldTemplates) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0')
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    auto store = lake::control::make_postgres_store(uri, std::string(instance_kek));
    auto identity = sign_in("user_legacy", "legacy@example.test");
    const auto legacy = store->sign_in_clerk(identity);
    ASSERT_TRUE(legacy);
    const auto principal = store->context_for(legacy->secret);
    ASSERT_TRUE(principal);
    const auto descendant = store->create_token(
        {principal->user_id, "lake", "legacy-child", "", {"*"}, 3600, false, legacy->session.id});
    ASSERT_TRUE(descendant);
    ASSERT_TRUE(
        store->record_clerk_webhook("msg_legacy_ignored", "session.revoked", "sess_legacy"));
    const auto native_identity = sign_in("unused_native_subject", "native@example.test");
    const auto native_user =
        store->create_user({native_identity.custodial_taap_id, native_identity.email, "Native"});
    ASSERT_TRUE(native_user);
    const auto native = store->create_token({native_user->id, "lake", "native", "", {"*"}, 3600});
    ASSERT_TRUE(native);
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        tx.exec("DELETE FROM schema_migrations WHERE version = '0094_clerk_revocation'");
        tx.commit();
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    ASSERT_TRUE(file);
    const std::string schema((std::istreambuf_iterator<char>(file)), {});
    const auto apply = [&] {
        pqxx::work tx(connection);
        tx.exec("SET LOCAL client_min_messages TO error");
        tx.exec(schema);
        tx.commit();
    };
    const auto cutoff = [&] {
        pqxx::read_transaction tx(connection);
        return tx
            .exec_params("SELECT revoked_before FROM clerk_user_states WHERE subject = $1",
                         identity.subject)[0][0]
            .as<std::int64_t>();
    };
    apply();
    EXPECT_FALSE(store->context_for(legacy->secret));
    const auto duplicate =
        deliver(*store,
                delivery_of("msg_legacy_ignored",
                            event("session.revoked",
                                  {{"id", "sess_legacy"}, {"user_id", identity.subject}})));
    EXPECT_EQ(duplicate.body.value("duplicate", false), true);
    EXPECT_FALSE(store->context_for(descendant->secret));
    EXPECT_FALSE(store->sign_in_clerk(identity)) << "a pre-migration template cannot re-exchange";
    EXPECT_TRUE(store->context_for(native->secret))
        << "accounts without Clerk links are unaffected";
    const auto migrated_at = cutoff();
    identity.issued_at = migrated_at / 1000 + 1;
    const auto fresh = store->sign_in_clerk(identity);
    ASSERT_TRUE(fresh) << fresh.error().message;
    ASSERT_TRUE(store->context_for(fresh->secret));
    apply();
    EXPECT_EQ(cutoff(), migrated_at);
    EXPECT_TRUE(store->context_for(fresh->secret))
        << "reapplication never revokes later credentials";
    EXPECT_FALSE(store->context_for(legacy->secret));
}
