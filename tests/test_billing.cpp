// Billing (lake#564, decision 56, docs/api/billing.md): the price list, plans chosen by an
// account, the bill worked out from usage read through usage_over_time, invoices with their frozen
// lines and payment state, the manual provider, and the switch that leaves billing absent on an
// enterprise install.

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "audit_test_support.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/memory_store.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::UsageRecorder;
using nlohmann::json;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";
constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;

std::string month_name(std::chrono::sys_days day) {
    return std::format("{:%Y-%m}", day);
}

std::chrono::sys_days first_of(std::chrono::year_month month) {
    return std::chrono::sys_days{month / std::chrono::day{1}};
}

std::chrono::year_month this_month() {
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return today.year() / today.month();
}

// A registry with billing on, over two ready workspaces of one account, and the usage recorder.
struct BillingRig {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::string user_id;
    std::string customer;
    std::string home;
    std::string lab;
    UsageRecorder recorder{store.get(), workspaces.service.get()};
    lake::api::Registry registry;

    explicit BillingRig(bool billing = true) {
        const auto user =
            store->create_user({"taap:1:ed25519:billing-owner", "owner@example.test", "Owner"});
        EXPECT_TRUE(user) << user.error().message;
        user_id = user->id;
        const auto created = store->create_customer({"individual", user->id, "", "billing"});
        EXPECT_TRUE(created) << created.error().message;
        customer = created->id;
        // The owner holds admin on the account, as sign-up grants it.
        const auto object = store->find_policy_object("customer", customer);
        EXPECT_TRUE(object) << object.error().message;
        EXPECT_TRUE(store->create_grant({"taap:1:ed25519:billing-owner",
                                         object->id,
                                         "admin",
                                         "attestation-owner",
                                         "admin",
                                         "request-owner"}));
        home = workspaces.create(customer, "home");
        lab = workspaces.create(customer, "lab");
        lake::api::register_core_capabilities(registry, "test");
        if (billing) {
            lake::api::register_billing_capabilities(registry);
        }
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
        return context;
    }

    // The account's owner, signed in.
    lake::api::ApiContext owner_context() {
        auto context = operator_context();
        context.operator_credential = false;
        context.acting_identity = "taap:1:ed25519:billing-owner";
        context.acting_user_id = user_id;
        context.session_credential = true;
        context.scopes = {"*"};
        return context;
    }

    lake::api::ApiResult call(const std::string& name, json input) {
        auto context = operator_context();
        return call(name, std::move(input), context);
    }

    lake::api::ApiResult call(const std::string& name, json input, lake::api::ApiContext& context) {
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }

    void flush() {
        static_cast<void>(liquid::runtime::block_on(recorder.flush()));
    }

    // The team plan: $20 a month, calls at $1 per 1000 beyond 1000, storage at $0.10 a GiB beyond
    // 1 GiB, model tokens at $2 per million.
    json team_plan() {
        return {{"id", "team"},
                {"name", "Team"},
                {"description", "For a team."},
                {"currency", "USD"},
                {"base_micros", 20'000'000},
                {"position", 1},
                {"prices",
                 json::array({{{"dimension", "calls"},
                               {"unit_micros", 1'000'000},
                               {"per", 1000},
                               {"included", 1000}},
                              {{"dimension", "storage"},
                               {"unit_micros", 100'000},
                               {"per", gib},
                               {"included", gib}},
                              {{"dimension", "model_tokens"},
                               {"unit_micros", 2'000'000},
                               {"per", 1'000'000}}})}};
    }
};

const json* line(const json& bill, const std::string& dimension) {
    for (const json& entry : bill["lines"]) {
        if (entry["dimension"] == dimension) {
            return &entry;
        }
    }
    return nullptr;
}

TEST(BillLines, PriceRoundsUpAndChargesOnlyBeyondWhatIsIncluded) {
    EXPECT_EQ(lake::api::price_of(0, 1'000'000, 1000), 0U);
    EXPECT_EQ(lake::api::price_of(1000, 1'000'000, 1000), 1'000'000U);
    EXPECT_EQ(lake::api::price_of(1, 1'000'000, 1000), 1000U);
    EXPECT_EQ(lake::api::price_of(1, 1, 3), 1U) << "a part of a micro-unit rounds up";
    EXPECT_EQ(lake::api::price_of(~0ULL, ~0ULL, 1), 9'223'372'036'854'775'807ULL)
        << "capped at the largest bigint";

    lake::control::BillingPlan plan;
    plan.id = "team";
    plan.name = "Team";
    plan.base_micros = 20'000'000;
    plan.prices = {{"calls", 1'000'000, 1000, 1000}, {"storage", 100'000, gib, gib}};
    const auto lines =
        lake::api::bill_lines(plan, {{"calls", 3500}, {"storage", gib / 2}}, "2026-09");
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[0].kind, "plan");
    EXPECT_EQ(lines[0].description, "Team plan, 2026-09");
    EXPECT_EQ(lines[0].amount_micros, 20'000'000U);
    EXPECT_EQ(lines[1].dimension, "calls");
    EXPECT_EQ(lines[1].unit, "calls");
    EXPECT_EQ(lines[1].quantity, 3500U);
    EXPECT_EQ(lines[1].billable, 2500U);
    EXPECT_EQ(lines[1].amount_micros, 2'500'000U);
    EXPECT_EQ(lines[2].description, "Storage, average over the month");
    EXPECT_EQ(lines[2].billable, 0U) << "within what the plan includes";
    EXPECT_EQ(lines[2].amount_micros, 0U);
}

TEST(Billing, AnAccountChoosesAPlanNowAndAChangeStartsNextMonth) {
    BillingRig rig;
    ASSERT_TRUE(rig.call("set_billing_plan", rig.team_plan()).ok);
    ASSERT_TRUE(rig.call("set_billing_plan",
                         {{"id", "free"}, {"name", "Free"}, {"base_micros", 0}, {"position", 0}})
                    .ok);
    ASSERT_TRUE(
        rig.call("set_billing_plan",
                 {{"id", "legacy"}, {"name", "Legacy"}, {"base_micros", 5}, {"active", false}})
            .ok);

    auto owner = rig.owner_context();
    const auto plans = rig.call("billing_plans", json::object(), owner);
    ASSERT_TRUE(plans.ok) << plans.error.message;
    ASSERT_EQ(plans.value["plans"].size(), 2U) << "a retired plan is not offered";
    EXPECT_EQ(plans.value["plans"][0]["id"], "free");
    EXPECT_EQ(plans.value["plans"][1]["prices"][0]["dimension"], "calls");
    EXPECT_EQ(plans.value["plans"][1]["prices"][0]["unit"], "calls");
    EXPECT_EQ(rig.call("billing_plans", {{"include_retired", true}}).value["plans"].size(), 3U);

    // No plan yet: a preview says to choose one.
    const auto none = rig.call("bill_preview", {{"customer_id", rig.customer}}, owner);
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(none.error.code, "conflict");

    const std::string now = month_name(first_of(this_month()));
    const std::string next = month_name(first_of(this_month() + std::chrono::months{1}));
    const auto first = rig.call(
        "choose_billing_plan", {{"customer_id", rig.customer}, {"plan_id", "free"}}, owner);
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(first.value["current"]["plan_id"], "free");
    EXPECT_EQ(first.value["current"]["effective_from"], now + "-01") << "from this month";
    EXPECT_TRUE(first.value["next"].is_null());
    EXPECT_EQ(first.value["provider"]["name"], "manual");

    const auto upgrade = rig.call(
        "choose_billing_plan", {{"customer_id", rig.customer}, {"plan_id", "team"}}, owner);
    ASSERT_TRUE(upgrade.ok) << upgrade.error.message;
    EXPECT_EQ(upgrade.value["current"]["plan_id"], "free");
    EXPECT_EQ(upgrade.value["next"]["plan_id"], "team");
    EXPECT_EQ(upgrade.value["next"]["effective_from"], next + "-01");

    // Choosing the current plan again cancels the change.
    const auto kept = rig.call(
        "choose_billing_plan", {{"customer_id", rig.customer}, {"plan_id", "free"}}, owner);
    ASSERT_TRUE(kept.ok);
    EXPECT_TRUE(kept.value["next"].is_null());
    EXPECT_EQ(kept.value["history"].size(), 1U);

    // A retired or unknown plan cannot be chosen.
    EXPECT_EQ(rig.call("choose_billing_plan",
                       {{"customer_id", rig.customer}, {"plan_id", "legacy"}},
                       owner)
                  .error.code,
              "invalid_input");
    EXPECT_EQ(
        rig.call("choose_billing_plan", {{"customer_id", rig.customer}, {"plan_id", "gold"}}, owner)
            .error.code,
        "not_found");

    // Only the operator sets prices; a person of another account reads nothing of this one.
    EXPECT_EQ(rig.call("set_billing_plan", rig.team_plan(), owner).error.code, "unauthorized");
    const auto outsider_user =
        rig.store->create_user({"taap:1:ed25519:billing-outsider", "out@example.test", "Out"});
    ASSERT_TRUE(outsider_user);
    ASSERT_TRUE(rig.store->create_customer({"individual", outsider_user->id, "", "billing-out"}));
    auto outsider = rig.owner_context();
    outsider.acting_identity = outsider_user->taap_id;
    outsider.acting_user_id = outsider_user->id;
    EXPECT_EQ(rig.call("billing_account", {{"customer_id", rig.customer}}, outsider).error.code,
              "unauthorized");
    EXPECT_EQ(rig.call("list_invoices", {{"customer_id", rig.customer}}, outsider).error.code,
              "unauthorized");
    EXPECT_EQ(rig.call("choose_billing_plan",
                       {{"customer_id", rig.customer}, {"plan_id", "team"}},
                       outsider)
                  .error.code,
              "unauthorized");

    // Every choice is in the central audit, refusals included.
    const auto events = lake::test::audit_events(*rig.store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    int chosen = 0;
    int refused = 0;
    for (const auto& event : *events) {
        if (event.draft.action != "choose_billing_plan") {
            continue;
        }
        if (event.draft.outcome == "success") {
            ++chosen;
            EXPECT_EQ(event.draft.object, "customer/" + rig.customer + "/billing");
        } else {
            ++refused;
        }
    }
    EXPECT_EQ(chosen, 3);
    EXPECT_GE(refused, 3);
}

TEST(Billing, AMonthsBillIsWorkedOutFromTheUsageOfEveryWorkspace) {
    BillingRig rig;
    ASSERT_TRUE(rig.call("set_billing_plan", rig.team_plan()).ok);
    // The account has been on Team since three months ago.
    const auto start = this_month() - std::chrono::months{3};
    ASSERT_TRUE(rig.store->set_billing_subscription(
        rig.customer, month_name(first_of(start)) + "-01", "team"));
    const auto last = this_month() - std::chrono::months{1};
    const std::chrono::sys_time<std::chrono::hours> day1{first_of(last)};
    const auto day2 = day1 + std::chrono::hours(24);
    rig.recorder.add(rig.home, "calls", 2000, {{"kind", "api"}}, day1 + std::chrono::hours(3));
    rig.recorder.add(rig.lab, "calls", 1500, {{"kind", "mcp"}}, day2);
    rig.recorder.add(rig.home, "model_tokens", 1'500'000, {{"kind", "input"}}, day2);
    // Storage: 2 GiB the first day and 4 GiB the second, across the workspaces: 3 GiB on average.
    rig.recorder.sample(rig.home, "workspace_data", gib, day1);
    rig.recorder.sample(rig.lab, "workspace_data", gib, day1);
    rig.recorder.sample(rig.home, "workspace_data", 3 * gib, day2);
    rig.recorder.sample(rig.lab, "workspace_data", gib, day2);
    rig.flush();

    const std::string period = month_name(first_of(last));
    auto owner = rig.owner_context();
    const auto bill =
        rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}}, owner);
    ASSERT_TRUE(bill.ok) << bill.error.message;
    EXPECT_TRUE(bill.value["complete"]);
    EXPECT_EQ(bill.value["plan"]["id"], "team");
    EXPECT_EQ(bill.value["lines"][0]["amount_micros"], 20'000'000);
    const json* calls = line(bill.value, "calls");
    ASSERT_NE(calls, nullptr);
    EXPECT_EQ((*calls)["quantity"], 3500);
    EXPECT_EQ((*calls)["billable"], 2500);
    EXPECT_EQ((*calls)["amount_micros"], 2'500'000);
    const json* storage = line(bill.value, "storage");
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ((*storage)["quantity"], 3 * gib);
    EXPECT_EQ((*storage)["amount_micros"], 200'000);
    const json* tokens = line(bill.value, "model_tokens");
    ASSERT_NE(tokens, nullptr);
    EXPECT_EQ((*tokens)["amount_micros"], 3'000'000);
    EXPECT_EQ(bill.value["total_micros"], 25'700'000);
    EXPECT_TRUE(bill.value["invoice"].is_null());

    // Only the operator issues the invoice, and only for a month that has ended.
    EXPECT_EQ(rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}}, owner)
                  .error.code,
              "unauthorized");
    EXPECT_EQ(
        rig.call("issue_invoice",
                 {{"customer_id", rig.customer}, {"period", month_name(first_of(this_month()))}})
            .error.code,
        "conflict");
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["state"], "due");
    EXPECT_EQ(issued.value["provider"], "manual");
    EXPECT_FALSE(issued.value["instructions"].get<std::string>().empty());
    EXPECT_EQ(issued.value["total_micros"], 25'700'000);
    EXPECT_EQ(issued.value["lines"].size(), 4U);
    const std::string invoice_id = issued.value["id"];

    // The lines are frozen: usage recorded later does not change the invoice.
    rig.recorder.add(rig.home, "calls", 9000, {{"kind", "api"}}, day2);
    rig.flush();
    const auto read =
        rig.call("get_invoice", {{"customer_id", rig.customer}, {"invoice_id", invoice_id}}, owner);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["total_micros"], 25'700'000);
    EXPECT_EQ(rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}}, owner)
                  .value["invoice"]["id"],
              invoice_id);
    EXPECT_EQ(
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}}).error.code,
        "conflict")
        << "one invoice a month";
    const auto listed = rig.call("list_invoices", {{"customer_id", rig.customer}}, owner);
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["invoices"].size(), 1U);

    // Paid by hand, with the transfer's reference; a paid invoice is never voided.
    EXPECT_EQ(rig.call("record_invoice_payment", {{"invoice_id", invoice_id}}, owner).error.code,
              "unauthorized");
    const auto paid =
        rig.call("record_invoice_payment", {{"invoice_id", invoice_id}, {"reference", "WIRE-123"}});
    ASSERT_TRUE(paid.ok) << paid.error.message;
    EXPECT_EQ(paid.value["state"], "paid");
    EXPECT_EQ(paid.value["provider_reference"], "WIRE-123");
    EXPECT_FALSE(paid.value["paid_at"].get<std::string>().empty());
    EXPECT_EQ(
        rig.call("void_invoice", {{"invoice_id", invoice_id}, {"note", "mistake"}}).error.code,
        "conflict");
    EXPECT_EQ(
        rig.call("get_invoice",
                 {{"customer_id", rig.customer}, {"invoice_id", lake::control::new_uuid_v4()}},
                 owner)
            .error.code,
        "not_found");
}

TEST(Billing, AVoidedMonthCanBeInvoicedAgain) {
    BillingRig rig;
    ASSERT_TRUE(rig.call("set_billing_plan", rig.team_plan()).ok);
    const auto last = this_month() - std::chrono::months{1};
    ASSERT_TRUE(rig.store->set_billing_subscription(
        rig.customer, month_name(first_of(last)) + "-01", "team"));
    const std::string period = month_name(first_of(last));
    const auto first =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(first.value["total_micros"], 20'000'000) << "the plan's fee alone";
    const auto voided =
        rig.call("void_invoice", {{"invoice_id", first.value["id"]}, {"note", "wrong price"}});
    ASSERT_TRUE(voided.ok) << voided.error.message;
    EXPECT_EQ(voided.value["state"], "void");
    EXPECT_EQ(voided.value["note"], "wrong price");
    const auto again =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_GT(again.value["number"].get<std::uint64_t>(),
              first.value["number"].get<std::uint64_t>());
}

TEST(Billing, AnInvoiceWaitsForEveryWorkspacesUsage) {
    BillingRig rig;
    ASSERT_TRUE(rig.call("set_billing_plan", rig.team_plan()).ok);
    const auto last = this_month() - std::chrono::months{1};
    ASSERT_TRUE(rig.store->set_billing_subscription(
        rig.customer, month_name(first_of(last)) + "-01", "team"));
    const std::string period = month_name(first_of(last));
    rig.workspaces.records(rig.lab).metric_unavailable_next = 10;
    const auto preview =
        rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_FALSE(preview.value["complete"]);
    ASSERT_EQ(preview.value["unread_workspaces"].size(), 1U);
    EXPECT_EQ(preview.value["unread_workspaces"][0]["name"], "lab");
    const auto refused =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "store_unavailable");
    const auto invoices = rig.store->list_invoices(rig.customer);
    ASSERT_TRUE(invoices);
    EXPECT_TRUE(invoices->empty()) << "nothing is issued short";
}

// A payment provider that reports payments itself, and one that refuses.
struct FakeProvider final : lake::api::PaymentProvider {
    bool refuse = false;
    int requests = 0;
    std::string_view name() const override {
        return "fake";
    }
    std::string_view title() const override {
        return "Fake";
    }
    std::expected<lake::api::PaymentRequest, std::string>
    request_payment(const lake::control::Invoice& invoice) override {
        ++requests;
        if (refuse) {
            return std::unexpected(std::string("card declined"));
        }
        return lake::api::PaymentRequest{"fake-" + invoice.period, "Pay at the fake page."};
    }
    bool records_payments_by_hand() const override {
        return false;
    }
};

TEST(Billing, ThePaymentProviderIsAnInterface) {
    auto provider = std::make_shared<FakeProvider>();
    BillingRig rig(false);
    lake::api::register_billing_capabilities(rig.registry, provider);
    ASSERT_TRUE(rig.call("set_billing_plan", rig.team_plan()).ok);
    const auto last = this_month() - std::chrono::months{1};
    ASSERT_TRUE(rig.store->set_billing_subscription(
        rig.customer, month_name(first_of(last)) + "-01", "team"));
    const std::string period = month_name(first_of(last));

    provider->refuse = true;
    const auto refused =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    EXPECT_EQ(refused.error.code, "store_unavailable");
    EXPECT_TRUE(rig.store->list_invoices(rig.customer)->empty());

    provider->refuse = false;
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["provider"], "fake");
    EXPECT_EQ(issued.value["provider_reference"], "fake-" + period);
    EXPECT_EQ(issued.value["instructions"], "Pay at the fake page.");
    EXPECT_EQ(provider->requests, 2);
    // This provider reports payments itself: the operator does not record them.
    EXPECT_EQ(rig.call("record_invoice_payment", {{"invoice_id", issued.value["id"]}}).error.code,
              "conflict");
    EXPECT_EQ(rig.call("billing_account", {{"customer_id", rig.customer}})
                  .value["provider"]["records_payments_by_hand"],
              false);
}

TEST(Billing, AnEnterpriseInstallHasNoBilling) {
    EXPECT_TRUE(lake::api::billing_enabled("on"));
    EXPECT_TRUE(lake::api::billing_enabled("true"));
    EXPECT_TRUE(lake::api::billing_enabled("1"));
    EXPECT_FALSE(lake::api::billing_enabled(""));
    EXPECT_FALSE(lake::api::billing_enabled("off"));
    EXPECT_FALSE(lake::api::billing_enabled("enterprise"));

    for (const bool billing : {false, true}) {
        SCOPED_TRACE(billing ? "billing on" : "billing off");
        BillingRig rig(billing);
        for (const char* name : {"billing_plans",
                                 "set_billing_plan",
                                 "billing_account",
                                 "choose_billing_plan",
                                 "bill_preview",
                                 "issue_invoice",
                                 "list_invoices",
                                 "get_invoice",
                                 "record_invoice_payment",
                                 "void_invoice"}) {
            EXPECT_EQ(rig.registry.find(name) != nullptr, billing) << name;
        }
        EXPECT_EQ(lake::api::billing_registered(&rig.registry), billing);
        auto owner = rig.owner_context();
        const auto viewer = rig.call("viewer", json::object(), owner);
        ASSERT_TRUE(viewer.ok) << viewer.error.message;
        EXPECT_EQ(viewer.value["billing"], billing);
        // The Docs guide on billing is absent too.
        const auto docs = rig.call("list_docs", json::object(), owner);
        ASSERT_TRUE(docs.ok);
        bool listed = false;
        for (const json& page : docs.value["pages"]) {
            listed = listed || page["slug"] == "billing";
        }
        EXPECT_EQ(listed, billing);
        EXPECT_EQ(rig.call("get_doc", {{"slug", "billing"}}, owner).ok, billing);
        // Called by name, it is a capability lake does not have.
        if (!billing) {
            const auto absent = rig.call("bill_preview", {{"customer_id", rig.customer}}, owner);
            EXPECT_FALSE(absent.ok);
            EXPECT_EQ(absent.error.code, "unknown_capability");
        }
        // Usage tracking stays on everywhere.
        EXPECT_NE(rig.registry.find("usage_over_time"), nullptr);
        EXPECT_NE(rig.registry.find("usage_limits"), nullptr);
    }
}

} // namespace
