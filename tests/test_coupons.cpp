// Coupon codes for ads (lake#1156, docs/api/billing.md#coupon-codes): only the operator or the
// instance administrator creates, lists and disables codes; a person redeems one for an account
// they administer, and the redemption is an ordinary grant from Orama (lake#1134) with the
// reference coupon:<coupon id>:<customer id>. One redemption per account and per verified email,
// the total cap, the code's expiry, disabling, new-accounts-only, the limit on failed attempts, the
// campaign report, and the central audit that never keeps a refused code. The store contract runs
// on both stores (PostgreSQL against LAKE_TEST_DB_URI).

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "audit_test_support.hpp"
#include "lake/api/balance.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"

namespace {

using nlohmann::json;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";
constexpr std::int64_t dollar = 1'000'000;

std::string utc_in(std::chrono::seconds offset) {
    return std::format(
        "{:%FT%T}Z",
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() + offset));
}

struct Person {
    std::string user_id, taap_id, customer_id, email;
};

struct CouponRig {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::api::Registry registry;
    std::shared_ptr<lake::api::BalanceRuntime> runtime =
        std::make_shared<lake::api::BalanceRuntime>();
    std::vector<std::pair<lake::api::Email, std::string>> sent;
    int people = 0;

    CouponRig() {
        lake::api::register_core_capabilities(registry, "test");
        lake::api::register_billing_capabilities(registry,
                                                 lake::api::manual_payment_provider("Pay us."));
        runtime->origin = "https://lake.test";
        runtime->mailer = [this](const lake::api::Email& email,
                                 std::string_view key) -> std::expected<std::string, std::string> {
            sent.emplace_back(email, std::string(key));
            return "email-" + std::to_string(sent.size());
        };
    }

    // A person with their own account, which they administer.
    Person person(const std::string& email) {
        Person out;
        out.email = email;
        out.taap_id = "taap:1:ed25519:coupon-person-" + std::to_string(++people);
        const auto user = store->create_user({out.taap_id, email, "Person"});
        EXPECT_TRUE(user) << user.error().message;
        out.user_id = user->id;
        const auto customer = store->create_customer(
            {"individual", user->id, "", "coupon-person-" + std::to_string(people)});
        EXPECT_TRUE(customer) << customer.error().message;
        out.customer_id = customer->id;
        const auto object = store->find_policy_object("customer", out.customer_id);
        EXPECT_TRUE(object) << object.error().message;
        EXPECT_TRUE(store->create_grant(
            {out.taap_id, object->id, "admin", "attestation-" + out.taap_id, "admin", "request"}));
        return out;
    }

    lake::api::ApiContext operator_context() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        context.balance = runtime;
        context.registry = &registry;
        context.external_origin = "https://lake.test";
        context.client_address = "198.51.100.1";
        return context;
    }

    lake::api::ApiContext as(const Person& who, std::string client = "198.51.100.7") {
        auto context = operator_context();
        context.operator_credential = false;
        context.acting_identity = who.taap_id;
        context.acting_user_id = who.user_id;
        context.session_credential = true;
        context.scopes = {"*"};
        context.client_address = std::move(client);
        return context;
    }

    lake::api::ApiResult call(const std::string& name, json input, lake::api::ApiContext context) {
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }

    lake::api::ApiResult admin(const std::string& name, json input) {
        return call(name, std::move(input), operator_context());
    }

    lake::api::ApiResult
    redeem(const Person& who, const std::string& code, std::string client = "198.51.100.7") {
        return call("redeem_coupon",
                    {{"customer_id", who.customer_id}, {"code", code}},
                    as(who, std::move(client)));
    }

    std::string create(json input) {
        const auto created = admin("create_coupon", std::move(input));
        EXPECT_TRUE(created.ok) << created.error.message;
        return created.ok ? created.value["coupon"]["code"].get<std::string>() : std::string();
    }

    lake::control::AccountBalance balance(const Person& who) {
        const auto read = store->account_balance(who.customer_id);
        EXPECT_TRUE(read) << read.error().message;
        return read ? *read : lake::control::AccountBalance{};
    }
};

std::vector<lake::audit::Event> audit_of(lake::control::Store& store, const std::string& kind) {
    std::vector<lake::audit::Event> out;
    const auto events = lake::test::audit_events(store);
    EXPECT_TRUE(events);
    for (const auto& event : events ? *events : std::vector<lake::audit::Event>{}) {
        if (event.draft.kind == kind) {
            out.push_back(event);
        }
    }
    return out;
}

std::string reason_of(const lake::api::ApiResult& result) {
    return result.error.details.is_object() ? result.error.details.value("reason", "") : "";
}

} // namespace

TEST(Coupon, CodesAreNormalizedAndEmailsDigestedPerInbox) {
    using lake::control::coupon_code_normalized;
    using lake::control::coupon_email_digest;
    EXPECT_EQ(coupon_code_normalized("lake50"), "LAKE50");
    EXPECT_EQ(coupon_code_normalized(" Lake-50 "), "LAKE50");
    EXPECT_EQ(coupon_code_normalized("ab"), std::nullopt) << "at least 4";
    EXPECT_EQ(coupon_code_normalized(std::string(33, 'A')), std::nullopt) << "at most 32";
    EXPECT_EQ(coupon_code_normalized("LAKE_50"), std::nullopt);
    EXPECT_EQ(coupon_code_normalized("LAKÉ50"), std::nullopt);
    EXPECT_EQ(coupon_email_digest("Pat.Doe+ads@Gmail.com"),
              coupon_email_digest("patdoe@gmail.com"));
    EXPECT_EQ(coupon_email_digest("patdoe@googlemail.com"),
              coupon_email_digest("patdoe@gmail.com"));
    EXPECT_EQ(coupon_email_digest("pat+x@example.com"), coupon_email_digest("PAT@example.com"));
    EXPECT_NE(coupon_email_digest("pat.doe@example.com"), coupon_email_digest("patdoe@example.com"))
        << "dots matter outside Gmail";
    EXPECT_EQ(coupon_email_digest("").size(), 0U);
    EXPECT_EQ(coupon_email_digest("a@b.c").size(), 64U);
}

TEST(Coupon, OnlyTheOperatorOrTheInstanceAdministratorCreatesListsOrDisables) {
    CouponRig rig;
    const Person owner = rig.person("owner@example.test");
    const json make{{"code", "LAKE50"}, {"amount_micros", 50 * dollar}};
    auto refused = rig.call("create_coupon", make, rig.as(owner));
    EXPECT_EQ(refused.error.code, "unauthorized") << refused.error.message;
    refused = rig.call("list_coupons", json::object(), rig.as(owner));
    EXPECT_EQ(refused.error.code, "unauthorized") << refused.error.message;
    // The instance administrator without the operator credential may.
    auto admin = rig.operator_context();
    admin.operator_credential = false;
    const auto created = rig.call("create_coupon", make, admin);
    ASSERT_TRUE(created.ok) << created.error.message;
    refused = rig.call("disable_coupon", {{"code", "LAKE50"}}, rig.as(owner));
    EXPECT_EQ(refused.error.code, "unauthorized") << refused.error.message;
    EXPECT_EQ(rig.store->coupon_by_code("LAKE50")->disabled_at, "");
    // The operator cannot redeem: a code is a person's.
    auto as_operator = rig.operator_context();
    const auto by_operator = rig.call(
        "redeem_coupon", {{"customer_id", owner.customer_id}, {"code", "LAKE50"}}, as_operator);
    EXPECT_EQ(by_operator.error.code, "unauthorized") << by_operator.error.message;
    // Nor can a stranger redeem into someone else's account.
    const Person stranger = rig.person("stranger@example.test");
    const auto into_other = rig.call("redeem_coupon",
                                     {{"customer_id", owner.customer_id}, {"code", "LAKE50"}},
                                     rig.as(stranger));
    EXPECT_EQ(into_other.error.code, "unauthorized") << into_other.error.message;
    EXPECT_EQ(rig.balance(owner).credits_micros, 0);
}

TEST(Coupon, CreatingChecksItsInputGeneratesUnguessableCodesAndAnswersTheAdLink) {
    CouponRig rig;
    const auto created = rig.admin("create_coupon",
                                   {{"code", "lake-50"},
                                    {"amount_micros", 50 * dollar},
                                    {"credit_days", 90},
                                    {"campaign", "google-2026-11"},
                                    {"max_redemptions", 500}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const json& coupon = created.value["coupon"];
    EXPECT_EQ(coupon["code"], "LAKE50") << "stored normalized";
    EXPECT_EQ(coupon["amount_micros"], 50 * dollar);
    EXPECT_EQ(coupon["credit_days"], 90);
    EXPECT_EQ(coupon["new_accounts_only"], true) << "the default for ads";
    EXPECT_EQ(coupon["max_redemptions"], 500);
    EXPECT_EQ(coupon["campaign"], "google-2026-11");
    EXPECT_EQ(coupon["state"], "active");
    EXPECT_EQ(coupon["created_by"], "operator");
    EXPECT_EQ(created.value["link"], "https://lake.test/?code=LAKE50");
    EXPECT_EQ(created.value["other_regions"], json::array());

    // The same code in another case is taken.
    auto taken = rig.admin("create_coupon", {{"code", "Lake50"}, {"amount_micros", dollar}});
    EXPECT_EQ(taken.error.code, "conflict") << taken.error.message;
    for (const json& bad :
         {json{{"code", "no_way"}, {"amount_micros", dollar}},
          json{{"code", "OKAY"}, {"amount_micros", dollar + 1}},
          json{{"code", "OKAY"}, {"amount_micros", 0}},
          json{{"code", "OKAY"}, {"amount_micros", dollar}, {"credit_days", 0}},
          json{{"code", "OKAY"}, {"amount_micros", dollar}, {"campaign", "Ads"}},
          json{{"code", "OKAY"},
               {"amount_micros", dollar},
               {"expires_at", utc_in(-std::chrono::hours{1})}},
          json{{"code", "OKAY"}, {"amount_micros", dollar}, {"expires_at", "soon"}},
          json{{"code", "OKAY"}, {"amount_micros", dollar}, {"max_redemptions", 0}}}) {
        EXPECT_EQ(rig.admin("create_coupon", bad).error.code, "invalid_input") << bad.dump();
    }

    // Generated: 16 characters without look-alikes, different every time.
    std::set<std::string> generated;
    for (int i = 0; i < 20; ++i) {
        const std::string code = rig.create({{"amount_micros", 5 * dollar}});
        ASSERT_EQ(code.size(), 16U) << code;
        EXPECT_EQ(code.find_first_of("01ILOU"), std::string::npos) << code;
        generated.insert(code);
    }
    EXPECT_EQ(generated.size(), 20U);

    // With regions, the link is the apex picker and the other regions are named.
    lake::api::Regions regions;
    regions.name = "swedencentral";
    regions.label = "Europe";
    regions.apex_host = "lake.example.com";
    regions.list = {{"swedencentral", "Europe", "eu.lake.example.com", 59.3, 18.0},
                    {"eastus", "United States", "us.lake.example.com", 37.4, -79.0}};
    auto context = rig.operator_context();
    context.regions = &regions;
    const auto regional =
        rig.call("create_coupon", {{"code", "EU50"}, {"amount_micros", 50 * dollar}}, context);
    ASSERT_TRUE(regional.ok) << regional.error.message;
    EXPECT_EQ(regional.value["link"], "https://lake.example.com/?code=EU50");
    EXPECT_EQ(regional.value["region"]["name"], "swedencentral");
    ASSERT_EQ(regional.value["other_regions"].size(), 1U);
    EXPECT_EQ(regional.value["other_regions"][0]["host"], "us.lake.example.com");

    const auto created_events = audit_of(*rig.store, "billing.coupon.created");
    ASSERT_FALSE(created_events.empty());
    EXPECT_EQ(created_events[0].draft.object, "billing/coupons");
    EXPECT_EQ(created_events[0].draft.details["campaign"], "google-2026-11");
}

TEST(Coupon, ARedemptionIsAnOrdinaryGrantShownEmailedAndAuditedCentrally) {
    CouponRig rig;
    rig.create({{"code", "LAKE50"},
                {"amount_micros", 50 * dollar},
                {"credit_days", 90},
                {"campaign", "google-2026-11"}});
    const Person newcomer = rig.person("newcomer@example.test");
    const auto redeemed = rig.redeem(newcomer, "lake50");
    ASSERT_TRUE(redeemed.ok) << redeemed.error.message;
    EXPECT_EQ(redeemed.value["code"], "LAKE50");
    EXPECT_EQ(redeemed.value["amount_micros"], 50 * dollar);
    EXPECT_EQ(redeemed.value["message"], "You have $50.00 of credit from Orama.");
    EXPECT_EQ(redeemed.value.contains("campaign"), false) << "attribution is the operator's";
    const std::string expires = redeemed.value["expires_at"];
    EXPECT_EQ(expires.substr(0, 10), utc_in(std::chrono::days{90}).substr(0, 10));
    EXPECT_EQ(redeemed.value["credit"]["kind"], "grant");
    EXPECT_EQ(redeemed.value["credit"]["label"], "Credit from Orama");
    EXPECT_EQ(redeemed.value["credit"]["reason"], "Coupon code LAKE50");
    EXPECT_EQ(redeemed.value["balance"]["balance_micros"], 50 * dollar);
    EXPECT_EQ(redeemed.value["balance"]["first_top_up_at"], "") << "not a payment";
    EXPECT_EQ(redeemed.value["emailed"], true);

    // The grant row: the grant path's own, with the coupon's reference.
    const auto coupon = rig.store->coupon_by_code("LAKE50");
    ASSERT_TRUE(coupon);
    const auto credits = rig.store->balance_credits(newcomer.customer_id, 0);
    ASSERT_TRUE(credits);
    ASSERT_EQ(credits->size(), 1U);
    EXPECT_EQ((*credits)[0].kind, "grant");
    EXPECT_EQ((*credits)[0].provider, "orama");
    EXPECT_EQ((*credits)[0].reference, "coupon:" + coupon->id + ":" + newcomer.customer_id);
    EXPECT_EQ((*credits)[0].granted_by, "coupon:" + coupon->id);
    EXPECT_EQ((*credits)[0].expires_at, expires);
    EXPECT_EQ(rig.balance(newcomer).balance_micros(), 50 * dollar);
    EXPECT_EQ(rig.balance(newcomer).grants_unused_micros, 50 * dollar);
    EXPECT_EQ(coupon->redemptions, 1);
    const auto redemption = rig.store->coupon_redemption(coupon->id, newcomer.customer_id);
    ASSERT_TRUE(redemption);
    EXPECT_EQ(redemption->credit_id, (*credits)[0].id);

    // One email, the grant's, keyed by the grant.
    ASSERT_EQ(rig.sent.size(), 1U);
    EXPECT_EQ(rig.sent[0].first.subject, "Orama added $50.00 of credit to your Lake account");
    EXPECT_EQ(rig.sent[0].second, "lake-balance-grant-" + (*credits)[0].id);

    // Audited centrally on the account's billing: who, which code, which campaign.
    const auto events = audit_of(*rig.store, "billing.coupon.redeemed");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].draft.principal, newcomer.taap_id);
    EXPECT_EQ(events[0].draft.object, "customer/" + newcomer.customer_id + "/billing");
    EXPECT_EQ(events[0].draft.details["code"], "LAKE50");
    EXPECT_EQ(events[0].draft.details["campaign"], "google-2026-11");
    EXPECT_EQ(events[0].draft.details["credit_id"], (*credits)[0].id);
    EXPECT_EQ(events[0].draft.outcome, "success");
}

TEST(Coupon, OneRedemptionPerAccountAndPerVerifiedEmail) {
    CouponRig rig;
    rig.create({{"code", "LAKE50"}, {"amount_micros", 50 * dollar}});
    const Person first = rig.person("pat.doe@gmail.com");
    ASSERT_TRUE(rig.redeem(first, "LAKE50").ok);
    // Again by the same account.
    auto again = rig.redeem(first, "LAKE50");
    EXPECT_EQ(again.error.code, "conflict");
    EXPECT_EQ(reason_of(again), "already_redeemed");
    // The same inbox on another account (Gmail ignores dots and +tags).
    const Person second = rig.person("patdoe+lake@gmail.com");
    const auto other = rig.redeem(second, "LAKE50", "203.0.113.9");
    EXPECT_EQ(other.error.code, "conflict");
    EXPECT_EQ(reason_of(other), "email_redeemed");
    EXPECT_EQ(rig.balance(second).credits_micros, 0);
    EXPECT_EQ(rig.balance(first).credits_micros, 50 * dollar) << "granted once";
    EXPECT_EQ(rig.store->coupon_by_code("LAKE50")->redemptions, 1);
    // Another person may.
    const Person third = rig.person("someone@example.test");
    EXPECT_TRUE(rig.redeem(third, "LAKE50", "203.0.113.10").ok);
}

TEST(Coupon, TheCapTheExpiryAndDisablingStopACode) {
    CouponRig rig;
    rig.create({{"code", "ONLYONE"}, {"amount_micros", 10 * dollar}, {"max_redemptions", 1}});
    const Person a = rig.person("a@example.test");
    const Person b = rig.person("b@example.test");
    ASSERT_TRUE(rig.redeem(a, "ONLYONE").ok);
    const auto capped = rig.redeem(b, "ONLYONE", "203.0.113.2");
    EXPECT_EQ(reason_of(capped), "used_up");
    EXPECT_EQ(capped.error.message, "This code has been used as many times as it can be.");

    // A code past its expiry (written directly: create_coupon refuses a past expiry).
    lake::control::Coupon old;
    old.code = "EXPIRED1";
    old.amount_micros = 10 * dollar;
    old.expires_at = utc_in(-std::chrono::minutes{1});
    old.new_accounts_only = false;
    old.created_by = "operator";
    ASSERT_TRUE(rig.store->create_coupon(old));
    const auto expired = rig.redeem(b, "expired1", "203.0.113.3");
    EXPECT_EQ(expired.error.code, "conflict");
    EXPECT_EQ(reason_of(expired), "expired");

    // Disabling stops a code at once; disabling again changes nothing.
    rig.create({{"code", "STOPME"}, {"amount_micros", 10 * dollar}});
    const auto disabled = rig.admin("disable_coupon", {{"code", "stop-me"}});
    ASSERT_TRUE(disabled.ok) << disabled.error.message;
    EXPECT_EQ(disabled.value["coupon"]["state"], "disabled");
    const std::string when = disabled.value["coupon"]["disabled_at"];
    const auto twice = rig.admin(
        "disable_coupon", {{"coupon_id", disabled.value["coupon"]["id"].get<std::string>()}});
    ASSERT_TRUE(twice.ok) << twice.error.message;
    EXPECT_EQ(twice.value["coupon"]["disabled_at"], when);
    const auto stopped = rig.redeem(b, "STOPME", "203.0.113.4");
    EXPECT_EQ(reason_of(stopped), "disabled");
    EXPECT_EQ(rig.balance(b).credits_micros, 0);
    EXPECT_EQ(rig.admin("disable_coupon", {{"code", "NOSUCHCODE"}}).error.code, "not_found");
    EXPECT_EQ(rig.admin("disable_coupon", json::object()).error.code, "invalid_input");
    EXPECT_EQ(audit_of(*rig.store, "billing.coupon.disabled").size(), 2U);
}

TEST(Coupon, ANewAccountsOnlyCodeRefusesOlderOrUsedAccounts) {
    CouponRig rig;
    const Person older = rig.person("older@example.test");
    rig.create({{"code", "NEWONLY"}, {"amount_micros", 25 * dollar}});
    rig.create({{"code", "ANYONE"}, {"amount_micros", 5 * dollar}, {"new_accounts_only", false}});
    // Created before the code.
    const auto refused = rig.redeem(older, "NEWONLY");
    EXPECT_EQ(reason_of(refused), "not_new_account");
    // An account made after the code, but already topped up.
    const Person used = rig.person("used@example.test");
    lake::control::BalanceCredit top_up;
    top_up.customer_id = used.customer_id;
    top_up.amount_micros = 20 * dollar;
    top_up.provider = "stripe";
    top_up.reference = "cs_used";
    ASSERT_TRUE(rig.store->apply_balance_credit(top_up));
    EXPECT_EQ(reason_of(rig.redeem(used, "NEWONLY", "203.0.113.5")), "account_used");
    // An account with charged usage.
    const Person charged = rig.person("charged@example.test");
    ASSERT_TRUE(rig.store->set_balance_charge({charged.customer_id,
                                               utc_in(std::chrono::seconds{0}).substr(0, 7),
                                               3 * dollar,
                                               "",
                                               "",
                                               ""}));
    EXPECT_EQ(reason_of(rig.redeem(charged, "NEWONLY", "203.0.113.6")), "account_used");
    // A fresh account may, and a code for anyone takes the older account.
    const Person fresh = rig.person("fresh@example.test");
    EXPECT_TRUE(rig.redeem(fresh, "NEWONLY", "203.0.113.7").ok);
    EXPECT_TRUE(rig.redeem(older, "ANYONE", "203.0.113.8").ok);
}

TEST(Coupon, FailedAttemptsAreLimitedPerClientAndPerPersonAndNeverAuditTheCode) {
    CouponRig rig;
    rig.create({{"code", "REALCODE"}, {"amount_micros", 10 * dollar}});
    const Person guesser = rig.person("guesser@example.test");
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto wrong = rig.redeem(guesser, "GUESS" + std::to_string(attempt), "192.0.2.50");
        EXPECT_EQ(wrong.error.code, "not_found") << attempt;
        EXPECT_EQ(reason_of(wrong), "unknown");
    }
    // The person is limited, even with the right code and from another address.
    const auto limited = rig.redeem(guesser, "REALCODE", "192.0.2.99");
    EXPECT_EQ(limited.error.code, "rate_limited") << limited.error.message;
    EXPECT_EQ(rig.balance(guesser).credits_micros, 0);
    // So is the client address, for another person.
    const Person neighbour = rig.person("neighbour@example.test");
    EXPECT_EQ(rig.redeem(neighbour, "REALCODE", "192.0.2.50").error.code, "rate_limited");
    // Another person elsewhere is not.
    EXPECT_TRUE(rig.redeem(neighbour, "REALCODE", "192.0.2.51").ok);

    // A refusal is audited with a short digest of what was typed, never the code itself.
    const auto refusals = audit_of(*rig.store, "billing.coupon.refused");
    ASSERT_GE(refusals.size(), 11U);
    for (const auto& event : refusals) {
        EXPECT_EQ(event.draft.details.contains("code"), false);
        EXPECT_EQ(event.draft.details["code_digest"].get<std::string>().size(), 12U);
        EXPECT_EQ(event.draft.details.dump().find("GUESS"), std::string::npos);
        EXPECT_EQ(event.draft.details.dump().find("REALCODE"), std::string::npos);
    }
    EXPECT_EQ(refusals.back().draft.details["reason"], "rate_limited");
}

TEST(Coupon, AnInterruptedRedemptionIsFinishedOnceOnRetry) {
    CouponRig rig;
    rig.create({{"code", "RESUME"}, {"amount_micros", 10 * dollar}});
    const Person who = rig.person("resume@example.test");
    const auto coupon = rig.store->coupon_by_code("RESUME");
    ASSERT_TRUE(coupon);
    // Claimed, but the grant never given (the process stopped between the two).
    lake::control::CouponRedemption claim{.coupon_id = coupon->id,
                                          .customer_id = who.customer_id,
                                          .email_sha256 =
                                              lake::control::coupon_email_digest(who.email),
                                          .redeemed_by = who.taap_id};
    ASSERT_TRUE(rig.store->claim_coupon(claim));
    const auto finished = rig.redeem(who, "RESUME");
    ASSERT_TRUE(finished.ok) << finished.error.message;
    EXPECT_EQ(rig.balance(who).credits_micros, 10 * dollar);
    EXPECT_EQ(rig.store->coupon_by_code("RESUME")->redemptions, 1) << "counted once";
    EXPECT_EQ(audit_of(*rig.store, "billing.coupon.redeemed")[0].draft.details["resumed"], true);
    EXPECT_EQ(reason_of(rig.redeem(who, "RESUME")), "already_redeemed");
}

TEST(Coupon, TheCampaignReportCountsRedemptionsFirstTopUpsAndSpend) {
    CouponRig rig;
    rig.create({{"code", "GOOG1"}, {"amount_micros", 50 * dollar}, {"campaign", "google-2026-11"}});
    rig.create({{"code", "GOOG2"}, {"amount_micros", 20 * dollar}, {"campaign", "google-2026-11"}});
    rig.create({{"code", "BING1"}, {"amount_micros", 20 * dollar}, {"campaign", "bing-2026-11"}});
    const Person a = rig.person("ca@example.test");
    const Person b = rig.person("cb@example.test");
    const Person c = rig.person("cc@example.test");
    ASSERT_TRUE(rig.redeem(a, "GOOG1", "203.0.113.21").ok);
    ASSERT_TRUE(rig.redeem(b, "GOOG1", "203.0.113.22").ok);
    ASSERT_TRUE(rig.redeem(c, "GOOG2", "203.0.113.23").ok);
    // a goes on to pay and use; b uses part of the credit.
    lake::control::BalanceCredit paid;
    paid.customer_id = a.customer_id;
    paid.amount_micros = 20 * dollar;
    paid.provider = "stripe";
    paid.reference = "cs_campaign";
    ASSERT_TRUE(rig.store->apply_balance_credit(paid));
    const std::string month = utc_in(std::chrono::seconds{0}).substr(0, 7);
    ASSERT_TRUE(rig.store->set_balance_charge({a.customer_id, month, 30 * dollar, "", "", ""}));
    ASSERT_TRUE(rig.store->set_balance_charge({b.customer_id, month, 4 * dollar, "", "", ""}));

    const auto listed = rig.admin("list_coupons", json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["coupons"].size(), 3U);
    const json goog1 = [&] {
        for (const json& row : listed.value["coupons"]) {
            if (row["code"] == "GOOG1") {
                return row;
            }
        }
        return json();
    }();
    EXPECT_EQ(goog1["redemptions"], 2);
    EXPECT_EQ(goog1["redeemed"], 2);
    EXPECT_EQ(goog1["topped_up"], 1);
    EXPECT_EQ(goog1["spend_micros"], 34 * dollar);
    EXPECT_EQ(goog1["paid_micros"], 20 * dollar);
    json google;
    for (const json& row : listed.value["campaigns"]) {
        if (row["campaign"] == "google-2026-11") {
            google = row;
        }
    }
    EXPECT_EQ(google["coupons"], 2);
    EXPECT_EQ(google["redeemed"], 3);
    EXPECT_EQ(google["topped_up"], 1);
    EXPECT_EQ(google["spend_micros"], 34 * dollar);
    // One campaign's only.
    const auto bing = rig.admin("list_coupons", {{"campaign", "bing-2026-11"}});
    ASSERT_TRUE(bing.ok);
    ASSERT_EQ(bing.value["coupons"].size(), 1U);
    EXPECT_EQ(bing.value["coupons"][0]["redeemed"], 0);
}

namespace {

// The store's side, on either store: create, the code's uniqueness, the claim's checks under the
// coupon's lock, finishing, the attempts and the report.
void coupon_contract(lake::control::Store& store, const std::string& suffix) {
    const auto user = store.create_user(
        {"taap:1:ed25519:coupon-" + suffix, "coupon-" + suffix + "@example.com", "Coupon"});
    ASSERT_TRUE(user) << user.error().message;
    const auto older = store.create_customer({"individual", user->id, "", "coupon-" + suffix});
    ASSERT_TRUE(older) << older.error().message;

    lake::control::Coupon coupon;
    coupon.code = "CONTRACT" + suffix.substr(0, 4);
    std::ranges::transform(coupon.code, coupon.code.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    coupon.amount_micros = 50 * dollar;
    coupon.max_redemptions = 2;
    coupon.campaign = "contract";
    coupon.created_by = "operator";
    const auto created = store.create_coupon(coupon);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->redemptions, 0);
    EXPECT_EQ(created->max_redemptions, 2);
    EXPECT_EQ(created->credit_days, 90);
    EXPECT_FALSE(created->created_at.empty());
    EXPECT_EQ(store.create_coupon(coupon).error().code, "conflict");
    lake::control::Coupon lower = coupon;
    lower.code = "lowercase";
    EXPECT_EQ(store.create_coupon(lower).error().code, "invalid_input");
    EXPECT_EQ(store.coupon_by_code(coupon.code)->id, created->id);
    EXPECT_EQ(store.get_coupon(created->id)->code, coupon.code);
    EXPECT_EQ(store.coupon_by_code("NOSUCH" + coupon.code).error().code, "not_found");

    const auto person = [&](const std::string& name) {
        const auto u = store.create_user(
            {"taap:1:ed25519:" + name + suffix, name + suffix + "@example.com", name});
        EXPECT_TRUE(u) << u.error().message;
        const auto c = store.create_customer({"individual", u->id, "", name + suffix});
        EXPECT_TRUE(c) << c.error().message;
        return *c;
    };
    const auto claim_for = [&](const lake::control::Customer& customer, std::string email) {
        return store.claim_coupon({.coupon_id = created->id,
                                   .customer_id = customer.id,
                                   .email_sha256 = lake::control::coupon_email_digest(email),
                                   .redeemed_by = "taap:1:ed25519:x"});
    };
    // Older than the coupon.
    EXPECT_EQ(claim_for(*older, "older@example.com").error().message, "not_new_account");
    const auto first = person("first");
    const auto claimed = claim_for(first, "first@example.com");
    ASSERT_TRUE(claimed) << claimed.error().message;
    EXPECT_FALSE(claimed->resumed);
    EXPECT_EQ(claimed->coupon.redemptions, 1);
    // Unfinished: the same account and email resume it.
    const auto resumed = claim_for(first, "first@example.com");
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_TRUE(resumed->resumed);
    EXPECT_EQ(resumed->redemption.id, claimed->redemption.id);
    // Finished, it is redeemed.
    lake::control::BalanceCredit grant;
    grant.customer_id = first.id;
    grant.kind = "grant";
    grant.amount_micros = 50 * dollar;
    grant.provider = "orama";
    grant.reference = "coupon:" + created->id + ":" + first.id;
    grant.reason = "Coupon code";
    const auto given = store.apply_balance_credit(grant);
    ASSERT_TRUE(given) << given.error().message;
    ASSERT_TRUE(store.finish_coupon_redemption(claimed->redemption.id, given->credit.id));
    EXPECT_EQ(store.coupon_redemption(created->id, first.id)->credit_id, given->credit.id);
    EXPECT_EQ(claim_for(first, "first@example.com").error().message, "already_redeemed");
    // The same inbox on another account.
    const auto second = person("second");
    EXPECT_EQ(claim_for(second, "First+x@Example.com").error().message, "email_redeemed");
    // A used account.
    lake::control::BalanceCredit top_up;
    top_up.customer_id = second.id;
    top_up.amount_micros = 20 * dollar;
    top_up.provider = "stripe";
    top_up.reference = "cs_" + suffix;
    ASSERT_TRUE(store.apply_balance_credit(top_up));
    EXPECT_EQ(claim_for(second, "second@example.com").error().message, "account_used");
    // The cap.
    const auto third = person("third");
    ASSERT_TRUE(claim_for(third, "third@example.com"));
    const auto fourth = person("fourth");
    EXPECT_EQ(claim_for(fourth, "fourth@example.com").error().message, "used_up");
    EXPECT_EQ(store.get_coupon(created->id)->redemptions, 2);
    // Disabled, then expired.
    const auto disabled = store.disable_coupon(created->id, "operator");
    ASSERT_TRUE(disabled);
    EXPECT_FALSE(disabled->disabled_at.empty());
    EXPECT_EQ(disabled->disabled_by, "operator");
    EXPECT_EQ(store.disable_coupon(created->id, "someone else")->disabled_by, "operator");
    lake::control::Coupon past = coupon;
    past.code = coupon.code + "X";
    past.max_redemptions.reset();
    past.new_accounts_only = false;
    past.expires_at = utc_in(-std::chrono::minutes{5});
    const auto expired = store.create_coupon(past);
    ASSERT_TRUE(expired) << expired.error().message;
    EXPECT_EQ(expired->expires_at, past.expires_at);
    EXPECT_EQ(store
                  .claim_coupon({.coupon_id = expired->id,
                                 .customer_id = fourth.id,
                                 .email_sha256 = lake::control::coupon_email_digest("f@x.com"),
                                 .redeemed_by = "x"})
                  .error()
                  .message,
              "expired");

    // The report.
    const auto reports = store.coupon_reports("contract");
    ASSERT_TRUE(reports) << reports.error().message;
    const auto mine = std::ranges::find_if(
        *reports, [&](const lake::control::CouponReport& r) { return r.coupon.id == created->id; });
    ASSERT_NE(mine, reports->end());
    EXPECT_EQ(mine->redeemed, 1);
    EXPECT_EQ(mine->coupon.redemptions, 2);
    EXPECT_EQ(mine->topped_up, 0);
    ASSERT_TRUE(store.coupon_reports("no-such-campaign-" + suffix));
    EXPECT_TRUE(store.coupon_reports("no-such-campaign-" + suffix)->empty());

    // Attempts: exhausted at the limit, per key.
    const std::string key = lake::control::coupon_email_digest("attempt-" + suffix + "@x.com");
    const std::string other = lake::control::coupon_email_digest("other-" + suffix + "@x.com");
    EXPECT_FALSE(*store.coupon_attempts_exhausted({key}, 3, 3600));
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(store.coupon_attempt_failed({key}, 3600));
    }
    EXPECT_TRUE(*store.coupon_attempts_exhausted({key}, 3, 3600));
    EXPECT_TRUE(*store.coupon_attempts_exhausted({other, key}, 3, 3600));
    EXPECT_FALSE(*store.coupon_attempts_exhausted({other}, 3, 3600));
    EXPECT_FALSE(*store.coupon_attempts_exhausted({key}, 4, 3600));
}

} // namespace

TEST(CouponStore, MemoryFollowsTheContract) {
    lake::control::InMemoryStore store;
    coupon_contract(store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(CouponStore, PostgresFollowsTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(kek));
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        EXPECT_FALSE(tx.exec("SELECT to_regclass('coupons_guard')")[0][0].is_null());
        EXPECT_FALSE(
            tx.exec("SELECT 1 FROM schema_migrations WHERE version = '0101_coupons'").empty());
    }
    coupon_contract(*store, lake::control::new_uuid_v4().substr(0, 8));

    // The table's own checks hold for a writer other than lake's store.
    pqxx::connection connection(uri);
    for (const char* statement :
         {"INSERT INTO billing_coupons (code, amount_micros, credit_days, created_by) VALUES "
          "('lower', 10000, 90, 'x')",
          "INSERT INTO billing_coupons (code, amount_micros, credit_days, created_by) VALUES "
          "('ODDCENTS', 10001, 90, 'x')",
          "INSERT INTO billing_coupons (code, amount_micros, credit_days, created_by, campaign) "
          "VALUES ('BADCAMPAIGN', 10000, 90, 'x', 'Upper')",
          "INSERT INTO billing_coupons (code, amount_micros, credit_days, created_by, "
          "max_redemptions, redemptions) VALUES ('OVERCAP', 10000, 90, 'x', 1, 2)",
          "INSERT INTO billing_coupon_attempts (key_sha256, failures) VALUES ('nothex', 1)"}) {
        pqxx::work tx(connection);
        EXPECT_THROW(tx.exec(statement), pqxx::check_violation) << statement;
    }
}
