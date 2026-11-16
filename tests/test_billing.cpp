// Billing (lake#564, docs/api/billing.md): the price list (usage only, lake#635: one
// rate per dimension, no plans, no fee, nothing included), the bill worked out from usage read
// through usage_over_time, invoices with their frozen lines and payment state, the manual provider,
// and the switch that leaves billing absent on an enterprise install.

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    rig.recorder.sample(rig.home, "drive_files", gib, day1);
    rig.recorder.sample(rig.lab, "repositories", gib, day1);
    rig.recorder.sample(rig.home, "drive_files", 3 * gib, day2);
    rig.recorder.sample(rig.lab, "repositories", gib, day2);
    // What an engine holds on its own volume is not billed as storage (lake#663).
    rig.recorder.sample(rig.home, "workspace_data", 10 * gib, day1);
    rig.recorder.sample(rig.lab, "models", 10 * gib, day2);
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
    // Only the operator resends an invoice (lake#857), and the manual provider has nothing to send.
    EXPECT_EQ(rig.call("resend_invoice", {{"invoice_id", invoice_id}}, owner).error.code,
              "unauthorized");
    EXPECT_EQ(rig.call("resend_invoice", {{"invoice_id", invoice_id}}).error.code, "conflict");

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

// lake#857: an account's owners and admins (and the operator) set its billing contact; its members
// read it and cannot change it. Until one is saved, billing_account suggests the signed-in owner's
// email and stores nothing. The audit names the fields set, never their values.
TEST(Billing, OwnersAndAdminsSetTheBillingContact) {
    BillingRig rig;
    lake::control::InMemoryStore& store = *rig.store;
    const auto organization =
        store.create_organization({"taap:1:ed25519:billing-org", "Billing Org"});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto account =
        store.create_customer({"organization", "", organization->id, "billing-org"});
    ASSERT_TRUE(account) << account.error().message;
    const auto person = [&](const std::string& role) {
        const auto user = store.create_user(
            {"taap:1:ed25519:billing-org-" + role, role + "@org.example.test", role});
        EXPECT_TRUE(user) << user.error().message;
        EXPECT_TRUE(store.create_membership({organization->id,
                                             {},
                                             "user",
                                             user->id,
                                             user->taap_id,
                                             role,
                                             "sha512-256:" + role,
                                             "admin",
                                             "request-" + role}));
        auto context = rig.owner_context();
        context.acting_identity = user->taap_id;
        context.acting_user_id = user->id;
        return context;
    };
    auto owner = person("owner");
    auto admin = person("admin");
    auto member = person("member");
    const json of_account{{"customer_id", account->id}};

    // No contact yet: the owner is offered their own email, the member nothing, the operator
    // (no user) nothing; nothing is stored.
    const auto offered = rig.call("billing_account", of_account, owner);
    ASSERT_TRUE(offered.ok) << offered.error.message;
    EXPECT_TRUE(offered.value["contact"].is_null());
    EXPECT_EQ(offered.value["suggested_email"], "owner@org.example.test");
    const auto read = rig.call("billing_account", of_account, member);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_TRUE(read.value["suggested_email"].is_null());
    EXPECT_TRUE(rig.call("billing_account", of_account).value["suggested_email"].is_null());
    EXPECT_FALSE(*store.billing_contact(account->id));

    // A member cannot set it.
    json contact{{"customer_id", account->id},
                 {"email", " billing@org.example.test "},
                 {"name", "Billing Org GmbH"},
                 {"line1", "Hauptstrasse 1"},
                 {"city", "Berlin"},
                 {"postal_code", "10115"},
                 {"country", "DE"}};
    EXPECT_EQ(rig.call("set_billing_contact", contact, member).error.code, "unauthorized");
    EXPECT_FALSE(*store.billing_contact(account->id));

    // The owner sets it, trimmed; the manual provider has no customer to update.
    const auto set = rig.call("set_billing_contact", contact, owner);
    ASSERT_TRUE(set.ok) << set.error.message;
    EXPECT_EQ(set.value["contact"]["email"], "billing@org.example.test");
    EXPECT_EQ(set.value["contact"]["line2"], "");
    EXPECT_EQ(set.value["provider_update"], "none");
    const auto shown = rig.call("billing_account", of_account, member);
    ASSERT_TRUE(shown.ok) << shown.error.message;
    EXPECT_EQ(shown.value["contact"]["name"], "Billing Org GmbH");
    EXPECT_EQ(shown.value["contact"]["country"], "DE");
    EXPECT_TRUE(rig.call("billing_account", of_account, owner).value["suggested_email"].is_null())
        << "a stored contact is not suggested over";

    // An admin replaces it whole, and the operator may too.
    const auto replaced = rig.call("set_billing_contact",
                                   {{"customer_id", account->id}, {"email", "ap@org.example.test"}},
                                   admin);
    ASSERT_TRUE(replaced.ok) << replaced.error.message;
    EXPECT_EQ(replaced.value["contact"]["name"], "") << "a field left out is emptied";
    EXPECT_EQ(rig.call("set_billing_contact", contact).ok, true);

    // What PostgreSQL would refuse is invalid_input on the field.
    const std::vector<std::pair<std::string, std::string>> refusals{
        {"email", "no-at-sign"},
        {"email", "two@at@example.test"},
        {"country", "de"},
        {"country", "DEU"},
        {"postal_code", std::string(33, '1')},
        {"name", std::string(257, 'n')}};
    for (const auto& [field, value] : refusals) {
        auto bad = contact;
        bad[field] = value;
        const auto refused = rig.call("set_billing_contact", bad, owner);
        EXPECT_EQ(refused.error.code, "invalid_input") << field << "=" << value;
        EXPECT_EQ(refused.error.path, "/" + field) << field << "=" << value;
    }
    EXPECT_EQ(store.billing_contact(account->id)->value().name, "Billing Org GmbH")
        << "a refused contact changes nothing";

    // The audit names the fields that were set, never their values.
    const auto events = lake::test::audit_events(store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    int recorded = 0;
    for (const auto& event : *events) {
        if (event.draft.action != "set_billing_contact" || event.draft.outcome != "success") {
            continue;
        }
        ++recorded;
        EXPECT_EQ(event.draft.kind, "billing.contact.set");
        EXPECT_EQ(event.draft.object, "customer/" + account->id + "/billing");
        const std::string details = event.draft.details.dump();
        for (const char* value : {"billing@org.example.test", "Hauptstrasse", "Berlin", "10115"}) {
            EXPECT_EQ(details.find(value), std::string::npos) << details;
        }
    }
    ASSERT_EQ(recorded, 3);
    bool listed = false;
    for (const auto& event : *events) {
        if (event.draft.action == "set_billing_contact" && event.draft.outcome == "success" &&
            event.draft.details["fields"] ==
                json::array({"city", "country", "email", "line1", "name", "postal_code"})) {
            listed = true;
        }
    }
    EXPECT_TRUE(listed);
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
                                 "set_billing_contact",
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

namespace {

const json* class_line(const json& bill, const std::string& dimension, const std::string& name) {
    for (const json& entry : bill["lines"]) {
        if (entry["dimension"] == dimension && entry["class"] == name) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

// lake#643: compute is billed as CPU, RAM, GPU and GPU memory, each by the minute of its unit on
// the month's total: a vCPU-minute (60,000 mCPU-seconds), a GiB-minute (61,440 MiB-seconds), a
// GPU-minute.
TEST(BillLines, ComputeIsBilledByTheVcpuGibAndGpuMinute) {
    EXPECT_EQ(lake::api::billable_quantity("mcpu_seconds", 1), 60'000U);
    EXPECT_EQ(lake::api::billable_quantity("mcpu_seconds", 60'000), 60'000U);
    EXPECT_EQ(lake::api::billable_quantity("mcpu_seconds", 60'001), 120'000U);
    EXPECT_EQ(lake::api::billable_quantity("mib_seconds", 61'439), 61'440U);
    EXPECT_EQ(lake::api::billable_quantity("mib_seconds", 61'441), 122'880U);
    EXPECT_EQ(lake::api::billable_quantity("gpu_seconds", 61), 120U);

    // $0.0015 a vCPU-minute, $0.00017 a GiB-minute, $0.0108 an L4-minute, $0.07875 an
    // H100-minute, $0.03875 a minute of any other GPU, $0.00025 a GiB-minute of GPU memory.
    lake::api::PriceList list;
    list.prices = {{"cpu", 1'500, 60'000, 0},
                   {"ram", 170, 61'440, 0},
                   {"gpu", 38'750, 60, 0},
                   {"gpu", 10'800, 60, 0, "l4"},
                   {"gpu", 78'750, 60, 0, "h100"},
                   {"vram", 250, 61'440, 0}};
    lake::api::UsageQuantities usage;
    // Half a vCPU and 1 GiB for an hour; an L4 for an hour, an H100 for ten minutes and a GPU of
    // no known class for a minute; 24 GiB of GPU memory for an hour.
    usage.totals = {{"cpu", 500 * 3600},
                    {"ram", 1024 * 3600},
                    {"gpu", 3600 + 600 + 60},
                    {"vram", 24 * 1024 * 3600}};
    usage.by_class = {{"gpu", {{"l4", 3600}, {"h100", 600}, {"", 60}}},
                      {"vram", {{"l4", 24 * 1024 * 3600}}}};
    const auto lines = lake::api::bill_lines(list, usage);
    ASSERT_EQ(lines.size(), 6U);
    EXPECT_EQ(lines[0].amount_micros, 45'000U) << "30 vCPU-minutes";
    EXPECT_EQ(lines[1].amount_micros, 10'200U) << "60 GiB-minutes";
    EXPECT_EQ(lines[2].class_name, "");
    EXPECT_EQ(lines[2].quantity, 60U) << "the default rate bills what no class rate covers";
    EXPECT_EQ(lines[2].amount_micros, 38'750U);
    EXPECT_NE(lines[2].description.find("other classes"), std::string::npos);
    EXPECT_EQ(lines[3].class_name, "l4");
    EXPECT_EQ(lines[3].quantity, 3600U);
    EXPECT_EQ(lines[3].amount_micros, 648'000U) << "an L4-hour is $0.648";
    EXPECT_NE(lines[3].description.find("(L4)"), std::string::npos);
    EXPECT_EQ(lines[4].amount_micros, 787'500U) << "ten H100-minutes";
    EXPECT_EQ(lines[5].amount_micros, 360'000U) << "24 GiB of GPU memory for an hour is $0.36";

    // Without a default rate, a class without its own rate is not charged.
    list.prices = {{"gpu", 10'800, 60, 0, "l4"}};
    const auto only_l4 = lake::api::bill_lines(list, usage);
    ASSERT_EQ(only_l4.size(), 1U);
    EXPECT_EQ(only_l4[0].quantity, 3600U);
}

// lake#643: the price list rates CPU, RAM, GPU (one rate per class allowed) and GPU memory; build,
// run and instance time are measured but no longer priced. A list published before keeps its rates
// for them, and an invoice issued at it keeps its lines.
TEST(Billing, ComputeIsPricedAsCpuRamGpuAndGpuMemory) {
    BillingRig rig;
    const auto refused = [&](json prices) {
        return rig.call("set_price_list", {{"prices", std::move(prices)}}).error;
    };
    for (const char* retired : {"instances", "build_compute", "run_compute"}) {
        const auto error =
            refused(json::array({{{"dimension", retired}, {"unit_micros", 1000}, {"per", 60}}}));
        EXPECT_EQ(error.code, "invalid_input") << retired;
        EXPECT_EQ(error.path, "/prices/0/dimension") << retired;
    }
    EXPECT_EQ(
        refused(
            json::array(
                {{{"dimension", "cpu"}, {"class", "h100"}, {"unit_micros", 1}, {"per", 60000}}}))
            .path,
        "/prices/0/class")
        << "CPU has no classes";
    EXPECT_EQ(
        refused(
            json::array({{{"dimension", "gpu"}, {"class", "l4"}, {"unit_micros", 1}, {"per", 60}},
                         {{"dimension", "gpu"}, {"class", "l4"}, {"unit_micros", 2}, {"per", 60}}}))
            .code,
        "invalid_input");

    // An invoice for two months ago, issued at a list from before lake#643 that rated instances.
    const auto two_ago = this_month() - std::chrono::months{2};
    const std::string old_period = month_name(first_of(two_ago));
    lake::control::BillingPlan old;
    old.id = lake::api::price_list_id(old_period, 1'700'000'000'000'000);
    old.name = "Usage";
    old.currency = "USD";
    old.prices = {{"instances", 1'350, 60, 0}, {"calls", 1'000'000, 1000, 0}};
    ASSERT_TRUE(rig.store->put_billing_plan(old));
    const std::chrono::sys_time<std::chrono::hours> old_day{first_of(two_ago)};
    rig.recorder.add(rig.home, "instances", 3600, {{"kind", "zetta"}}, old_day);
    rig.flush();
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", old_period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const json* instances = line(issued.value, "instances");
    ASSERT_NE(instances, nullptr);
    EXPECT_EQ((*instances)["amount_micros"], 81'000);
    EXPECT_EQ((*instances)["class"], "");

    // The new list, in force since last month: CPU, RAM, GPU with an L4 rate, and GPU memory.
    const auto last = this_month() - std::chrono::months{1};
    const std::string period = month_name(first_of(last));
    const auto published = rig.call(
        "set_price_list",
        {{"effective_from", period},
         {"prices",
          json::array(
              {{{"dimension", "cpu"}, {"unit_micros", 1'500}, {"per", 60'000}},
               {{"dimension", "ram"}, {"unit_micros", 170}, {"per", 61'440}},
               {{"dimension", "gpu"}, {"unit_micros", 38'750}, {"per", 60}},
               {{"dimension", "gpu"}, {"class", "l4"}, {"unit_micros", 10'800}, {"per", 60}},
               {{"dimension", "vram"}, {"unit_micros", 250}, {"per", 61'440}}})}});
    ASSERT_TRUE(published.ok) << published.error.message;
    EXPECT_EQ(published.value["prices"].size(), 5U);
    const std::chrono::sys_time<std::chrono::hours> day{first_of(last)};
    // Two workspaces' compute: an hour of half a vCPU and 1 GiB in each, an L4-hour in home.
    for (const std::string& workspace : {rig.home, rig.lab}) {
        rig.recorder.add_together(workspace,
                                  {{"cpu", 500 * 3600, {{"kind", "workspace"}}},
                                   {"ram", 1024 * 3600, {{"kind", "workspace"}}}},
                                  day);
    }
    rig.recorder.add_together(rig.home,
                              {{"gpu", 3600, {{"kind", "app"}, {"class", "l4"}}},
                               {"vram", 24 * 1024 * 3600, {{"kind", "app"}, {"class", "l4"}}}},
                              day);
    rig.recorder.add(rig.home, "instances", 3600, {{"kind", "zetta"}}, day);
    rig.flush();
    auto owner = rig.owner_context();
    const auto bill =
        rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}}, owner);
    ASSERT_TRUE(bill.ok) << bill.error.message;
    EXPECT_EQ(bill.value["lines"].size(), 5U) << "instances are not priced any more";
    EXPECT_EQ((*line(bill.value, "cpu"))["amount_micros"], 90'000) << "60 vCPU-minutes";
    EXPECT_EQ((*line(bill.value, "ram"))["amount_micros"], 20'400) << "120 GiB-minutes";
    const json* l4 = class_line(bill.value, "gpu", "l4");
    ASSERT_NE(l4, nullptr);
    EXPECT_EQ((*l4)["quantity"], 3600);
    EXPECT_EQ((*l4)["amount_micros"], 648'000);
    const json* other_gpus = class_line(bill.value, "gpu", "");
    ASSERT_NE(other_gpus, nullptr);
    EXPECT_EQ((*other_gpus)["quantity"], 0);
    EXPECT_EQ((*line(bill.value, "vram"))["amount_micros"], 360'000);
    EXPECT_EQ(bill.value["total_micros"], 90'000 + 20'400 + 648'000 + 360'000);
    const auto issued_new =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", period}});
    ASSERT_TRUE(issued_new.ok) << issued_new.error.message;
    EXPECT_EQ((*class_line(issued_new.value, "gpu", "l4"))["unit_micros"], 10'800);

    // The old invoice is unchanged: its instances line, at its rate.
    const auto kept = rig.call(
        "get_invoice", {{"customer_id", rig.customer}, {"invoice_id", issued.value["id"]}}, owner);
    ASSERT_TRUE(kept.ok) << kept.error.message;
    EXPECT_EQ(kept.value["lines"], issued.value["lines"]);
    EXPECT_EQ(kept.value["total_micros"], issued.value["total_micros"]);
}

// lake#658, the metering contract: `transfer` is read from every source written to a workspace's
// `lake_usage`, not only from the repository records. Any caller of the usage recorder (an MCP
// connection's bytes, a drive download, one made up here) with its own labels is in
// usage_over_time, added to the repository's exact records, and on the month's bill.
TEST(UsageContract, TransferFromAnyRecorderCallerIsShownAndBilled) {
    BillingRig rig;
    auto list = rig.price_list(month_name(first_of(this_month() - std::chrono::months{2})));
    list["prices"].push_back({{"dimension", "transfer"}, {"unit_micros", 50'000}, {"per", gib}});
    ASSERT_TRUE(rig.call("set_price_list", list).ok);
    const auto last = this_month() - std::chrono::months{1};
    const std::chrono::sys_time<std::chrono::hours> day1{first_of(last)};
    // An arbitrary caller, with labels of its own, and the shared helper; two workspaces.
    rig.recorder.add(rig.home,
                     "transfer",
                     3 * gib,
                     {{"kind", "out"}, {"object_id", "mcp-connection-1"}, {"source", "mcp"}},
                     day1 + std::chrono::hours(2));
    rig.recorder.add_transfer(rig.lab, gib, 0, day1 + std::chrono::hours(30));
    rig.flush();

    const std::string period = month_name(first_of(last));
    auto owner = rig.owner_context();
    const auto read = rig.call("usage_over_time",
                               {{"customer_id", rig.customer},
                                {"from", period + "-01T00:00:00Z"},
                                {"to", month_name(first_of(this_month())) + "-01T00:00:00Z"},
                                {"granularity", "day"},
                                {"dimensions", json::array({"transfer"})}},
                               owner);
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_EQ(read.value["dimensions"].size(), 1U);
    const json& transfer = read.value["dimensions"][0];
    EXPECT_EQ(transfer["total"].get<double>(), 4.0 * gib);
    EXPECT_TRUE(transfer["billed"].get<bool>());
    EXPECT_EQ(transfer["by_workspace"].size(), 2U);

    const auto bill =
        rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}}, owner);
    ASSERT_TRUE(bill.ok) << bill.error.message;
    const json* billed = line(bill.value, "transfer");
    ASSERT_NE(billed, nullptr) << bill.value.dump();
    EXPECT_EQ((*billed)["quantity"], 4 * gib);
    EXPECT_EQ((*billed)["amount_micros"], 200'000) << "4 GiB at $0.05";
}

// The bill reads the model/direction classes instead of blending all model tokens.
TEST(Billing, ModelInputAndOutputHaveExactPricesAndCannotFallBackToTheBlendedRate) {
    BillingRig rig;
    const auto last = this_month() - std::chrono::months{1};
    const std::string period = month_name(first_of(last));
    const std::chrono::sys_time<std::chrono::hours> day{first_of(last)};
    const json prices = json::array(
        {{{"dimension", "model_tokens"}, {"unit_micros", 9'000'000}, {"per", 1'000'000}},
         {{"dimension", "model_tokens"},
          {"class", "gpt-5.5/input"},
          {"unit_micros", 6'000'000},
          {"per", 1'000'000}},
         {{"dimension", "model_tokens"},
          {"class", "gpt-5.5/output"},
          {"unit_micros", 36'000'000},
          {"per", 1'000'000}},
         {{"dimension", "model_tokens"},
          {"class", "text-embedding-3-small/input"},
          {"unit_micros", 24'000},
          {"per", 1'000'000}}});
    ASSERT_TRUE(rig.call("set_price_list", {{"effective_from", period}, {"prices", prices}}).ok);
    rig.recorder.add(rig.home, "model_tokens", 100, {{"class", "gpt-5.5/input"}}, day);
    rig.recorder.add(rig.lab, "model_tokens", 200, {{"class", "gpt-5.5/output"}}, day);
    rig.recorder.add(
        rig.home, "model_tokens", 1000, {{"class", "text-embedding-3-small/input"}}, day);
    // Unclassed usage from before this change retains the historical default tariff.
    rig.recorder.add(rig.home, "model_tokens", 10, {{"model", "legacy-model"}}, day);
    rig.flush();
    const json input{{"customer_id", rig.customer}, {"period", period}};
    const auto preview = rig.call("bill_preview", input);
    ASSERT_TRUE(preview.ok) << preview.error.message;
    ASSERT_NE(class_line(preview.value, "model_tokens", "gpt-5.5/input"), nullptr);
    EXPECT_EQ(class_line(preview.value, "model_tokens", "gpt-5.5/input")->at("amount_micros"), 600);
    EXPECT_EQ(class_line(preview.value, "model_tokens", "gpt-5.5/output")->at("amount_micros"),
              7200);
    EXPECT_EQ(class_line(preview.value, "model_tokens", "text-embedding-3-small/input")
                  ->at("amount_micros"),
              24);
    EXPECT_EQ(class_line(preview.value, "model_tokens", "")->at("quantity"), 10);
    const auto issued = rig.call("issue_invoice", input);
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["total_micros"], 7914);

    // A later list that omits a used class cannot bill it at the default or silently erase it.
    auto incomplete = prices;
    incomplete.erase(2);
    ASSERT_TRUE(
        rig.call("set_price_list", {{"effective_from", period}, {"prices", incomplete}}).ok);
    const auto refused = rig.call("bill_preview", input);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "conflict");
    ASSERT_TRUE(
        rig.call("set_price_list", {{"effective_from", period}, {"prices", json::array()}}).ok);
    const auto empty = rig.call("bill_preview", input);
    EXPECT_FALSE(empty.ok);
    EXPECT_EQ(empty.error.code, "conflict");
}

// LSR-121 (lake#988): deleting a workspace never takes its consumed usage off a bill that has not
// been issued. delete_workspace retains the usage before the cascade, the month's bill and
// usage_over_time still count it, values the recorder held are added later, a repeated delete
// writes the same rollup, and a deletion whose usage cannot be read is refused.
TEST(Billing, ADeletedWorkspacesUsageStaysOnTheUnissuedBill) {
    BillingRig rig;
    rig.publish(1);
    const auto now = std::chrono::system_clock::now();
    const std::string period = month_name(first_of(this_month()));
    rig.recorder.add(rig.home, "calls", 1000, {{"kind", "api"}}, now);
    rig.recorder.add(rig.lab, "calls", 2000, {{"kind", "api"}}, now);
    rig.flush();
    auto owner = rig.owner_context();
    const auto calls_billed = [&] {
        const auto bill =
            rig.call("bill_preview", {{"customer_id", rig.customer}, {"period", period}});
        EXPECT_TRUE(bill.ok) << bill.error.message;
        EXPECT_TRUE(bill.value["complete"]);
        const json* calls = line(bill.value, "calls");
        return calls == nullptr ? json(nullptr) : (*calls)["quantity"];
    };
    EXPECT_EQ(calls_billed(), 3000);

    // A deletion whose usage cannot be read is refused, and nothing is deleted.
    rig.workspaces.records(rig.lab).metric_unavailable_next = 10;
    const auto refused =
        rig.call("delete_workspace",
                 {{"customer_id", rig.customer}, {"workspace_id", rig.lab}, {"confirm", "lab"}},
                 owner);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "store_unavailable") << refused.error.message;
    EXPECT_EQ(rig.store->get_workspace(rig.lab)->desired, "running");
    rig.workspaces.records(rig.lab).metric_unavailable_next = 0;

    const auto deleted =
        rig.call("delete_workspace",
                 {{"customer_id", rig.customer}, {"workspace_id", rig.lab}, {"confirm", "lab"}},
                 owner);
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(rig.store->get_workspace(rig.lab)->desired, "deleted");
    const auto sets = rig.store->retained_usage_sets(rig.customer);
    ASSERT_TRUE(sets);
    ASSERT_EQ(sets->size(), 1U);
    EXPECT_EQ((*sets)[0].workspace_id, rig.lab);
    EXPECT_EQ(calls_billed(), 3000) << "the deleted workspace's calls stay on the bill";

    // A value the recorder still held for the deleted workspace is added, not dropped.
    rig.recorder.add(rig.lab, "calls", 500, {{"kind", "api"}}, now);
    rig.flush();
    EXPECT_EQ(rig.recorder.totals().dropped_workspace_deleted, 0U);
    EXPECT_EQ(calls_billed(), 3500);

    // The whole account's usage names the deleted workspace; a chosen one is live only.
    const auto usage = rig.call(
        "usage_over_time",
        {{"customer_id", rig.customer},
         {"from", period + "-01T00:00:00Z"},
         {"to", month_name(first_of(this_month() + std::chrono::months{1})) + "-01T00:00:00Z"},
         {"granularity", "month"},
         {"dimensions", json::array({"calls"})}},
        owner);
    ASSERT_TRUE(usage.ok) << usage.error.message;
    bool named = false;
    for (const json& workspace : usage.value["workspaces"]) {
        if (workspace["workspace_id"] == rig.lab) {
            named = true;
            EXPECT_TRUE(workspace["deleted"]);
            EXPECT_TRUE(workspace["measured"]);
        }
    }
    EXPECT_TRUE(named);
    EXPECT_EQ(usage.value["dimensions"][0]["total"], 3500);

    // Repeating the delete of a deleted workspace changes nothing on the bill.
    const auto again =
        rig.call("delete_workspace",
                 {{"customer_id", rig.customer}, {"workspace_id", rig.lab}, {"confirm", "lab"}},
                 owner);
    EXPECT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(calls_billed(), 3500);

    // What is retained is content-free: no object ids, only the labels billing reads.
    const auto points =
        rig.store->retained_usage(rig.customer, 0, std::numeric_limits<std::int64_t>::max());
    ASSERT_TRUE(points);
    for (const auto& point : *points) {
        EXPECT_TRUE(point.labels.size() <= 3U) << "only kind, class and model are kept";
        EXPECT_FALSE(point.labels.contains("object_id"));
    }
}

// The store keeps a rollup per workspace: a repeated rollup replaces it, late values add up, and
// a late value for a workspace with no rollup is refused (lake#988).
TEST(RetainedUsage, ARollupIsReplacedAndLateValuesAddUp) {
    lake::control::InMemoryStore store;
    const auto user = store.create_user({"taap:1:ed25519:retained", "r@example.test", "R"});
    ASSERT_TRUE(user);
    const auto customer = store.create_customer({"individual", user->id, "", "retained"});
    ASSERT_TRUE(customer);
    const auto workspace =
        store.create_workspace({customer->id, "gone", "dev", "taap:1:ed25519:retained"});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const lake::control::RetainedUsagePoint late{.workspace_id = workspace->id,
                                                 .source = "late",
                                                 .dimension = "calls",
                                                 .labels = {{"kind", "api"}},
                                                 .at_ms = 3'600'000,
                                                 .value = 5};
    EXPECT_EQ(store.add_retained_usage(late).error().code, "not_found");
    const lake::control::RetainedUsageSet set{.workspace_id = workspace->id,
                                              .customer_id = customer->id,
                                              .from_ms = 0,
                                              .to_ms = 86'400'000};
    lake::control::RetainedUsagePoint rolled{.workspace_id = workspace->id,
                                             .dimension = "calls",
                                             .labels = {{"kind", "api"}},
                                             .at_ms = 0,
                                             .value = 10};
    ASSERT_TRUE(store.retain_workspace_usage(set, {rolled}));
    ASSERT_TRUE(store.add_retained_usage(late));
    ASSERT_TRUE(store.add_retained_usage(late));
    rolled.value = 12;
    ASSERT_TRUE(store.retain_workspace_usage(set, {rolled}));
    const auto points = store.retained_usage(customer->id, 0, 86'400'000);
    ASSERT_TRUE(points);
    double rollup = 0, added = 0;
    for (const auto& point : *points) {
        (point.source == "rollup" ? rollup : added) += point.value;
    }
    EXPECT_EQ(rollup, 12) << "a repeated rollup replaces the first";
    EXPECT_EQ(added, 10) << "late values are kept and add up";
    const lake::control::RetainedUsageSet other{.workspace_id = workspace->id,
                                                .customer_id = lake::control::new_uuid_v4(),
                                                .from_ms = 0,
                                                .to_ms = 1};
    EXPECT_EQ(store.retain_workspace_usage(other, {}).error().code, "not_found")
        << "a workspace is retained only under its own account";
}
