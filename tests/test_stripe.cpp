// Stripe, lake's payment provider (decision 57, lake#620, docs/api/billing.md#stripe): lake's own
// REST client against a fake Stripe, the webhook signature and its tolerance window, notices
// applied once per event id, the keys read from lake's secret store with rotation, and the
// manual provider when they are absent.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "audit_test_support.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/api/stripe.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/memory_store.hpp"
#include "one_thread_io.hpp"
#include "workspace_fixture.hpp"

namespace {

using nlohmann::json;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";
constexpr std::string_view test_key = "sk_test_lake620fakekey0001";
constexpr std::string_view test_webhook = "whsec_lake620fakesecret0001";

std::string month_name(std::chrono::sys_days day) {
    return std::format("{:%Y-%m}", day);
}

std::chrono::year_month last_month() {
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return today.year() / today.month() - std::chrono::months{1};
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Stripe as far as lake uses it: customers, invoices, invoice items, finalize and void, with
// Stripe's idempotency (a repeated Idempotency-Key answers the first answer).
struct FakeStripe {
    std::mutex mutex;
    std::vector<lake::api::StripeRequest> requests;
    std::map<std::string, lake::api::StripeResponse> answered; // by Idempotency-Key
    int customers = 0;
    int invoices = 0;
    int items = 0;
    std::map<std::string, std::vector<std::int64_t>> amounts; // invoice -> item amounts
    std::map<std::string, std::string> status;                // invoice -> status
    bool fail_finalize = false;

    static std::string field(std::string_view body, std::string_view name) {
        const std::string key = std::string(name) + "=";
        std::size_t at = 0;
        while (at < body.size()) {
            const std::size_t end = body.find('&', at);
            const std::string_view item =
                body.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
            if (item.starts_with(key)) {
                return std::string(item.substr(key.size()));
            }
            if (end == std::string_view::npos) {
                break;
            }
            at = end + 1;
        }
        return {};
    }

    static std::string header(const lake::api::StripeRequest& request, std::string_view name) {
        for (const auto& [key, value] : request.headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

    lake::api::StripeResponse answer(const lake::api::StripeRequest& request) {
        std::lock_guard lock(mutex);
        requests.push_back(request);
        if (header(request, "Authorization") != "Bearer " + std::string(test_key)) {
            return {
                401, R"({"error":{"message":"Invalid API Key provided: sk_test_****0001"}})", {}};
        }
        const std::string key = header(request, "Idempotency-Key");
        if (const auto found = answered.find(key); !key.empty() && found != answered.end()) {
            return found->second;
        }
        lake::api::StripeResponse response = route(request);
        if (!key.empty() && response.status == 200) {
            answered[key] = response;
        }
        return response;
    }

    lake::api::StripeResponse route(const lake::api::StripeRequest& request) {
        const std::string path = request.url.substr(request.url.find("/v1/"));
        if (path == "/v1/customers") {
            return {200, json{{"id", std::format("cus_{}", ++customers)}}.dump(), {}};
        }
        if (path == "/v1/invoices") {
            const std::string id = std::format("in_{}", ++invoices);
            status[id] = "draft";
            return {200, json{{"id", id}, {"status", "draft"}}.dump(), {}};
        }
        if (path == "/v1/invoiceitems") {
            ++items;
            amounts[field(request.body, "invoice")].push_back(
                std::stoll(field(request.body, "amount")));
            return {200, json{{"id", std::format("ii_{}", items)}}.dump(), {}};
        }
        if (path.ends_with("/finalize")) {
            if (fail_finalize) {
                return {500, R"({"error":{"message":"try again"}})", {}};
            }
            const std::string id = path.substr(13, path.size() - 13 - 9);
            status[id] = "open";
            return {200,
                    json{{"id", id},
                         {"status", "open"},
                         {"hosted_invoice_url", "https://invoice.stripe.test/i/" + id}}
                        .dump(),
                    {}};
        }
        if (path.ends_with("/void")) {
            const std::string id = path.substr(13, path.size() - 13 - 5);
            status[id] = "void";
            return {200, json{{"id", id}, {"status", "void"}}.dump(), {}};
        }
        return {404, R"({"error":{"message":"no such route"}})", {}};
    }

    lake::api::StripeHttp http() {
        return [this](const lake::api::StripeRequest& request) { return answer(request); };
    }
};

// A Stripe webhook event for a Stripe invoice.
std::string event(const std::string& id, const std::string& type, const std::string& invoice) {
    return json{{"id", id},
                {"object", "event"},
                {"type", type},
                {"data", {{"object", {{"id", invoice}, {"object", "invoice"}}}}}}
        .dump();
}

// A registry with billing on over one account and two workspaces, a plan in effect last month,
// and a payment-provider source the test chooses.
struct StripeRig {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::string customer;
    std::string home;
    lake::api::UsageRecorder recorder{store.get(), workspaces.service.get()};
    lake::api::Registry registry;
    std::shared_ptr<lake::api::PaymentProviders> payments;
    std::string period = month_name(std::chrono::sys_days{last_month() / std::chrono::day{1}});

    explicit StripeRig(std::shared_ptr<lake::api::PaymentProviders> providers)
        : payments(std::move(providers)) {
        const auto user =
            store->create_user({"taap:1:ed25519:stripe-owner", "owner@example.test", "Owner"});
        EXPECT_TRUE(user) << user.error().message;
        const auto created = store->create_customer({"individual", user->id, "", "acme"});
        EXPECT_TRUE(created) << created.error().message;
        customer = created->id;
        home = workspaces.create(customer, "home");
        lake::api::register_core_capabilities(registry, "test");
        lake::api::register_billing_capabilities(registry, payments);
        EXPECT_TRUE(call("set_billing_plan",
                         {{"id", "team"},
                          {"name", "Team"},
                          {"currency", "USD"},
                          {"base_micros", 20'004'999},
                          {"prices",
                           json::array({{{"dimension", "calls"},
                                         {"unit_micros", 1'000'000},
                                         {"per", 1000},
                                         {"included", 0}}})}})
                        .ok);
        EXPECT_TRUE(store->set_billing_subscription(customer, period + "-01", "team"));
    }

    lake::api::ApiContext context() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.usage_recorder = &recorder;
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        context.registry = &registry;
        context.payments = payments;
        return context;
    }

    lake::api::ApiResult call(const std::string& name, json input) {
        auto ctx = context();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, ctx));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }

    // A notice as the route receives it: no credential, only the signature.
    lake::api::NoticeOutcome notice(const std::string& body, const std::string& signature) {
        auto ctx = context();
        ctx.authorized = false;
        ctx.operator_credential = false;
        ctx.acting_identity.clear();
        auto outcome = liquid::runtime::block_on(
            lake::api::receive_payment_notice(*payments, ctx, body, signature));
        EXPECT_TRUE(outcome);
        return std::move(*outcome);
    }

    lake::api::NoticeOutcome signed_notice(const std::string& body,
                                           std::string_view secret = test_webhook,
                                           std::int64_t at = unix_now()) {
        return notice(body, lake::api::stripe_signature_header(body, secret, at));
    }
};

std::shared_ptr<lake::api::PaymentProviders> stripe_providers(FakeStripe& stripe) {
    return lake::api::fixed_payment_providers(lake::api::stripe_payment_provider(
        {std::string(test_key), std::string(test_webhook)}, stripe.http(), "https://stripe.test"));
}

TEST(StripeSignature, ChecksTheHmacAndTheToleranceWindow) {
    const std::string body = event("evt_1", "invoice.paid", "in_1");
    const auto now = std::chrono::system_clock::now();
    const std::int64_t at = unix_now();
    const std::string good = lake::api::stripe_signature_header(body, test_webhook, at);
    EXPECT_TRUE(lake::api::verify_stripe_signature(body, good, test_webhook, now));
    // Stripe's documented header shape: more than one v1 (during a secret roll) and a v0.
    const std::string rolled = std::format(
        "t={},v1={},v1={},v0=ignored", at, std::string(64, '0'), good.substr(good.find("v1=") + 3));
    EXPECT_TRUE(lake::api::verify_stripe_signature(body, rolled, test_webhook, now));

    const auto refused = [&](std::string_view header,
                             std::string_view text,
                             std::string_view code,
                             std::string_view secret = test_webhook) {
        const auto checked = lake::api::verify_stripe_signature(text, header, secret, now);
        ASSERT_FALSE(checked) << header;
        EXPECT_EQ(checked.error().code, code) << header;
    };
    refused(good, body + " ", "invalid_signature");
    refused(good, body, "invalid_signature", "whsec_another_secret_0001");
    refused("", body, "invalid_signature");
    refused(std::format("t={}", at), body, "invalid_signature");
    refused(std::format("v1={}", good.substr(good.find("v1=") + 3)), body, "invalid_signature");
    refused("t=abc,v1=00", body, "invalid_signature");
    // A correctly signed event outside the five-minute window is a replay, either way.
    refused(
        lake::api::stripe_signature_header(body, test_webhook, at - 301), body, "stale_timestamp");
    refused(
        lake::api::stripe_signature_header(body, test_webhook, at + 301), body, "stale_timestamp");
    EXPECT_TRUE(lake::api::verify_stripe_signature(
        body, lake::api::stripe_signature_header(body, test_webhook, at - 299), test_webhook, now));
}

TEST(StripeClient, FormsCentsAndEvents) {
    EXPECT_EQ(lake::api::stripe_form({{"metadata[lake_period]", "2026-09"}, {"name", "A & B"}}),
              "metadata%5Blake_period%5D=2026-09&name=A%20%26%20B");
    EXPECT_EQ(lake::api::stripe_cents(0), 0);
    EXPECT_EQ(lake::api::stripe_cents(4'999), 0);
    EXPECT_EQ(lake::api::stripe_cents(5'000), 1) << "half a cent rounds up";
    EXPECT_EQ(lake::api::stripe_cents(20'000'000), 2000);
    EXPECT_TRUE(lake::api::stripe_secret_key_shape(test_key));
    EXPECT_TRUE(lake::api::stripe_secret_key_shape("rk_live_restricted0001"));
    EXPECT_FALSE(lake::api::stripe_secret_key_shape("pk_test_publishable0001"));
    EXPECT_TRUE(lake::api::stripe_webhook_secret_shape(test_webhook));
    EXPECT_FALSE(lake::api::stripe_webhook_secret_shape("sk_test_wrongkind0001"));

    const auto paid = lake::api::parse_stripe_event(event("evt_1", "invoice.paid", "in_9"));
    ASSERT_TRUE(paid);
    EXPECT_EQ(paid->reference, "in_9");
    EXPECT_EQ(paid->state, "paid");
    const auto failed =
        lake::api::parse_stripe_event(event("evt_2", "invoice.payment_failed", "in_9"));
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->state, "");
    EXPECT_EQ(failed->provider_status, "payment_failed");
    const auto voided = lake::api::parse_stripe_event(event("evt_3", "invoice.voided", "in_9"));
    ASSERT_TRUE(voided);
    EXPECT_EQ(voided->state, "void");
    const auto other = lake::api::parse_stripe_event(event("evt_4", "customer.created", "cus_1"));
    ASSERT_TRUE(other);
    EXPECT_TRUE(other->state.empty() && other->provider_status.empty());
    EXPECT_FALSE(lake::api::parse_stripe_event("not json"));
    EXPECT_FALSE(lake::api::parse_stripe_event(R"({"type":"invoice.paid"})"));
}

TEST(StripeBilling, AnInvoiceBecomesAStripeInvoiceWithItsLines) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const auto account = rig.call("billing_account", {{"customer_id", rig.customer}});
    ASSERT_TRUE(account.ok) << account.error.message;
    EXPECT_EQ(account.value["provider"]["name"], "stripe");
    EXPECT_EQ(account.value["provider"]["online_payment"], true);
    EXPECT_EQ(stripe.customers, 0) << "the Stripe customer is made at the first invoice";

    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["provider"], "stripe");
    EXPECT_EQ(issued.value["provider_reference"], "in_1");
    EXPECT_EQ(issued.value["payment_url"], "https://invoice.stripe.test/i/in_1");
    EXPECT_EQ(issued.value["provider_status"], "open");
    EXPECT_EQ(issued.value["state"], "due");
    EXPECT_EQ(issued.value["total_micros"], 20'004'999);
    // One customer, kept on the account's billing row.
    EXPECT_EQ(stripe.customers, 1);
    EXPECT_EQ(*rig.store->billing_provider_customer(rig.customer, "stripe"), "cus_1");
    // The plan line: $20.004999 is 2000 cents; lake's total rounds the same, so no rounding line.
    EXPECT_EQ(stripe.amounts["in_1"], std::vector<std::int64_t>{2000});
    EXPECT_EQ(stripe.status["in_1"], "open");

    // Every request is a form POST with lake's key, the pinned version and an Idempotency-Key.
    std::set<std::string> keys;
    for (const auto& request : stripe.requests) {
        EXPECT_EQ(request.method, "POST");
        EXPECT_TRUE(request.url.starts_with("https://stripe.test/v1/")) << request.url;
        EXPECT_EQ(FakeStripe::header(request, "Content-Type"), "application/x-www-form-urlencoded");
        EXPECT_EQ(FakeStripe::header(request, "Stripe-Version"), lake::api::stripe_api_version);
        const std::string key = FakeStripe::header(request, "Idempotency-Key");
        EXPECT_FALSE(key.empty()) << request.url;
        EXPECT_TRUE(keys.insert(key).second) << "one key per write: " << key;
    }
    const auto& create = stripe.requests[1];
    EXPECT_EQ(FakeStripe::field(create.body, "customer"), "cus_1");
    EXPECT_EQ(FakeStripe::field(create.body, "currency"), "usd");
    EXPECT_EQ(FakeStripe::field(create.body, "collection_method"), "send_invoice");
    EXPECT_EQ(FakeStripe::header(create, "Idempotency-Key"),
              "lake-invoice-" + rig.customer + "-" + rig.period + "-0");

    // Paid through Stripe, never by hand.
    EXPECT_EQ(rig.call("record_invoice_payment", {{"invoice_id", issued.value["id"]}}).error.code,
              "conflict");

    // Voiding in lake voids it in Stripe; the next issue of the month is a new Stripe invoice
    // (a new key family) for the same customer.
    const auto voided =
        rig.call("void_invoice", {{"invoice_id", issued.value["id"]}, {"note", "wrong plan"}});
    ASSERT_TRUE(voided.ok) << voided.error.message;
    EXPECT_EQ(stripe.status["in_1"], "void");
    const auto again =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["provider_reference"], "in_2");
    EXPECT_EQ(stripe.customers, 1) << "the kept customer is reused";
    EXPECT_EQ(FakeStripe::header(stripe.requests[stripe.requests.size() - 3], "Idempotency-Key"),
              "lake-invoice-" + rig.customer + "-" + rig.period + "-1");
}

TEST(StripeBilling, ARetryAfterAFailedStepNeverBillsTwice) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    rig.recorder.add(rig.home,
                     "calls",
                     2'505,
                     {{"kind", "api"}},
                     std::chrono::sys_time<std::chrono::hours>{
                         std::chrono::sys_days{last_month() / std::chrono::day{2}}});
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    stripe.fail_finalize = true;
    const auto failed =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    EXPECT_EQ(failed.error.code, "store_unavailable");
    EXPECT_NE(failed.error.message.find("try again"), std::string::npos) << failed.error.message;
    EXPECT_TRUE(rig.store->list_invoices(rig.customer)->empty()) << "nothing is issued";
    EXPECT_EQ(*rig.store->billing_provider_customer(rig.customer, "stripe"), "cus_1")
        << "the customer is kept before the invoice is asked for";

    stripe.fail_finalize = false;
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(stripe.customers, 1);
    EXPECT_EQ(stripe.invoices, 1) << "the retry's keys answer Stripe's first invoice";
    // The plan's 2000 cents and calls 2505 at $1 per 1000 ($2.505: 251 cents); lake's total,
    // $22.509999, is 2251 cents too, so there is no rounding line.
    EXPECT_EQ(stripe.amounts["in_1"], (std::vector<std::int64_t>{2000, 251}))
        << "items are kept by their keys too";
    EXPECT_EQ(issued.value["total_micros"], 22'509'999);
}

TEST(StripeBilling, StripesTotalIsLakesTotalToTheCent) {
    FakeStripe stripe;
    auto provider = lake::api::stripe_payment_provider(
        {std::string(test_key), std::string(test_webhook)}, stripe.http(), "https://stripe.test");
    lake::control::Invoice invoice;
    invoice.customer_id = "c";
    invoice.period = "2026-09";
    invoice.plan_id = "p";
    invoice.plan_name = "P";
    invoice.currency = "USD";
    // Two lines of half a cent each round to a cent each; lake's total is one cent.
    invoice.lines = {{"plan", "", "Plan", "month", 1, 0, 1, 5'000, 1, 5'000},
                     {"usage", "calls", "Calls", "calls", 1, 0, 1, 5'000, 1, 5'000}};
    invoice.total_micros = 10'000;
    const auto requested = provider->request_payment({"c", "acme", "cus_9", 0}, invoice);
    ASSERT_TRUE(requested) << requested.error();
    EXPECT_EQ(stripe.amounts["in_1"], (std::vector<std::int64_t>{1, 1, -1}));
    // USD only, and a customer first.
    invoice.currency = "EUR";
    EXPECT_FALSE(provider->request_payment({"c", "acme", "cus_9", 1}, invoice));
    invoice.currency = "USD";
    EXPECT_FALSE(provider->request_payment({"c", "acme", "", 1}, invoice));
    // A wrong key is refused without echoing it.
    auto wrong =
        lake::api::stripe_payment_provider({"sk_test_notthekey00000001", std::string(test_webhook)},
                                           stripe.http(),
                                           "https://stripe.test");
    const auto refused = wrong->provider_customer({"c", "acme", "", 0});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().find("sk_test"), std::string::npos) << refused.error();
}

TEST(StripeBilling, WebhooksMoveTheInvoiceOncePerEvent) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const std::string invoice_id = issued.value["id"];
    const auto read = [&] { return *rig.store->get_invoice(invoice_id); };

    // A bad signature and a stale one are refused, and nothing moves.
    const std::string paid = event("evt_paid", "invoice.paid", "in_1");
    const auto forged = rig.signed_notice(paid, "whsec_not_lakes_secret_01");
    EXPECT_EQ(forged.status, 400);
    EXPECT_EQ(forged.body["error"]["code"], "invalid_signature");
    const auto stale = rig.signed_notice(paid, test_webhook, unix_now() - 600);
    EXPECT_EQ(stale.status, 400);
    EXPECT_EQ(stale.body["error"]["code"], "stale_timestamp");
    EXPECT_EQ(rig.notice(paid, "").status, 400);
    EXPECT_EQ(read().state, "due");

    // A failed payment keeps the invoice due and says so.
    const auto failed = rig.signed_notice(event("evt_failed", "invoice.payment_failed", "in_1"));
    EXPECT_EQ(failed.status, 200);
    EXPECT_EQ(read().state, "due");
    EXPECT_EQ(read().provider_status, "payment_failed");

    // Paid: once. The same event again (Stripe's retry, or a replay inside the window) is a
    // duplicate that changes nothing.
    const auto first = rig.signed_notice(paid);
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(first.body["state"], "paid");
    EXPECT_EQ(first.body["changed"], true);
    EXPECT_EQ(read().state, "paid");
    EXPECT_EQ(read().provider_status, "paid");
    EXPECT_FALSE(read().paid_at.empty());
    const auto again = rig.signed_notice(paid);
    EXPECT_EQ(again.status, 200);
    EXPECT_EQ(again.body["duplicate"], true);

    // A late failure notice for a paid invoice changes nothing; a void notice cannot void a paid
    // invoice.
    EXPECT_EQ(rig.signed_notice(event("evt_failed_late", "invoice.payment_failed", "in_1"))
                  .body["changed"],
              false);
    EXPECT_EQ(rig.signed_notice(event("evt_void_late", "invoice.voided", "in_1")).body["changed"],
              false);
    EXPECT_EQ(read().state, "paid");
    EXPECT_EQ(read().provider_status, "paid");

    // Events lake does not act on, and invoices that are not lake's, are acknowledged.
    const auto other = rig.signed_notice(event("evt_other", "customer.created", "cus_1"));
    EXPECT_EQ(other.status, 200);
    EXPECT_EQ(other.body["ignored"], true);
    const auto foreign = rig.signed_notice(event("evt_foreign", "invoice.paid", "in_999"));
    EXPECT_EQ(foreign.status, 200);
    EXPECT_EQ(foreign.body["matched"], false);

    // Every notice that moved an invoice, and every refusal, is in the central audit.
    const auto events = lake::test::audit_events(*rig.store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    int paid_events = 0;
    int denied = 0;
    for (const auto& entry : *events) {
        if (entry.draft.action != "payment_notice") {
            continue;
        }
        if (entry.draft.kind == "billing.invoice.paid") {
            ++paid_events;
            EXPECT_EQ(entry.draft.object, "customer/" + rig.customer + "/billing");
            EXPECT_EQ(entry.draft.customer_id, rig.customer);
            EXPECT_EQ(entry.draft.details["event_id"], "evt_paid");
        }
        if (entry.draft.outcome == "denied") {
            ++denied;
        }
    }
    EXPECT_EQ(paid_events, 1);
    EXPECT_EQ(denied, 3);
}

TEST(StripeBilling, AVoidNoticeVoidsADueInvoice) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const auto voided = rig.signed_notice(event("evt_void", "invoice.voided", "in_1"));
    EXPECT_EQ(voided.status, 200);
    const auto invoice = *rig.store->get_invoice(issued.value["id"]);
    EXPECT_EQ(invoice.state, "void");
    EXPECT_EQ(invoice.provider_status, "void");
    EXPECT_EQ(invoice.note, "Voided in Stripe");
    // The month can be invoiced again.
    EXPECT_TRUE(
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}}).ok);
}

TEST(StripeBilling, TheManualProviderTakesNoNotices) {
    StripeRig rig(lake::api::fixed_payment_providers(nullptr, "no mount"));
    const auto refused = rig.signed_notice(event("evt_1", "invoice.paid", "in_1"));
    EXPECT_EQ(refused.status, 404);
    const auto account = rig.call("billing_account", {{"customer_id", rig.customer}});
    ASSERT_TRUE(account.ok);
    EXPECT_EQ(account.value["provider"]["name"], "manual");
    EXPECT_EQ(account.value["provider"]["online_payment"], false);
    EXPECT_EQ(account.value["provider"]["reason"], "no mount");
}

// A provider source the test can point elsewhere after the capabilities are registered.
struct SwappableProviders final : lake::api::PaymentProviders {
    std::shared_ptr<lake::api::PaymentProviders> target;
    liquid::runtime::task<
        std::expected<std::shared_ptr<lake::api::PaymentProvider>, lake::api::ApiError>>
    current(lake::api::ApiContext& context, bool refresh) override {
        auto got = co_await target->current(context, refresh);
        if (!got) {
            co_return std::expected<std::shared_ptr<lake::api::PaymentProvider>,
                                    lake::api::ApiError>(
                std::unexpected(lake::api::ApiError{"store_error", got.error().message, {}}));
        }
        co_return std::move(*got);
    }
    lake::api::PaymentStatus status() const override {
        return target->status();
    }
};

// The keys in lake's own secret store: a mount of the operator's account, read as the operator.
TEST(StripeBilling, KeysComeFromTheSecretStoreWithRotationAndFallBackToManual) {
    FakeStripe stripe;
    int built = 0;
    lake::api::SecretPaymentConfig config;
    config.instructions = "Pay by wire.";
    config.ttl = std::chrono::seconds(3600); // only a refresh reads again
    config.stripe =
        [&](const lake::api::StripeKeys& keys) -> std::shared_ptr<lake::api::PaymentProvider> {
        ++built;
        if (!lake::api::stripe_secret_key_shape(keys.secret_key) ||
            !lake::api::stripe_webhook_secret_shape(keys.webhook_secret)) {
            return nullptr;
        }
        return lake::api::stripe_payment_provider(keys, stripe.http(), "https://stripe.test");
    };
    // The mount does not exist yet: its id is configured before it is created.
    config.mount_id = lake::control::new_uuid_v4();
    auto missing = lake::api::secret_payment_providers(config);
    auto swappable = std::make_shared<SwappableProviders>();
    swappable->target = missing;
    StripeRig rig(swappable);
    auto ctx = rig.context();
    auto first = liquid::runtime::block_on(missing->current(ctx));
    ASSERT_TRUE(first && *first);
    EXPECT_EQ((**first)->name(), "manual");
    EXPECT_NE(missing->status().reason.find("does not exist"), std::string::npos)
        << missing->status().reason;
    const auto viewer = rig.call("viewer", json::object());
    ASSERT_TRUE(viewer.ok) << viewer.error.message;
    EXPECT_EQ(viewer.value["billing_payment"]["provider"], "manual");
    EXPECT_EQ(viewer.value["billing_payment"]["online_payment"], false);
    const auto info = rig.call("server_info", json::object());
    EXPECT_EQ(info.value["billing"], true);
    EXPECT_EQ(info.value["payment_provider"], "manual");
    EXPECT_EQ(info.value["online_payment"], false);

    // The operator makes the mount in a workspace of its own account and puts the secret.
    const auto mount =
        rig.call("create_secret_mount",
                 {{"customer_id", rig.customer}, {"workspace_id", rig.home}, {"name", "billing"}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    config.mount_id = mount.value["id"];
    auto providers = lake::api::secret_payment_providers(config);
    swappable->target = providers;
    ctx = rig.context();
    auto empty = liquid::runtime::block_on(providers->current(ctx));
    ASSERT_TRUE(empty && *empty);
    EXPECT_EQ((**empty)->name(), "manual");
    EXPECT_NE(providers->status().reason.find("no live `stripe` secret"), std::string::npos)
        << providers->status().reason;

    // Half the keys is still the manual provider.
    ASSERT_TRUE(rig.call("put_secret",
                         {{"mount_id", config.mount_id},
                          {"name", "stripe"},
                          {"value", {{"secret_key", std::string(test_key)}}}})
                    .ok);
    auto half = liquid::runtime::block_on(providers->current(ctx, true));
    ASSERT_TRUE(half && *half);
    EXPECT_EQ((**half)->name(), "manual");
    EXPECT_NE(providers->status().reason.find("webhook_secret"), std::string::npos);

    ASSERT_TRUE(rig.call("put_secret",
                         {{"mount_id", config.mount_id},
                          {"name", "stripe"},
                          {"value",
                           {{"secret_key", std::string(test_key)},
                            {"webhook_secret", std::string(test_webhook)}}}})
                    .ok);
    auto stripe_now = liquid::runtime::block_on(providers->current(ctx, true));
    ASSERT_TRUE(stripe_now && *stripe_now);
    EXPECT_EQ((**stripe_now)->name(), "stripe");
    EXPECT_EQ(providers->status().key_version, 2U);
    EXPECT_TRUE(providers->status().reason.empty());
    const int reads = built;
    // Cached: no read until the time to live passes or a refresh is asked for.
    auto cached = liquid::runtime::block_on(providers->current(ctx));
    ASSERT_TRUE(cached && *cached);
    EXPECT_EQ(built, reads);
    // The keys never appear in what lake says about the provider.
    const auto described = rig.call("billing_account", {{"customer_id", rig.customer}});
    ASSERT_TRUE(described.ok) << described.error.message;
    EXPECT_EQ(described.value["provider"]["name"], "stripe");
    EXPECT_EQ(described.value.dump().find(test_key), std::string::npos);
    EXPECT_EQ(rig.call("viewer", json::object()).value.dump().find(test_webhook),
              std::string::npos);

    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;

    // Rotation: a new webhook secret is put; a notice signed with it is checked again with the
    // newest keys and accepted, although the cached ones are still within their time to live.
    const std::string rotated = "whsec_lake620rotated000002";
    ASSERT_TRUE(
        rig.call("put_secret",
                 {{"mount_id", config.mount_id},
                  {"name", "stripe"},
                  {"value", {{"secret_key", std::string(test_key)}, {"webhook_secret", rotated}}}})
            .ok);
    const auto accepted = rig.signed_notice(event("evt_rotated", "invoice.paid", "in_1"), rotated);
    EXPECT_EQ(accepted.status, 200) << accepted.body.dump();
    EXPECT_EQ(rig.store->get_invoice(issued.value["id"])->state, "paid");
    EXPECT_EQ(providers->status().key_version, 3U);
    // The old secret no longer verifies.
    EXPECT_EQ(rig.signed_notice(event("evt_old", "invoice.paid", "in_1")).status, 400);

    // The secret removed: back to the manual provider, which says why.
    ASSERT_TRUE(rig.call("delete_secret_versions",
                         {{"mount_id", config.mount_id},
                          {"name", "stripe"},
                          {"versions", json::array({1, 2, 3})}})
                    .ok);
    auto removed = liquid::runtime::block_on(providers->current(ctx, true));
    ASSERT_TRUE(removed && *removed);
    EXPECT_EQ((**removed)->name(), "manual");
    EXPECT_FALSE(providers->status().reason.empty());
}

TEST(StripeBilling, TheWebhookRouteAnswersOnlyWhereTheLakeBills) {
    FakeStripe stripe;
    const auto post = [](int port, const std::string& body, const std::string& signature) {
        liquid::http1::request_options options;
        options.url = std::format("http://127.0.0.1:{}/billing/stripe/webhook", port);
        options.method = "POST";
        options.headers = {{"Content-Type", "application/json"}, {"Stripe-Signature", signature}};
        options.body.resize(body.size());
        std::memcpy(options.body.data(), body.data(), body.size());
        return lake::test::http_request(std::move(options));
    };
    const std::string body = event("evt_route", "invoice.paid", "in_404");
    for (const bool billing : {false, true}) {
        SCOPED_TRACE(billing ? "billing on" : "billing off");
        lake::api::ServerOptions options;
        options.port = 0;
        options.store = std::make_shared<lake::control::InMemoryStore>();
        options.admin_token = "stripe-route-operator";
        options.instance_kek = std::string(kek);
        options.billing = billing;
        options.payment_providers = stripe_providers(stripe);
        lake::api::ApiServer server(options);
        ASSERT_TRUE(server.start()) << server.reason();
        const auto good = post(server.port(),
                               body,
                               lake::api::stripe_signature_header(body, test_webhook, unix_now()));
        ASSERT_TRUE(good);
        EXPECT_EQ(good->status, billing ? 200U : 404U);
        const auto bad = post(server.port(), body, "t=1,v1=00");
        ASSERT_TRUE(bad);
        EXPECT_EQ(bad->status, billing ? 400U : 404U);
        server.stop();
    }
}

} // namespace
