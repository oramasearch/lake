// Clerk webhooks (lake#765, docs/api/clerk.md#webhooks): Svix verification and the user events
// that create, update and disable Lake accounts.
#include "audit_test_support.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
    return {
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
    return {{"type", type}, {"object", "event"}, {"data", data}};
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
    return {
        subject, email, "", "browser", minted->taap_id, minted->public_key, minted->wrapped_seed};
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
