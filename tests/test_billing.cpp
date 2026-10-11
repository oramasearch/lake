// Billing (lake#564, decision 56, docs/api/billing.md): the price list (usage only, lake#635: one
// rate per dimension, no plans, no fee, nothing included), the bill worked out from usage read
// through usage_over_time, invoices with their frozen lines and payment state, the manual provider,
// and the switch that leaves billing absent on an enterprise install.

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

    // A price list: calls at $1 per 1000, storage at $0.10 a GiB, model tokens at $2 per million,
    // in force from `effective_from` (this month when empty).
    json price_list(const std::string& effective_from = {}) {
        json input{
            {"prices",
             json::array({{{"dimension", "calls"}, {"unit_micros", 1'000'000}, {"per", 1000}},
                          {{"dimension", "storage"}, {"unit_micros", 100'000}, {"per", gib}},
                          {{"dimension", "model_tokens"},
                           {"unit_micros", 2'000'000},
                           {"per", 1'000'000}}})}};
        if (!effective_from.empty()) {
            input["effective_from"] = effective_from;
        }
        return input;
    }

    // Publishes the price list in force from `months_ago` months back.
    void publish(int months_ago) {
        const auto published =
            call("set_price_list",
                 price_list(month_name(first_of(this_month() - std::chrono::months{months_ago}))));
        EXPECT_TRUE(published.ok) << published.error.message;
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

TEST(BillLines, EveryUnitIsBilledAtTheListRateRoundedUp) {
    EXPECT_EQ(lake::api::price_of(0, 1'000'000, 1000), 0U);
    EXPECT_EQ(lake::api::price_of(1000, 1'000'000, 1000), 1'000'000U);
    EXPECT_EQ(lake::api::price_of(1, 1'000'000, 1000), 1000U);
    EXPECT_EQ(lake::api::price_of(1, 1, 3), 1U) << "a part of a micro-unit rounds up";
    EXPECT_EQ(lake::api::price_of(~0ULL, ~0ULL, 1), 9'223'372'036'854'775'807ULL)
        << "capped at the largest bigint";

    lake::api::PriceList list;
    list.id = "usage-202609-00000000000000001";
    list.effective_from = "2026-09";
    list.prices = {{"calls", 1'000'000, 1000, 0}, {"storage", 100'000, gib, 0}};
    const auto lines = lake::api::bill_lines(list, {{"calls", 3500}, {"storage", gib / 2}});
    ASSERT_EQ(lines.size(), 2U) << "no fee line: usage only";
    EXPECT_EQ(lines[0].kind, "usage");
    EXPECT_EQ(lines[0].dimension, "calls");
    EXPECT_EQ(lines[0].unit, "calls");
    EXPECT_EQ(lines[0].quantity, 3500U);
    EXPECT_EQ(lines[0].included, 0U);
    EXPECT_EQ(lines[0].billable, 3500U) << "nothing is included";
    EXPECT_EQ(lines[0].amount_micros, 3'500'000U);
    EXPECT_EQ(lines[1].description, "Storage, average over the month");
    EXPECT_EQ(lines[1].billable, gib / 2);
    EXPECT_EQ(lines[1].amount_micros, 50'000U);
}

// lake#637: time is billed by the minute on the month's summed seconds, rounded up once, so many
// short jobs are not each charged a minute.
TEST(BillLines, TimeIsBilledByTheMinuteOnTheMonthsTotal) {
    EXPECT_EQ(lake::api::billable_quantity("seconds", 0), 0U);
    EXPECT_EQ(lake::api::billable_quantity("seconds", 1), 60U);
    EXPECT_EQ(lake::api::billable_quantity("seconds", 60), 60U);
    EXPECT_EQ(lake::api::billable_quantity("seconds", 61), 120U);
    EXPECT_EQ(lake::api::billable_quantity("instance_seconds", 3599), 3600U);
    EXPECT_EQ(lake::api::billable_quantity("calls", 61), 61U) << "only time is rounded";
    EXPECT_EQ(lake::api::billable_quantity("gib_seconds", 61), 61U) << "volumes by GiB-month";
    EXPECT_EQ(lake::api::billable_quantity("seconds", ~0ULL), 9'223'372'036'854'775'807ULL);

    lake::api::PriceList list;
    list.prices = {{"build_compute", 5'000, 60, 0}, // $0.005 a minute
                   {"run_compute", 4'000, 60, 0},   // $0.004 a minute
                   {"instances", 1'350, 60, 0},     // $0.00135 an instance-minute
                   {"speech", 7'200, 60, 0}};       // $0.0072 a minute
    // 100 builds of one second each: 100 seconds, billed as 2 minutes, not 100.
    const auto lines = lake::api::bill_lines(
        list, {{"build_compute", 100}, {"run_compute", 3600}, {"instances", 89'280 * 60}});
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_EQ(lines[0].quantity, 100U);
    EXPECT_EQ(lines[0].billable, 120U);
    EXPECT_EQ(lines[0].amount_micros, 10'000U);
    EXPECT_EQ(lines[1].billable, 3600U);
    EXPECT_EQ(lines[1].amount_micros, 240'000U) << "an hour is $0.24";
    EXPECT_EQ(lines[2].amount_micros, 120'528'000U) << "1,488 instance-hours";
    EXPECT_EQ(lines[3].quantity, 0U);
    EXPECT_EQ(lines[3].amount_micros, 0U);
}

// lake#635: a month is billed at the list in force at its end. A list applies from the first day
// of its month; of the lists for a month or before, the latest month wins, then the last published.
TEST(BillLines, AMonthIsBilledAtTheListInForceAtItsEnd) {
    EXPECT_EQ(lake::api::price_list_id("2026-10", 1'760'000'000'000'000),
              "usage-202610-01760000000000000");
    EXPECT_EQ(lake::api::price_list_id("2026-13", 1), "");
    const auto row = [](std::string id, std::uint64_t rate) {
        lake::control::BillingPlan plan;
        plan.id = std::move(id);
        plan.name = "Usage";
        plan.created_at = "2026-10-11T00:00:00Z";
        plan.prices = {{"calls", rate, 1000, 0}};
        return plan;
    };
    const auto lists = lake::api::price_lists({
        row("usage-202610-00000000000000003", 3), // October, published third
        row("usage-202609-00000000000000001", 1), // September, published first
        row("usage-202609-00000000000000002", 2), // September again, published second
        row("usage-202612-00000000000000004", 4), // scheduled for December
        row("team", 9),                           // a plan from before lake#635: ignored
        row("usage-202613-00000000000000005", 5), // not a month: ignored
    });
    ASSERT_EQ(lists.size(), 4U);
    EXPECT_EQ(lists[0].effective_from, "2026-09");
    EXPECT_EQ(lists[0].published_at, "2026-10-11T00:00:00Z");
    const auto rate = [&](std::string_view period) -> std::uint64_t {
        const auto list = lake::api::price_list_for(lists, period);
        return list ? list->prices.at(0).unit_micros : 0;
    };
    EXPECT_EQ(rate("2026-08"), 0U) << "nothing published for August or before";
    EXPECT_EQ(rate("2026-09"), 2U) << "September's last publication prices all of September";
    EXPECT_EQ(rate("2026-10"), 3U) << "a list published for October prices the whole month";
    EXPECT_EQ(rate("2026-11"), 3U) << "a list stays in force until the next";
    EXPECT_EQ(rate("2026-12"), 4U);
    EXPECT_EQ(rate("2027-06"), 4U);
}

TEST(Billing, EveryAccountIsBilledAtTheOnePriceList) {
    BillingRig rig;
    auto owner = rig.owner_context();

    // Before a list is published there is nothing to bill at.
    const auto empty = rig.call("price_list", json::object(), owner);
    ASSERT_TRUE(empty.ok) << empty.error.message;
    EXPECT_TRUE(empty.value["list"].is_null());
    const auto none = rig.call("bill_preview", {{"customer_id", rig.customer}}, owner);
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(none.error.code, "conflict");
    EXPECT_NE(none.error.message.find("set_price_list"), std::string::npos);

    // Only the operator publishes; a dimension has one rate; there is no fee or included amount.
    EXPECT_EQ(rig.call("set_price_list", rig.price_list(), owner).error.code, "unauthorized");
    auto twice = rig.price_list();
    twice["prices"].push_back({{"dimension", "calls"}, {"unit_micros", 1}});
    EXPECT_EQ(rig.call("set_price_list", twice).error.code, "invalid_input");
    auto included = rig.price_list();
    included["prices"][0]["included"] = 1000;
    EXPECT_EQ(rig.call("set_price_list", included).error.code, "invalid_input");
    EXPECT_EQ(
        rig.call("set_price_list", {{"base_micros", 1}, {"prices", json::array()}}).error.code,
        "invalid_input");
    for (const char* retired : {"billing_plans", "set_billing_plan", "choose_billing_plan"}) {
        EXPECT_EQ(rig.registry.find(retired), nullptr) << retired;
    }

    const auto published = rig.call("set_price_list", rig.price_list());
    ASSERT_TRUE(published.ok) << published.error.message;
    const std::string now = month_name(first_of(this_month()));
    EXPECT_EQ(published.value["effective_from"], now) << "this month by default";
    EXPECT_EQ(published.value["currency"], "USD");
    ASSERT_EQ(published.value["prices"].size(), 3U);

    // Anyone signed in reads it; every account is billed at it with nothing to choose.
    const auto read = rig.call("price_list", json::object(), owner);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["period"], now);
    EXPECT_EQ(read.value["list"]["id"], published.value["id"]);
    EXPECT_EQ(read.value["list"]["prices"][0]["dimension"], "calls");
    EXPECT_EQ(read.value["list"]["prices"][0]["unit"], "calls");
    EXPECT_FALSE(read.value["list"]["prices"][0].contains("included"));
    EXPECT_TRUE(read.value["next"].is_null());
    EXPECT_FALSE(read.value.contains("history")) << "history is the operator's";
    const auto account = rig.call("billing_account", {{"customer_id", rig.customer}}, owner);
    ASSERT_TRUE(account.ok) << account.error.message;
    EXPECT_EQ(account.value["price_list"]["id"], published.value["id"]);
    EXPECT_EQ(account.value["provider"]["name"], "manual");
    EXPECT_FALSE(account.value.contains("current"));
    const auto preview = rig.call("bill_preview", {{"customer_id", rig.customer}}, owner);
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_EQ(preview.value["price_list"]["id"], published.value["id"]);
    EXPECT_EQ(preview.value["total_micros"], 0) << "no usage, no fee: nothing to pay";

    // A list for next month is announced as `next` and does not change this month.
    const std::string next = month_name(first_of(this_month() + std::chrono::months{1}));
    auto raised = rig.price_list(next);
    raised["prices"][0]["unit_micros"] = 1'500'000;
    const auto scheduled = rig.call("set_price_list", raised);
    ASSERT_TRUE(scheduled.ok) << scheduled.error.message;
    const auto later = rig.call("price_list", json::object(), owner);
    EXPECT_EQ(later.value["list"]["id"], published.value["id"]);
    EXPECT_EQ(later.value["next"]["id"], scheduled.value["id"]);
    EXPECT_EQ(rig.call("price_list", {{"period", next}}, owner).value["list"]["id"],
              scheduled.value["id"]);
    const auto history = rig.call("price_list", {{"include_history", true}});
    ASSERT_EQ(history.value["history"].size(), 2U);
    EXPECT_EQ(history.value["history"][0]["id"], scheduled.value["id"]) << "newest first";

    // A person of another account reads nothing of this one.
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

    // Every publication is in the central audit, refusals included.
    const auto events = lake::test::audit_events(*rig.store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    int set = 0;
    int refused = 0;
    for (const auto& event : *events) {
        if (event.draft.action != "set_price_list") {
            continue;
        }
        if (event.draft.outcome == "success") {
            ++set;
            EXPECT_EQ(event.draft.kind, "billing.price_list.set");
        } else {
            ++refused;
        }
    }
    EXPECT_EQ(set, 2);
    EXPECT_GE(refused, 1);
}

TEST(Billing, AMonthsBillIsWorkedOutFromTheUsageOfEveryWorkspace) {
    BillingRig rig;
    // The list has been in force since three months ago.
    rig.publish(3);
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
    EXPECT_EQ(bill.value["price_list"]["effective_from"],
              month_name(first_of(this_month() - std::chrono::months{3})));
    EXPECT_EQ(bill.value["lines"].size(), 3U) << "one line per rate, no fee";
    const json* calls = line(bill.value, "calls");
    ASSERT_NE(calls, nullptr);
    EXPECT_EQ((*calls)["quantity"], 3500);
    EXPECT_EQ((*calls)["billable"], 3500);
    EXPECT_EQ((*calls)["amount_micros"], 3'500'000);
    const json* storage = line(bill.value, "storage");
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ((*storage)["quantity"], 3 * gib);
    EXPECT_EQ((*storage)["amount_micros"], 300'000);
    const json* tokens = line(bill.value, "model_tokens");
    ASSERT_NE(tokens, nullptr);
    EXPECT_EQ((*tokens)["amount_micros"], 3'000'000);
    EXPECT_EQ(bill.value["total_micros"], 6'800'000);
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
    EXPECT_EQ(issued.value["total_micros"], 6'800'000);
    EXPECT_EQ(issued.value["lines"].size(), 3U);
    EXPECT_EQ(issued.value["price_list_id"], bill.value["price_list"]["id"]);
    const std::string invoice_id = issued.value["id"];

    // The lines are frozen: usage recorded later and a new list for the month do not change the
    // invoice, while a preview of the month now prices at the new list.
    rig.recorder.add(rig.home, "calls", 9000, {{"kind", "api"}}, day2);
    rig.flush();
    auto cheaper = rig.price_list(period);
    cheaper["prices"][0]["unit_micros"] = 500'000;
    ASSERT_TRUE(rig.call("set_price_list", cheaper).ok);
    const auto read =
        rig.call("get_invoice", {{"customer_id", rig.customer}, {"invoice_id", invoice_id}}, owner);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["total_micros"], 6'800'000);
    EXPECT_EQ((*line(read.value, "calls"))["unit_micros"], 1'000'000);
    const auto repriced =
        rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}}, owner);
    EXPECT_EQ((*line(repriced.value, "calls"))["unit_micros"], 500'000);
    EXPECT_EQ((*line(repriced.value, "calls"))["quantity"], 12'500);
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
    rig.publish(1);
    const auto last = this_month() - std::chrono::months{1};
    const std::string period = month_name(first_of(last));
    rig.recorder.add(rig.home,
                     "calls",
                     20'000,
                     {{"kind", "api"}},
                     std::chrono::sys_time<std::chrono::hours>{first_of(last)});
    rig.flush();
    const auto first =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(first.value["total_micros"], 20'000'000) << "20,000 calls at $1 per 1000";
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
    rig.publish(1);
    const auto last = this_month() - std::chrono::months{1};
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
    request_payment(const lake::api::PaymentAccount&,
                    const lake::control::Invoice& invoice) override {
        ++requests;
        if (refuse) {
            return std::unexpected(std::string("card declined"));
        }
        return lake::api::PaymentRequest{"fake-" + invoice.period, "Pay at the fake page.", {}, {}};
    }
    bool records_payments_by_hand() const override {
        return false;
    }
};

TEST(Billing, ThePaymentProviderIsAnInterface) {
    auto provider = std::make_shared<FakeProvider>();
    BillingRig rig(false);
    lake::api::register_billing_capabilities(rig.registry, provider);
    rig.publish(1);
    const auto last = this_month() - std::chrono::months{1};
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
        for (const char* name : {"price_list",
                                 "set_price_list",
                                 "billing_account",
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
