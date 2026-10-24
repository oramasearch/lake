// The prepaid account balance (lake#1114, liquid-bridge decision 67, docs/api/billing.md#balance):
// the balance's view and its math, top-ups credited once per payment from Stripe's webhook, the
// charges refreshed from usage (a month's charge is its bill, a failed read holds the balance, an
// issued invoice freezes its month), the 402 gate on the capabilities that start paid work and
// the one list that marks them, the warning on every answer, the emails once per crossing, the
// first top-up before any paid work, and auto top-up (off by default, one charge per crossing, a
// decline turning it off).

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "audit_test_support.hpp"
#include "lake/api/balance.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/stripe.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/memory_store.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::UsageRecorder;
using nlohmann::json;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";
constexpr std::string_view webhook_secret = "whsec_lake1114fakesecret01";
constexpr std::string_view this_lake = "balance.lake.test";
constexpr std::int64_t dollar = 1'000'000;

std::string month_name(std::chrono::sys_days day) {
    return std::format("{:%Y-%m}", day);
}

std::chrono::year_month this_month() {
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return today.year() / today.month();
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A card provider as far as the balance uses it: a customer, the balance's checkouts and the
// off-session charge, with a switch that declines.
struct FakeCards final : lake::api::PaymentProvider {
    int checkouts = 0;
    std::vector<lake::api::BalanceCheckoutRequest> asked;
    std::vector<lake::api::OffSessionCharge> charges;
    std::set<std::string> keys; // idempotency keys seen
    bool decline = false;
    std::string_view name() const override {
        return "stripe";
    }
    std::string_view title() const override {
        return "Stripe";
    }
    bool records_payments_by_hand() const override {
        return false;
    }
    std::expected<lake::api::ProviderCustomer, std::string>
    provider_customer(const lake::api::PaymentAccount& account) override {
        return lake::api::ProviderCustomer{"cus_" + account.customer_id.substr(0, 8), {}};
    }
    std::expected<lake::api::PaymentRequest, std::string>
    request_payment(const lake::api::PaymentAccount&, const lake::control::Invoice&) override {
        return lake::api::PaymentRequest{"in_fake", "Pay on Stripe.", "https://pay.test", "open"};
    }
    std::expected<lake::api::CheckoutSession, std::string>
    create_balance_checkout(const lake::api::PaymentAccount&,
                            const lake::api::BalanceCheckoutRequest& request) override {
        ++checkouts;
        asked.push_back(request);
        return lake::api::CheckoutSession{"cs_fake_" + std::to_string(checkouts),
                                          "https://checkout.stripe.test/" +
                                              std::to_string(checkouts)};
    }
    std::expected<lake::api::OffSessionResult, std::string>
    charge_saved_payment_method(const lake::api::PaymentAccount&,
                                const lake::api::OffSessionCharge& charge) override {
        charges.push_back(charge);
        keys.insert(charge.idempotency_key);
        if (decline) {
            return std::unexpected(
                std::string("Stripe refused to charge the saved card (HTTP 402): Your card was "
                            "declined."));
        }
        return lake::api::OffSessionResult{"pi_fake_" + std::to_string(charges.size()),
                                           "succeeded"};
    }
};

struct SentEmail {
    lake::api::Email email;
    std::string key;
};

// A registry with billing on over two ready workspaces of one account, the usage recorder, a
// published price list ($1 per 1,000 calls) and the balance's runtime with a fake mailer.
struct BalanceRig {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::string user_id;
    std::string customer;
    std::string home;
    UsageRecorder recorder{store.get(), workspaces.service.get()};
    lake::api::Registry registry;
    std::shared_ptr<FakeCards> cards = std::make_shared<FakeCards>();
    std::shared_ptr<lake::api::PaymentProviders> providers =
        lake::api::fixed_payment_providers(cards);
    std::vector<SentEmail> sent;
    bool mail_fails = false;
    std::shared_ptr<lake::api::BalanceRuntime> runtime =
        std::make_shared<lake::api::BalanceRuntime>();

    BalanceRig() {
        const auto user =
            store->create_user({"taap:1:ed25519:balance-owner", "owner@example.test", "Owner"});
        EXPECT_TRUE(user) << user.error().message;
        user_id = user->id;
        const auto created = store->create_customer({"individual", user->id, "", "balance"});
        EXPECT_TRUE(created) << created.error().message;
        customer = created->id;
        const auto object = store->find_policy_object("customer", customer);
        EXPECT_TRUE(object) << object.error().message;
        EXPECT_TRUE(store->create_grant({"taap:1:ed25519:balance-owner",
                                         object->id,
                                         "admin",
                                         "attestation-owner",
                                         "admin",
                                         "request-owner"}));
        home = workspaces.create(customer, "home");
        lake::api::register_core_capabilities(registry, "test");
        lake::api::register_billing_capabilities(registry, providers);
        runtime->origin = "https://lake.test";
        runtime->mailer = [this](const lake::api::Email& email,
                                 std::string_view key) -> std::expected<std::string, std::string> {
            if (mail_fails) {
                return std::unexpected(std::string("Resend refused"));
            }
            sent.push_back({email, std::string(key)});
            return "email-" + std::to_string(sent.size());
        };
        auto op = operator_context();
        const auto published = call(
            "set_price_list",
            {{"effective_from",
              month_name(std::chrono::sys_days{(this_month() - std::chrono::months{2}) / 1})},
             {"prices",
              json::array({{{"dimension", "calls"}, {"unit_micros", 1'000'000}, {"per", 1000}}})}},
            op);
        EXPECT_TRUE(published.ok) << published.error.message;
    }

    lake::api::ApiContext operator_context() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.usage_recorder = &recorder;
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        context.payments = providers;
        context.balance = runtime;
        context.registry = &registry;
        return context;
    }

    lake::api::ApiContext owner_context() {
        auto context = operator_context();
        context.operator_credential = false;
        context.acting_identity = "taap:1:ed25519:balance-owner";
        context.acting_user_id = user_id;
        context.session_credential = true;
        context.scopes = {"*"};
        return context;
    }

    lake::api::ApiResult call(const std::string& name, json input, lake::api::ApiContext& context) {
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }

    lake::api::ApiResult owner(const std::string& name, json input) {
        auto context = owner_context();
        return call(name, std::move(input), context);
    }

    void credit(std::int64_t micros, const std::string& reference, std::string kind = "top_up") {
        lake::control::BalanceCredit credit;
        credit.customer_id = customer;
        credit.kind = std::move(kind);
        credit.amount_micros = micros;
        credit.provider = "stripe";
        credit.reference = reference;
        const auto applied = store->apply_balance_credit(credit);
        ASSERT_TRUE(applied) << applied.error().message;
    }

    // `calls` API calls in the home workspace now: $1 per 1,000.
    void use(std::uint64_t calls) {
        recorder.add(home, "calls", calls, {{"kind", "api"}}, std::chrono::system_clock::now());
        static_cast<void>(liquid::runtime::block_on(recorder.flush()));
    }

    lake::api::BalanceRefreshReport refresh() {
        auto context = operator_context();
        auto report = liquid::runtime::block_on(
            lake::api::refresh_account_balances(context, providers, *runtime));
        EXPECT_TRUE(report);
        return report ? *report : lake::api::BalanceRefreshReport{};
    }

    lake::control::AccountBalance balance() {
        const auto read = store->account_balance(customer);
        EXPECT_TRUE(read) << read.error().message;
        return read ? *read : lake::control::AccountBalance{};
    }
};

// A signed Stripe event, as the webhook route reads it.
struct StripeEvents {
    std::vector<lake::api::StripeRequest> requests;
    std::shared_ptr<lake::api::PaymentProvider> provider = lake::api::stripe_payment_provider(
        {"sk_test_lake1114fakekey001", std::string(webhook_secret)},
        std::string(this_lake),
        [this](const lake::api::StripeRequest& request) {
            requests.push_back(request);
            return lake::api::StripeResponse{500, "{}", {}};
        });
    std::shared_ptr<lake::api::PaymentProviders> providers =
        lake::api::fixed_payment_providers(provider);

    lake::api::NoticeOutcome deliver(lake::api::ApiContext& context, const json& event) {
        const std::string body = event.dump();
        auto outcome = liquid::runtime::block_on(lake::api::receive_payment_notice(
            *providers,
            context,
            body,
            lake::api::stripe_signature_header(body, webhook_secret, unix_now())));
        EXPECT_TRUE(outcome);
        return outcome ? *outcome : lake::api::NoticeOutcome{};
    }
};

json event(const std::string& id, const std::string& type, json object) {
    return {{"id", id}, {"type", type}, {"created", unix_now()}, {"data", {{"object", object}}}};
}

json top_up_session(const std::string& customer,
                    const std::string& session,
                    std::int64_t cents,
                    const std::string& payment_status = "paid") {
    return {{"id", session},
            {"object", "checkout.session"},
            {"amount_total", cents},
            {"payment_status", payment_status},
            {"client_reference_id", customer},
            {"metadata",
             {{"lake_customer_id", customer},
              {"lake_balance", "top_up"},
              {"orama_app", "lake"},
              {"lake_instance", this_lake}}}};
}

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

} // namespace

TEST(Balance, TheViewFollowsTheBalance) {
    const lake::api::BalanceSettings settings;
    lake::control::AccountBalance balance;
    balance.customer_id = "account";

    // A new account starts at $0 and must add the first top-up before anything that costs money.
    auto view = lake::api::balance_view(balance, settings);
    EXPECT_EQ(view.state, "blocked");
    EXPECT_EQ(view.reason, "first_top_up");
    EXPECT_EQ(view.add_micros, 20 * dollar);

    // Balance = credits - charges.
    balance.first_top_up_at = "2026-10-24T00:00:00Z";
    balance.credits_micros = 20 * dollar;
    balance.charges_micros = 3 * dollar;
    view = lake::api::balance_view(balance, settings);
    EXPECT_EQ(view.balance_micros, 17 * dollar);
    EXPECT_EQ(view.state, "ok");
    EXPECT_EQ(view.overdraft_micros, 50 * dollar);

    balance.charges_micros = 12'500'000; // $7.50 left, under the $10 warning amount
    view = lake::api::balance_view(balance, settings);
    EXPECT_EQ(view.state, "low");
    EXPECT_EQ(view.add_micros, 20 * dollar) << "at least the minimum top-up";

    balance.charges_micros = 25 * dollar; // -$5
    view = lake::api::balance_view(balance, settings);
    EXPECT_EQ(view.state, "negative");
    EXPECT_EQ(view.add_micros, 20 * dollar);

    balance.charges_micros = 70 * dollar; // -$50: at the overdraft limit
    view = lake::api::balance_view(balance, settings);
    EXPECT_EQ(view.state, "blocked");
    EXPECT_EQ(view.reason, "overdraft");
    EXPECT_EQ(view.add_micros, 60 * dollar) << "what brings it back over the warning amount";

    balance.charges_micros = 69'990'000; // a cent above the limit
    EXPECT_EQ(lake::api::balance_view(balance, settings).state, "negative");

    // The operator's overdraft for this account replaces the instance's.
    balance.overdraft_limit_micros = 100 * dollar;
    EXPECT_EQ(lake::api::balance_view(balance, settings).state, "negative");
    balance.overdraft_limit_micros = 0;
    balance.charges_micros = 20 * dollar;
    EXPECT_EQ(lake::api::balance_view(balance, settings).state, "blocked")
        << "no overdraft: $0 is the limit";

    // An account a paid order covers is never nagged or stopped.
    view = lake::api::balance_view(balance, settings, "paid_order");
    EXPECT_EQ(view.state, "ok");
    EXPECT_EQ(view.exempt, "paid_order");

    // The settings come from the environment, in whole dollars.
    const auto defaults = lake::api::balance_settings_from_environment("", "", "", "", "", "");
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->first_top_up_micros, 20 * dollar);
    EXPECT_EQ(defaults->minimum_top_up_micros, 20 * dollar);
    EXPECT_EQ(defaults->overdraft_micros, 50 * dollar);
    EXPECT_EQ(defaults->refresh_interval, std::chrono::seconds{300});
    const auto set = lake::api::balance_settings_from_environment("25", "30", "0", "5", "60", "2");
    ASSERT_TRUE(set);
    EXPECT_EQ(set->first_top_up_micros, 25 * dollar);
    EXPECT_EQ(set->minimum_top_up_micros, 30 * dollar);
    EXPECT_EQ(set->overdraft_micros, 0);
    EXPECT_EQ(set->auto_top_up_amount_micros, 30 * dollar) << "never under the minimum";
    const auto bad = lake::api::balance_settings_from_environment("20.5", "", "", "", "", "");
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().find("LAKE_BALANCE_FIRST_TOP_UP_USD"), std::string::npos);
    EXPECT_FALSE(lake::api::balance_settings_from_environment("", "", "", "", "5", ""))
        << "a refresh under 30 seconds";

    EXPECT_EQ(lake::api::add_funds_link("https://lake.live", "abc"),
              "https://lake.live/billing/add-funds?customer_id=abc");
}

TEST(Balance, ATopUpIsCreditedOncePerPayment) {
    BalanceRig rig;
    StripeEvents stripe;
    auto context = rig.operator_context();
    context.public_host = std::string(this_lake);

    const json paid = event("evt_top_up_1",
                            "checkout.session.completed",
                            top_up_session(rig.customer, "cs_top_up_1", 2000));
    const auto first = stripe.deliver(context, paid);
    EXPECT_EQ(first.status, 200) << first.body;
    EXPECT_EQ(first.body["credited"], true) << first.body;
    EXPECT_EQ(first.body["balance_micros"], 20 * dollar);
    // Stripe's retry of the same event, and the delayed-payment event of the same session.
    const auto again = stripe.deliver(context, paid);
    EXPECT_EQ(again.body["duplicate"], true) << again.body;
    const auto async = stripe.deliver(context,
                                      event("evt_top_up_1_async",
                                            "checkout.session.async_payment_succeeded",
                                            top_up_session(rig.customer, "cs_top_up_1", 2000)));
    EXPECT_EQ(async.body["duplicate"], true) << async.body;

    auto balance = rig.balance();
    EXPECT_EQ(balance.credits_micros, 20 * dollar) << "credited once";
    EXPECT_EQ(balance.credit_count, 1);
    EXPECT_FALSE(balance.first_top_up_at.empty());
    EXPECT_EQ(audit_of(*rig.store, "billing.balance.credit").size(), 1U);

    // A session whose payment is not in yet credits nothing; another top-up does.
    const auto pending =
        stripe.deliver(context,
                       event("evt_top_up_2",
                             "checkout.session.completed",
                             top_up_session(rig.customer, "cs_top_up_2", 5000, "unpaid")));
    EXPECT_EQ(pending.body["ignored"], true) << pending.body;
    const auto later = stripe.deliver(context,
                                      event("evt_top_up_2_async",
                                            "checkout.session.async_payment_succeeded",
                                            top_up_session(rig.customer, "cs_top_up_2", 5000)));
    EXPECT_EQ(later.body["credited"], true) << later.body;
    EXPECT_EQ(rig.balance().balance_micros(), 70 * dollar);

    // Another Lake's top-up in the shared account is acknowledged with nothing recorded.
    json foreign = top_up_session(rig.customer, "cs_other", 9000);
    foreign["metadata"]["lake_instance"] = "other.lake.test";
    const auto other =
        stripe.deliver(context, event("evt_other", "checkout.session.completed", foreign));
    EXPECT_EQ(other.body["ignored"], true) << other.body;
    EXPECT_EQ(rig.balance().balance_micros(), 70 * dollar);

    // The history lists the credits, newest first.
    const auto read = rig.owner("account_balance", {{"customer_id", rig.customer}});
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_EQ(read.value["credits"].size(), 2U);
    EXPECT_EQ(read.value["credits"][0]["reference"], "cs_top_up_2");
    EXPECT_EQ(read.value["state"], "ok");
    EXPECT_EQ(read.value["add_funds_url"],
              "https://lake.test/billing/add-funds?customer_id=" + rig.customer);
}

TEST(Balance, TheFirstTopUpIsTheFirstAmountAndLaterOnesAtLeastTheMinimum) {
    BalanceRig rig;
    // The first is exactly $20.
    auto refused =
        rig.owner("start_top_up", {{"customer_id", rig.customer}, {"amount_micros", 50 * dollar}});
    EXPECT_EQ(refused.error.code, "invalid_input");
    auto started = rig.owner("start_top_up", {{"customer_id", rig.customer}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["amount_micros"], 20 * dollar);
    EXPECT_TRUE(started.value["checkout_url"].get<std::string>().starts_with("https://"));
    ASSERT_EQ(rig.cards->asked.size(), 1U);
    EXPECT_EQ(rig.cards->asked[0].purpose, "top_up");
    EXPECT_EQ(rig.cards->asked[0].amount_cents, 2000);
    EXPECT_NE(
        rig.cards->asked[0].success_url.find("/billing/add-funds?customer_id=" + rig.customer),
        std::string::npos);
    EXPECT_NE(rig.cards->asked[0].cancel_url.find("top_up=cancelled"), std::string::npos);
    // Nothing is credited by starting: only Stripe's notice credits.
    EXPECT_EQ(rig.balance().credits_micros, 0);

    rig.credit(20 * dollar, "cs_first");
    refused =
        rig.owner("start_top_up", {{"customer_id", rig.customer}, {"amount_micros", 19 * dollar}});
    EXPECT_EQ(refused.error.code, "invalid_input");
    refused =
        rig.owner("start_top_up", {{"customer_id", rig.customer}, {"amount_micros", 20'000'001}});
    EXPECT_EQ(refused.error.code, "invalid_input") << "whole cents";
    started =
        rig.owner("start_top_up", {{"customer_id", rig.customer}, {"amount_micros", 135 * dollar}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(rig.cards->asked.back().amount_cents, 13500);

    // The manual provider takes no card: refused, said plainly.
    lake::api::Registry manual;
    lake::api::register_core_capabilities(manual, "test");
    lake::api::register_billing_capabilities(manual);
    auto context = rig.owner_context();
    context.registry = &manual;
    auto completed = liquid::runtime::block_on(manual.call_async(
        "start_top_up", {{"customer_id", rig.customer}, {"amount_micros", 20 * dollar}}, context));
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->error.code, "conflict") << completed->error.message;
}

TEST(Balance, ChargesAreEachMonthsBillAndAFailedReadHoldsTheBalance) {
    BalanceRig rig;
    rig.credit(20 * dollar, "cs_first");
    rig.use(3500); // $3.50
    auto report = rig.refresh();
    EXPECT_EQ(report.refreshed, 1);
    auto balance = rig.balance();
    EXPECT_EQ(balance.charges_micros, 3'500'000);
    EXPECT_EQ(balance.balance_micros(), 16'500'000);
    EXPECT_TRUE(balance.refresh_error.empty());
    EXPECT_FALSE(balance.charges_refreshed_at.empty());

    // The month's charge is the month's bill.
    auto op = rig.operator_context();
    const auto preview = rig.call("bill_preview", {{"customer_id", rig.customer}}, op);
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_EQ(preview.value["total_micros"].get<std::int64_t>(), balance.charges_micros);

    // A workspace that does not answer holds the balance where it was, and says why.
    const std::string lab = rig.workspaces.create(rig.customer, "lab");
    rig.use(10'000);
    rig.workspaces.records(lab).metric_unavailable_next = 10;
    report = rig.refresh();
    EXPECT_EQ(report.held, 1);
    balance = rig.balance();
    EXPECT_EQ(balance.charges_micros, 3'500'000) << "held, never guessed";
    EXPECT_NE(balance.refresh_error.find("lab"), std::string::npos) << balance.refresh_error;
    EXPECT_FALSE(balance.refresh_failed_at.empty());

    rig.workspaces.records(lab).metric_unavailable_next = 0;
    report = rig.refresh();
    EXPECT_EQ(report.refreshed, 1);
    balance = rig.balance();
    EXPECT_EQ(balance.charges_micros, 13'500'000);
    EXPECT_TRUE(balance.refresh_error.empty());
}

TEST(Balance, AnInvoiceRecordsWhatTheBalancePaidAndFreezesItsMonth) {
    BalanceRig rig;
    rig.credit(20 * dollar, "cs_first");
    const std::string last =
        month_name(std::chrono::sys_days{(this_month() - std::chrono::months{1}) / 1});
    auto op = rig.operator_context();
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", last}}, op);
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["state"], "paid");
    EXPECT_EQ(issued.value["provider"], "balance");
    EXPECT_EQ(rig.cards->checkouts, 0);
    const auto charges = rig.store->balance_charges(rig.customer);
    ASSERT_TRUE(charges);
    const auto frozen = std::ranges::find(*charges, last, &lake::control::BalanceCharge::period);
    ASSERT_NE(frozen, charges->end());
    EXPECT_EQ(frozen->invoice_id, issued.value["id"].get<std::string>());
    EXPECT_EQ(frozen->amount_micros, issued.value["total_micros"].get<std::int64_t>());
    // A refresh never moves a month its invoice froze.
    ASSERT_TRUE(rig.store->set_balance_charge({rig.customer, last, 99 * dollar, "x", {}, {}}));
    const auto after = rig.store->balance_charges(rig.customer);
    ASSERT_TRUE(after);
    EXPECT_EQ(std::ranges::find(*after, last, &lake::control::BalanceCharge::period)->amount_micros,
              frozen->amount_micros);
}

TEST(Balance, TheGateRefusesPaidWorkAtTheLimitAndLeavesTheRestOpen) {
    BalanceRig rig;
    const json create{{"customer_id", rig.customer}, {"name", "more"}, {"environment", "dev"}};

    // Before the first top-up: refused with the balance, the limit and where to add funds.
    auto refused = rig.owner("create_workspace", create);
    ASSERT_EQ(refused.error.code, "payment_required") << refused.error.message;
    EXPECT_EQ(lake::api::find_error_code("payment_required")->status, 402);
    EXPECT_EQ(refused.error.details["reason"], "first_top_up");
    EXPECT_EQ(refused.error.details["balance_micros"], 0);
    EXPECT_EQ(refused.error.details["add_micros"], 20 * dollar);
    EXPECT_EQ(refused.error.details["add_funds_url"],
              "https://lake.test/billing/add-funds?customer_id=" + rig.customer);
    EXPECT_NE(refused.envelope()["error"].dump().find("add_funds_url"), std::string::npos);
    // Reading stays open.
    auto read = rig.owner("list_workspaces", {{"customer_id", rig.customer}});
    EXPECT_TRUE(read.ok) << read.error.message;
    // The model gateway is gated too, as are every other listed capability (PaidWork*).
    auto completion =
        rig.owner("model_completion",
                  {{"endpoint", "chat/completions"}, {"request", {{"model", "gpt-5.5"}}}});
    EXPECT_EQ(completion.error.code, "payment_required") << completion.error.message;

    // After the first top-up the gate lets it through (whatever the handler answers).
    rig.credit(20 * dollar, "cs_first");
    auto allowed = rig.owner("create_workspace", create);
    EXPECT_NE(allowed.error.code, "payment_required") << allowed.error.message;

    // Past the $50 overdraft: refused again, and stopping and deleting stay open.
    ASSERT_TRUE(rig.store->set_balance_charge({rig.customer,
                                               month_name(std::chrono::sys_days{this_month() / 1}),
                                               75 * dollar,
                                               "",
                                               {},
                                               {}}));
    refused = rig.owner("create_workspace", create);
    ASSERT_EQ(refused.error.code, "payment_required");
    EXPECT_EQ(refused.error.details["reason"], "overdraft");
    EXPECT_EQ(refused.error.details["balance_micros"], -55 * dollar);
    EXPECT_EQ(refused.error.details["overdraft_micros"], 50 * dollar);
    EXPECT_NE(refused.error.message.find("Reading, stopping and deleting still work"),
              std::string::npos);
    auto deleted = rig.owner("delete_workspace", {{"workspace_id", rig.home}, {"dry_run", true}});
    EXPECT_NE(deleted.error.code, "payment_required") << deleted.error.message;
    auto stopped = rig.owner("stop_app", {{"workspace_id", rig.home}, {"app_id", "none"}});
    EXPECT_NE(stopped.error.code, "payment_required");
    // Refusals are audited.
    const auto events = lake::test::audit_events(*rig.store);
    ASSERT_TRUE(events);
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.action == "create_workspace" &&
               event.draft.details.value("error_code", "") == "payment_required";
    }));

    // The operator and the instance administrator are exempt.
    auto op = rig.operator_context();
    auto as_operator = rig.call("create_workspace", create, op);
    EXPECT_NE(as_operator.error.code, "payment_required");

    // The operator raises this account's overdraft: the same balance is allowed.
    auto raised = rig.call("set_balance_overdraft",
                           {{"customer_id", rig.customer}, {"overdraft_micros", 100 * dollar}},
                           op);
    ASSERT_TRUE(raised.ok) << raised.error.message;
    EXPECT_EQ(raised.value["state"], "negative");
    allowed = rig.owner("create_workspace", create);
    EXPECT_NE(allowed.error.code, "payment_required");
    auto owner_refused = rig.owner("set_balance_overdraft",
                                   {{"customer_id", rig.customer}, {"overdraft_micros", 0}});
    EXPECT_EQ(owner_refused.error.code, "unauthorized") << "the operator's alone";

    // With billing off (no balance runtime) nothing is gated.
    auto off = rig.owner_context();
    off.balance = nullptr;
    ASSERT_TRUE(rig.store->set_balance_charge({rig.customer,
                                               month_name(std::chrono::sys_days{this_month() / 1}),
                                               500 * dollar,
                                               "",
                                               {},
                                               {}}));
    auto ungated = rig.call("create_workspace", create, off);
    EXPECT_NE(ungated.error.code, "payment_required");
}

TEST(Balance, APaidOrderCoversTheAccount) {
    BalanceRig rig;
    auto person = rig.owner_context();
    const auto organization =
        rig.store->create_organization({"taap:1:ed25519:balance-covered-org", "Covered"});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto space =
        rig.store->create_customer({"organization", "", organization->id, "covered-org"});
    ASSERT_TRUE(space) << space.error().message;
    const auto object = rig.store->find_policy_object("customer", space->id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(rig.store->create_grant({"taap:1:ed25519:balance-owner",
                                         object->id,
                                         "admin",
                                         "attestation-org",
                                         "admin",
                                         "request-org"}));
    lake::control::NewCloudOrder order;
    order.offer = "private_warehouse";
    order.path = "order_form";
    order.contact_name = "Ops";
    order.contact_email = "ops@example.test";
    order.amount_cents = 100'000;
    order.organization_id = organization->id;
    const auto placed = rig.store->create_cloud_order(order);
    ASSERT_TRUE(placed) << placed.error().message;
    EXPECT_FALSE(rig.store->account_balance(space->id)->paid_order) << "not paid yet";
    ASSERT_TRUE(rig.store->transition_cloud_order(
        {placed->id, "submitted", "paid", "operator", "paid by wire"}));
    const auto covered = rig.store->account_balance(space->id);
    ASSERT_TRUE(covered);
    EXPECT_TRUE(covered->paid_order);
    const json create{{"customer_id", space->id}, {"name", "warehouse"}, {"environment", "dev"}};
    auto allowed = rig.call("create_workspace", create, person);
    EXPECT_NE(allowed.error.code, "payment_required") << allowed.error.message;
}

TEST(Balance, AnswersCarryAWarningWhileLowNegativeOrBlocked) {
    BalanceRig rig;
    // A new account is told to add its first top-up, on reads too.
    auto read = rig.owner("list_workspaces", {{"customer_id", rig.customer}});
    ASSERT_TRUE(read.ok);
    ASSERT_TRUE(read.warnings.is_array());
    EXPECT_EQ(read.warnings[0]["code"], "balance_blocked");
    EXPECT_EQ(read.envelope()["warnings"][0]["add_micros"], 20 * dollar);

    rig.credit(40 * dollar, "cs_first");
    read = rig.owner("list_workspaces", {{"customer_id", rig.customer}});
    EXPECT_TRUE(read.warnings.is_null()) << read.warnings.dump();
    EXPECT_FALSE(read.envelope().contains("warnings"));

    const std::string month = month_name(std::chrono::sys_days{this_month() / 1});
    ASSERT_TRUE(rig.store->set_balance_charge({rig.customer, month, 35 * dollar, "", {}, {}}));
    read = rig.owner("list_workspaces", {{"customer_id", rig.customer}});
    ASSERT_TRUE(read.warnings.is_array());
    EXPECT_EQ(read.warnings[0]["code"], "balance_low");
    EXPECT_NE(read.warnings[0]["message"].get<std::string>().find("$5.00"), std::string::npos);

    ASSERT_TRUE(rig.store->set_balance_charge({rig.customer, month, 45 * dollar, "", {}, {}}));
    read = rig.owner("list_workspaces", {{"customer_id", rig.customer}});
    ASSERT_TRUE(read.warnings.is_array());
    EXPECT_EQ(read.warnings[0]["code"], "balance_negative");
    EXPECT_EQ(read.warnings[0]["balance_micros"], -5 * dollar);
    EXPECT_EQ(read.warnings[0]["add_funds_url"],
              "https://lake.test/billing/add-funds?customer_id=" + rig.customer);

    // Only the outermost answer: bill_preview reads usage_over_time through the registry.
    auto preview = rig.owner("bill_preview", {{"customer_id", rig.customer}});
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_EQ(preview.warnings.size(), 1U);

    // The operator is never warned; viewer carries the balance for the banner.
    auto op = rig.operator_context();
    read = rig.call("list_workspaces", {{"customer_id", rig.customer}}, op);
    EXPECT_TRUE(read.warnings.is_null());
    auto viewer = rig.owner("viewer", {{"customer_id", rig.customer}});
    ASSERT_TRUE(viewer.ok) << viewer.error.message;
    ASSERT_TRUE(viewer.value["balance"].is_object()) << viewer.value.dump();
    EXPECT_EQ(viewer.value["balance"]["state"], "negative");
    EXPECT_EQ(viewer.value["balance"]["limit_micros"], -50 * dollar);
    viewer = rig.call("viewer", json::object(), op);
    EXPECT_TRUE(viewer.value["balance"].is_null());
}

TEST(Balance, EmailsGoOncePerCrossingAndResetWithACredit) {
    BalanceRig rig;
    // A new account that never topped up is not emailed.
    rig.refresh();
    EXPECT_TRUE(rig.sent.empty());

    rig.credit(20 * dollar, "cs_first");
    rig.use(25'000); // $25: -$5, past $0
    rig.refresh();
    ASSERT_EQ(rig.sent.size(), 1U);
    EXPECT_EQ(rig.sent[0].email.to, std::vector<std::string>{"owner@example.test"})
        << "the owner's email while there is no billing contact";
    EXPECT_EQ(rig.sent[0].email.subject, "Your Lake balance is below $0");
    EXPECT_NE(rig.sent[0].email.text.find("-$5.00"), std::string::npos);
    EXPECT_NE(rig.sent[0].email.text.find("Add $20.00 or more"), std::string::npos);
    EXPECT_NE(rig.sent[0].email.text.find("https://lake.test/billing/add-funds?customer_id=" +
                                          rig.customer),
              std::string::npos);
    EXPECT_EQ(rig.sent[0].email.text.find("balance\n"), std::string::npos);
    EXPECT_EQ(rig.sent[0].email.text.find("Owner"), std::string::npos) << "no name in the email";
    EXPECT_EQ(rig.sent[0].key, "lake-balance-" + rig.customer + "-negative-1");
    // Every refresh after it sends nothing more.
    rig.refresh();
    rig.refresh();
    EXPECT_EQ(rig.sent.size(), 1U);

    // Within $10 of the limit: its own email, once, to the billing contact once there is one.
    ASSERT_TRUE(rig.store->set_billing_contact({rig.customer, "billing@example.test"}));
    rig.use(20'000); // $45: -$25
    rig.refresh();
    EXPECT_EQ(rig.sent.size(), 1U) << "-$25 is not within $10 of -$50";
    rig.use(17'000); // $62: -$42
    rig.refresh();
    rig.refresh();
    ASSERT_EQ(rig.sent.size(), 2U);
    EXPECT_EQ(rig.sent[1].email.to, std::vector<std::string>{"billing@example.test"});
    EXPECT_EQ(rig.sent[1].email.subject, "Your Lake balance is close to its limit");

    // At the limit: blocked, once.
    rig.use(10'000); // $72: -$52
    rig.refresh();
    rig.refresh();
    ASSERT_EQ(rig.sent.size(), 3U);
    EXPECT_EQ(rig.sent[2].email.subject, "Lake has stopped starting paid work on your account");
    EXPECT_NE(rig.sent[2].email.text.find("Add $62.00 or more"), std::string::npos)
        << rig.sent[2].email.text;

    // Adding funds resets the marks: crossing again emails again, under a new key.
    rig.credit(80 * dollar, "cs_second"); // +$28
    rig.refresh();
    EXPECT_EQ(rig.sent.size(), 3U);
    rig.use(30'000); // -$2
    rig.refresh();
    ASSERT_EQ(rig.sent.size(), 4U);
    EXPECT_EQ(rig.sent[3].email.subject, "Your Lake balance is below $0");
    EXPECT_EQ(rig.sent[3].key, "lake-balance-" + rig.customer + "-negative-2");

    // A failed send is tried again on the next refresh, not marked sent.
    rig.use(45'000); // -$47: within $10 of the limit
    rig.mail_fails = true;
    auto report = rig.refresh();
    EXPECT_EQ(report.email_failures, 1);
    EXPECT_TRUE(rig.balance().limit_warning_notified_at.empty());
    rig.mail_fails = false;
    rig.refresh();
    ASSERT_EQ(rig.sent.size(), 5U);
    EXPECT_EQ(rig.sent[4].email.subject, "Your Lake balance is close to its limit");
    EXPECT_EQ(audit_of(*rig.store, "billing.balance.email").size(), 5U);
}

TEST(Balance, AutoTopUpIsOffByDefault) {
    BalanceRig rig;
    rig.credit(20 * dollar, "cs_first");
    rig.use(19'000); // $1 left, under the $5 threshold
    rig.refresh();
    EXPECT_TRUE(rig.cards->charges.empty()) << "no auto top-up until an owner turns it on";
    const auto read = rig.owner("account_balance", {{"customer_id", rig.customer}});
    ASSERT_TRUE(read.ok);
    EXPECT_EQ(read.value["auto_top_up"]["enabled"], false);
    EXPECT_EQ(read.value["auto_top_up"]["card_saved"], false);
}

TEST(Balance, AutoTopUpChargesOncePerCrossingAndIsCreditedFromTheWebhook) {
    BalanceRig rig;
    rig.credit(20 * dollar, "cs_first");

    // Turning it on saves a card through Stripe first: it is on once Stripe says so.
    auto set = rig.owner("set_auto_top_up", {{"customer_id", rig.customer}, {"enabled", true}});
    ASSERT_TRUE(set.ok) << set.error.message;
    EXPECT_TRUE(set.value.contains("checkout_url"));
    EXPECT_EQ(set.value["auto_top_up"]["enabled"], false);
    EXPECT_EQ(set.value["auto_top_up"]["awaiting_card"], true);
    EXPECT_EQ(set.value["auto_top_up"]["threshold_micros"], 5 * dollar);
    EXPECT_EQ(set.value["auto_top_up"]["amount_micros"], 20 * dollar);
    ASSERT_EQ(rig.cards->asked.size(), 1U);
    EXPECT_EQ(rig.cards->asked[0].purpose, "card_setup");
    auto refused = rig.owner(
        "set_auto_top_up",
        {{"customer_id", rig.customer}, {"enabled", true}, {"amount_micros", 10 * dollar}});
    EXPECT_EQ(refused.error.code, "invalid_input") << "never less than the minimum top-up";

    auto op = rig.operator_context();
    lake::api::PaymentNotice saved;
    saved.event_id = "evt_seti_1";
    saved.event_type = "setup_intent.succeeded";
    saved.balance = lake::api::PaymentNotice::Balance{
        "card_saved", rig.customer, "seti_1", "", 0, "pm_card_visa", ""};
    auto applied = lake::api::apply_balance_notice(op, "stripe", saved);
    EXPECT_EQ(applied.status, 200) << applied.body;
    EXPECT_EQ(applied.body["auto_top_up"], true) << applied.body;
    EXPECT_EQ(lake::api::apply_balance_notice(op, "stripe", saved).body["duplicate"], true);

    // Above the threshold nothing is charged.
    rig.use(10'000); // $10 left
    rig.refresh();
    EXPECT_TRUE(rig.cards->charges.empty());

    // Below it: one charge, and further refreshes while it is in flight charge nothing.
    rig.use(7'000); // $3 left
    auto report = rig.refresh();
    EXPECT_EQ(report.auto_top_ups, 1);
    rig.refresh();
    rig.refresh();
    ASSERT_EQ(rig.cards->charges.size(), 1U);
    EXPECT_EQ(rig.cards->charges[0].amount_cents, 2000);
    EXPECT_EQ(rig.cards->charges[0].payment_method, "pm_card_visa");
    EXPECT_EQ(rig.cards->charges[0].idempotency_key, "lake-auto-top-up-" + rig.customer + "-1");
    EXPECT_FALSE(rig.balance().auto_top_up_pending.empty());
    EXPECT_EQ(rig.balance().credits_micros, 20 * dollar) << "credited only from the webhook";

    // payment_intent.succeeded credits it, once, and clears the charge in flight.
    lake::api::PaymentNotice paid;
    paid.event_id = "evt_pi_1";
    paid.event_type = "payment_intent.succeeded";
    paid.balance = lake::api::PaymentNotice::Balance{
        "credit", rig.customer, "pi_fake_1", "auto_top_up", 2000, "", ""};
    EXPECT_EQ(lake::api::apply_balance_notice(op, "stripe", paid).body["credited"], true);
    EXPECT_EQ(lake::api::apply_balance_notice(op, "stripe", paid).body["duplicate"], true);
    auto balance = rig.balance();
    EXPECT_EQ(balance.credits_micros, 40 * dollar);
    EXPECT_TRUE(balance.auto_top_up_pending.empty());
    EXPECT_EQ(balance.balance_micros(), 23 * dollar);

    // The next crossing is a new charge with its own key.
    rig.use(19'000); // $4 left
    rig.refresh();
    ASSERT_EQ(rig.cards->charges.size(), 2U);
    EXPECT_EQ(rig.cards->charges[1].idempotency_key, "lake-auto-top-up-" + rig.customer + "-2");
    EXPECT_EQ(audit_of(*rig.store, "billing.balance.auto_top_up_started").size(), 2U);
}

TEST(Balance, AutoTopUpStopsAtTheDailyLimit) {
    BalanceRig rig;
    rig.runtime->settings.auto_top_ups_per_day = 1;
    rig.credit(20 * dollar, "cs_first");
    ASSERT_TRUE(
        rig.owner("set_auto_top_up", {{"customer_id", rig.customer}, {"enabled", true}}).ok);
    auto op = rig.operator_context();
    lake::api::PaymentNotice saved;
    saved.event_id = "evt_seti";
    saved.balance =
        lake::api::PaymentNotice::Balance{"card_saved", rig.customer, "seti", "", 0, "pm_1", ""};
    lake::api::apply_balance_notice(op, "stripe", saved);
    rig.use(18'000);
    rig.refresh();
    lake::api::PaymentNotice paid;
    paid.event_id = "evt_pi";
    paid.balance = lake::api::PaymentNotice::Balance{
        "credit", rig.customer, "pi_1", "auto_top_up", 2000, "", ""};
    lake::api::apply_balance_notice(op, "stripe", paid);
    rig.use(20'000);
    rig.refresh();
    EXPECT_EQ(rig.cards->charges.size(), 1U) << "one a day at this limit";
}

TEST(Balance, ADeclinedAutoTopUpTurnsItOffAndEmailsTheOwner) {
    BalanceRig rig;
    rig.credit(20 * dollar, "cs_first");
    ASSERT_TRUE(
        rig.owner("set_auto_top_up", {{"customer_id", rig.customer}, {"enabled", true}}).ok);
    auto op = rig.operator_context();
    lake::api::PaymentNotice saved;
    saved.event_id = "evt_seti";
    saved.balance =
        lake::api::PaymentNotice::Balance{"card_saved", rig.customer, "seti", "", 0, "pm_1", ""};
    lake::api::apply_balance_notice(op, "stripe", saved);

    rig.cards->decline = true;
    rig.use(17'000); // $3 left
    auto report = rig.refresh();
    EXPECT_EQ(report.auto_top_up_failures, 1);
    auto balance = rig.balance();
    EXPECT_FALSE(balance.auto_top_up) << "a decline turns it off";
    EXPECT_NE(balance.auto_top_up_off_reason.find("declined"), std::string::npos);
    ASSERT_EQ(rig.sent.size(), 1U);
    EXPECT_EQ(rig.sent[0].email.subject, "Your Lake auto top-up was turned off");
    EXPECT_NE(rig.sent[0].email.text.find("/billing/add-funds?customer_id=" + rig.customer),
              std::string::npos);
    // It stays off: no more charges, no more emails.
    rig.use(1'000);
    rig.refresh();
    rig.refresh();
    EXPECT_EQ(rig.cards->charges.size(), 1U);
    EXPECT_EQ(rig.sent.size(), 1U);
    EXPECT_EQ(audit_of(*rig.store, "billing.balance.auto_top_up_failed").size(), 1U);

    // An asynchronous failure from the webhook does the same.
    rig.cards->decline = false;
    auto on = rig.owner("set_auto_top_up", {{"customer_id", rig.customer}, {"enabled", true}});
    ASSERT_TRUE(on.ok) << on.error.message;
    EXPECT_EQ(on.value["auto_top_up"]["enabled"], true) << "the saved card is reused";
    lake::api::PaymentNotice failed;
    failed.event_id = "evt_pi_failed";
    failed.event_type = "payment_intent.payment_failed";
    failed.balance = lake::api::PaymentNotice::Balance{
        "auto_top_up_failed", rig.customer, "pi_x", "", 0, "", "Your card has insufficient funds."};
    EXPECT_EQ(lake::api::apply_balance_notice(op, "stripe", failed).body["auto_top_up"], false);
    rig.refresh();
    ASSERT_EQ(rig.sent.size(), 2U);
    EXPECT_EQ(rig.sent[1].email.subject, "Your Lake auto top-up was turned off");
}

TEST(StripeBalance, CheckoutsChargesAndEventsAreLakesOwn) {
    std::vector<lake::api::StripeRequest> requests;
    int status = 200;
    std::string body;
    const auto provider = lake::api::stripe_payment_provider(
        {"sk_test_lake1114fakekey001", std::string(webhook_secret)},
        std::string(this_lake),
        [&](const lake::api::StripeRequest& request) {
            requests.push_back(request);
            return lake::api::StripeResponse{status, body, {}};
        });
    const lake::api::PaymentAccount account{"acct-1", "Account", "cus_1", 0, {}};
    const auto decoded = [](const std::string& form) {
        std::string out;
        for (std::size_t i = 0; i < form.size(); ++i) {
            if (form[i] == '%' && i + 2 < form.size()) {
                out.push_back(static_cast<char>(std::stoi(form.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(form[i] == '+' ? ' ' : form[i]);
            }
        }
        return out;
    };
    const auto header = [](const lake::api::StripeRequest& request, std::string_view name) {
        for (const auto& [key, value] : request.headers) {
            if (key == name) {
                return value;
            }
        }
        return std::string();
    };

    body = R"({"id":"cs_test_1","url":"https://checkout.stripe.com/c/1"})";
    auto session = provider->create_balance_checkout(
        account, {"top_up", 2000, "https://lake.test/ok", "https://lake.test/cancel", "a1"});
    ASSERT_TRUE(session) << session.error();
    EXPECT_EQ(session->id, "cs_test_1");
    std::string form = decoded(requests.back().body);
    EXPECT_EQ(requests.back().url, "https://api.stripe.com/v1/checkout/sessions");
    EXPECT_NE(form.find("mode=payment"), std::string::npos);
    EXPECT_NE(form.find("customer=cus_1"), std::string::npos);
    EXPECT_NE(form.find("line_items[0][price_data][product]=lake_usage"), std::string::npos);
    EXPECT_NE(form.find("line_items[0][price_data][unit_amount]=2000"), std::string::npos);
    EXPECT_NE(form.find("metadata[lake_customer_id]=acct-1"), std::string::npos);
    EXPECT_NE(form.find("metadata[lake_balance]=top_up"), std::string::npos);
    EXPECT_NE(form.find("metadata[lake_instance]=balance.lake.test"), std::string::npos);
    EXPECT_EQ(header(requests.back(), "Idempotency-Key"), "lake-top-up-acct-1-a1");

    session = provider->create_balance_checkout(
        account, {"card_setup", 0, "https://lake.test/ok", "https://lake.test/cancel", "a2"});
    ASSERT_TRUE(session) << session.error();
    form = decoded(requests.back().body);
    EXPECT_NE(form.find("mode=setup"), std::string::npos);
    EXPECT_NE(form.find("setup_intent_data[metadata][lake_balance]=card_setup"), std::string::npos);
    EXPECT_NE(form.find("setup_intent_data[metadata][lake_customer_id]=acct-1"), std::string::npos);
    EXPECT_EQ(form.find("line_items"), std::string::npos) << "a setup charges nothing";

    body = R"({"id":"pi_1","status":"succeeded"})";
    auto charged =
        provider->charge_saved_payment_method(account, {"pm_1", 2000, "lake-auto-top-up-acct-1-1"});
    ASSERT_TRUE(charged) << charged.error();
    form = decoded(requests.back().body);
    EXPECT_EQ(requests.back().url, "https://api.stripe.com/v1/payment_intents");
    EXPECT_NE(form.find("off_session=true"), std::string::npos);
    EXPECT_NE(form.find("confirm=true"), std::string::npos);
    EXPECT_NE(form.find("payment_method=pm_1"), std::string::npos);
    EXPECT_NE(form.find("metadata[lake_balance]=auto_top_up"), std::string::npos);
    EXPECT_EQ(header(requests.back(), "Idempotency-Key"), "lake-auto-top-up-acct-1-1");
    body = R"({"id":"pi_2","status":"requires_action"})";
    EXPECT_FALSE(provider->charge_saved_payment_method(account, {"pm_1", 2000, "k2"}))
        << "a charge that needs the person is a failure";
    status = 402;
    body =
        R"({"error":{"type":"card_error","code":"card_declined","message":"Your card was declined."}})";
    charged = provider->charge_saved_payment_method(account, {"pm_1", 2000, "k3"});
    ASSERT_FALSE(charged);
    EXPECT_NE(charged.error().find("Your card was declined."), std::string::npos);

    // Events.
    const auto parse = [](const json& event) {
        auto notice = lake::api::parse_stripe_event(event.dump(), this_lake);
        EXPECT_TRUE(notice);
        return notice ? *notice : lake::api::PaymentNotice{};
    };
    auto notice =
        parse(event("e1", "checkout.session.completed", top_up_session("acct-1", "cs_1", 2500)));
    ASSERT_TRUE(notice.balance);
    EXPECT_EQ(notice.balance->kind, "credit");
    EXPECT_EQ(notice.balance->credit_kind, "top_up");
    EXPECT_EQ(notice.balance->amount_cents, 2500);
    EXPECT_EQ(notice.balance->reference, "cs_1");
    EXPECT_TRUE(notice.order_state.empty()) << "never taken for an order";
    notice = parse(event(
        "e2", "checkout.session.completed", top_up_session("acct-1", "cs_2", 2500, "unpaid")));
    EXPECT_FALSE(notice.balance);
    EXPECT_FALSE(notice.foreign);
    const json tags{
        {"lake_customer_id", "acct-1"}, {"orama_app", "lake"}, {"lake_instance", this_lake}};
    json seti{{"id", "seti_1"}, {"payment_method", "pm_9"}, {"metadata", tags}};
    seti["metadata"]["lake_balance"] = "card_setup";
    notice = parse(event("e3", "setup_intent.succeeded", seti));
    ASSERT_TRUE(notice.balance);
    EXPECT_EQ(notice.balance->kind, "card_saved");
    EXPECT_EQ(notice.balance->payment_method, "pm_9");
    json intent{{"id", "pi_9"}, {"amount", 2000}, {"amount_received", 2000}, {"metadata", tags}};
    intent["metadata"]["lake_balance"] = "auto_top_up";
    notice = parse(event("e4", "payment_intent.succeeded", intent));
    ASSERT_TRUE(notice.balance);
    EXPECT_EQ(notice.balance->credit_kind, "auto_top_up");
    EXPECT_EQ(notice.balance->amount_cents, 2000);
    intent["last_payment_error"] = {{"message", "Your card was declined."}};
    notice = parse(event("e5", "payment_intent.payment_failed", intent));
    ASSERT_TRUE(notice.balance);
    EXPECT_EQ(notice.balance->kind, "auto_top_up_failed");
    EXPECT_EQ(notice.balance->message, "Your card was declined.");
    // Cloud v1's and another Lake's are foreign.
    notice = parse(event(
        "e6", "payment_intent.succeeded", {{"id", "pi_v1"}, {"metadata", {{"team_id", "t"}}}}));
    EXPECT_TRUE(notice.foreign);
    intent["metadata"]["lake_instance"] = "other.lake.test";
    notice = parse(event("e7", "payment_intent.succeeded", intent));
    EXPECT_TRUE(notice.foreign);
}

// The gate is one list on the registry (lake#1114): every capability whose name says it creates,
// starts, runs, deploys, trains, launches, builds or stores something is either on the list or
// classified here as starting nothing that costs money, and every name on the list is registered.
TEST(PaidWorkContract, EveryCapabilityThatStartsPaidWorkIsMarked) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::register_billing_capabilities(registry);
    std::set<std::string> registered;
    for (const auto& capability : registry.list()) {
        registered.insert(capability->name);
        EXPECT_EQ(capability->starts_paid_work, lake::api::starts_paid_work(capability->name))
            << capability->name << ": the list marks it";
    }
    for (const auto& work : lake::api::paid_work_capabilities()) {
        EXPECT_TRUE(registered.contains(std::string(work.capability)))
            << work.capability << " is on the paid work list but not registered";
        EXPECT_FALSE(work.why.empty());
    }
    // Starts nothing that costs money: a record, an identity, a configuration, a credential, a
    // read that takes input, or work on something already running and billed as it runs.
    static const std::map<std::string, std::string> free_to_start{
        {"start_oauth", "a sign-in"},
        {"start_sso_oidc", "a sign-in"},
        {"start_sso_saml", "a sign-in"},
        {"start_top_up", "adds funds"},
        {"create_customer", "an account record"},
        {"create_user", "an identity"},
        {"create_organization", "an identity"},
        {"create_organization_space", "an account record"},
        {"create_team", "an identity"},
        {"create_membership", "an identity"},
        {"create_invitation", "an identity"},
        {"create_grant", "a permission"},
        {"create_token", "a credential"},
        {"create_encryption_key", "a key record"},
        {"create_federation_issuer", "a configuration"},
        {"create_pki_authority", "a credential"},
        {"create_secret_mount", "a secret record"},
        {"create_application_organization", "an identity"},
        {"create_application_webhook", "a configuration"},
        {"create_webhook", "a configuration"},
        {"create_domain", "a configuration"},
        {"create_network_connection", "network desired state; the workload it reaches is billed"},
        {"create_network_egress_rule", "a configuration"},
        {"create_issue", "a record"},
        {"create_issue_comment", "a record"},
        {"create_review", "a record"},
        {"create_review_comment", "a record"},
        {"create_release", "a record"},
        {"create_repository", "an empty repository; pushes grow it (push is gated)"},
        {"create_image_repository", "a route"},
        {"create_drive", "a record; its files are gated when stored"},
        {"create_drive_folder", "a record"},
        {"create_drive_folder_entry", "a record"},
        {"create_drive_shortcut", "a record"},
        {"create_drive_share_link", "a record"},
        {"create_object", "a record"},
        {"create_project", "a record"},
        {"create_projects_from_opportunities", "records"},
        {"create_pipeline", "a definition; running it is gated"},
        {"create_solution", "a definition"},
        {"create_methodology", "a definition; validating and running it are gated"},
        {"create_agent", "a definition; launching it is gated"},
        {"create_agent_from_proposal", "a definition"},
        {"create_mcp_connection", "a configuration; its agent is gated"},
        {"create_connection", "a configuration"},
        {"create_stream", "a topic on the running workspace engine"},
        {"build_logs", "a read"},
        {"import_n8n", "a pipeline definition from an n8n workflow"},
        {"upload_chunks", "part of a push; the push is gated"},
    };
    static const std::regex starts(
        "^(create|launch|start|deploy|scale|run|train|retrain|build|expose|upload|import|"
        "schedule|generate|transcribe|restart|rollback)(_.*)?$");
    std::string unmarked;
    for (const std::string& name : registered) {
        if (std::regex_match(name, starts) && !lake::api::starts_paid_work(name) &&
            !free_to_start.contains(name)) {
            unmarked += " " + name;
        }
    }
    std::string stale;
    for (const auto& [name, why] : free_to_start) {
        if (!registered.contains(name)) {
            stale += " " + name;
        }
    }
    EXPECT_TRUE(stale.empty()) << "classified as free but not registered:" << stale;
    EXPECT_TRUE(unmarked.empty())
        << "add each to paid_work_capabilities() (src/api/balance.cpp), or to free_to_start "
           "here with why it costs nothing:"
        << unmarked;
}
