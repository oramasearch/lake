// Stripe, lake's payment provider (decision 57, lake#620, docs/api/billing.md#stripe): lake's own
// REST client against a fake Stripe, the webhook signature and its tolerance window, notices
// applied once per event id, other apps' and other Lakes' events ignored in a shared account
// (lake#855), a kept customer Stripe no longer has replaced (lake#856), the keys read from the
// environment with rotation, and the manual provider when they are absent.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
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
// This Lake's public host, and another Lake's billing through the same Stripe account.
constexpr std::string_view this_lake = "shared.lake.test";
constexpr std::string_view other_lake = "shared-us.lake.test";

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

// Stripe as far as lake uses it: customers and their updates, invoices, invoice items, finalize,
// send and void, with Stripe's idempotency (a repeated Idempotency-Key answers the first successful
// answer), the products the operator created in the account (lake#855), and the customers it has
// (lake#856).
struct FakeStripe {
    std::mutex mutex;
    std::vector<lake::api::StripeRequest> requests;
    std::map<std::string, lake::api::StripeResponse> answered; // by Idempotency-Key
    int customers = 0;
    int invoices = 0;
    int items = 0;
    std::map<std::string, std::vector<std::int64_t>> amounts; // invoice -> item amounts
    std::map<std::string, std::vector<std::string>> products; // invoice -> item products ("": none)
    std::map<std::string, std::string> status;                // invoice -> status
    std::set<std::string> catalog{std::string(lake::api::stripe_usage_product)};
    // The customers this account has, and those deleted in the Dashboard, which Stripe still
    // reads as {id, deleted: true}. Any other cus_ id is missing (another account or mode).
    std::set<std::string> known_customers;
    std::set<std::string> deleted_customers;
    int customer_reads = 0;
    int customer_read_status = 0; // the status a customer read fails with; 0, none
    bool fail_finalize = false;
    bool fail_customer_update = false;
    int customer_updates = 0;
    // Invoices sent to their customer's email (lake#857), each send asked for, and what the test
    // checks while Stripe handles one.
    std::vector<std::string> sent;
    int sends = 0;
    bool fail_send = false;
    std::function<void(const std::string&)> on_send;

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
        if (request.method == "GET" && path.starts_with("/v1/customers/")) {
            ++customer_reads;
            if (customer_read_status != 0) {
                return {customer_read_status, R"({"error":{"message":"try again"}})", {}};
            }
            const std::string id = path.substr(std::string_view("/v1/customers/").size());
            if (deleted_customers.contains(id)) {
                return {
                    200, json{{"id", id}, {"object", "customer"}, {"deleted", true}}.dump(), {}};
            }
            if (!known_customers.contains(id)) {
                return {404,
                        json{{"error",
                              {{"type", "invalid_request_error"},
                               {"code", "resource_missing"},
                               {"param", "id"},
                               {"message", "No such customer: '" + id + "'"}}}}
                            .dump(),
                        {}};
            }
            return {200, json{{"id", id}, {"object", "customer"}}.dump(), {}};
        }
        if (path == "/v1/customers") {
            const std::string id = std::format("cus_{}", ++customers);
            known_customers.insert(id);
            return {200, json{{"id", id}}.dump(), {}};
        }
        if (path.starts_with("/v1/customers/")) {
            if (fail_customer_update) {
                return {400, R"({"error":{"message":"Invalid address: no such postal code"}})", {}};
            }
            ++customer_updates;
            return {200, json{{"id", path.substr(14)}}.dump(), {}};
        }
        if (path == "/v1/invoices") {
            const std::string id = std::format("in_{}", ++invoices);
            status[id] = "draft";
            return {200, json{{"id", id}, {"status", "draft"}}.dump(), {}};
        }
        if (path == "/v1/invoiceitems") {
            // A bare amount, or an inline price of a product in the account, never both; an
            // inline price is never negative.
            const std::string product = field(request.body, "price_data%5Bproduct%5D");
            const std::string amount = field(request.body, "amount");
            const auto refused = [](std::string_view message) {
                return lake::api::StripeResponse{
                    400, json{{"error", {{"message", std::string(message)}}}}.dump(), {}};
            };
            std::int64_t cents = 0;
            if (product.empty()) {
                if (amount.empty()) {
                    return refused("Missing required param: amount");
                }
                cents = std::stoll(amount);
            } else {
                if (!amount.empty()) {
                    return refused("You may only specify one of these parameters: amount, "
                                   "price_data");
                }
                if (!catalog.contains(product)) {
                    return {400,
                            json{{"error",
                                  {{"type", "invalid_request_error"},
                                   {"code", "resource_missing"},
                                   {"param", "price_data[product]"},
                                   {"message", "No such product: '" + product + "'"}}}}
                                .dump(),
                            {}};
                }
                const std::string unit = field(request.body, "price_data%5Bunit_amount%5D");
                const std::string quantity = field(request.body, "quantity");
                if (unit.empty() || unit.starts_with('-') || quantity.empty()) {
                    return refused("Invalid non-negative integer");
                }
                cents = std::stoll(unit) * std::stoll(quantity);
            }
            ++items;
            amounts[field(request.body, "invoice")].push_back(cents);
            products[field(request.body, "invoice")].push_back(product);
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
        if (path.ends_with("/send")) {
            const std::string id = path.substr(13, path.size() - 13 - 5);
            ++sends;
            if (on_send) {
                on_send(id);
            }
            if (fail_send) {
                return {500, R"({"error":{"message":"the email could not be sent"}})", {}};
            }
            sent.push_back(id);
            return {200, json{{"id", id}, {"status", status[id]}}.dump(), {}};
        }
        return {404, R"({"error":{"message":"no such route"}})", {}};
    }

    lake::api::StripeHttp http() {
        return [this](const lake::api::StripeRequest& request) { return answer(request); };
    }
};

// A Stripe webhook event for a Stripe invoice whose metadata is `metadata`: by default an invoice
// lake issued (lake_customer_id), and with none, another app's.
std::string event(const std::string& id,
                  const std::string& type,
                  const std::string& invoice,
                  const json& metadata = {{"lake_customer_id", "c"}}) {
    return json{
        {"id", id},
        {"object", "event"},
        {"type", type},
        {"data", {{"object", {{"id", invoice}, {"object", "invoice"}, {"metadata", metadata}}}}}}
        .dump();
}

// A registry with billing on over one account and two workspaces, a price list in force last month
// (calls at $1 per 1000), 20,000 calls last month ($20.00), and a payment-provider source the test
// chooses.
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
        EXPECT_TRUE(
            call("set_price_list",
                 {{"currency", "USD"},
                  {"effective_from", period},
                  {"prices",
                   json::array(
                       {{{"dimension", "calls"}, {"unit_micros", 1'000'000}, {"per", 1000}}})}})
                .ok);
        recorder.add(home,
                     "calls",
                     20'000,
                     {{"kind", "api"}},
                     std::chrono::sys_time<std::chrono::hours>{
                         std::chrono::sys_days{last_month() / std::chrono::day{1}}});
        static_cast<void>(liquid::runtime::block_on(recorder.flush()));
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

// The central audit's events of a kept Stripe customer replaced (lake#856).
std::vector<lake::audit::Event> replacements(lake::control::Store& store) {
    const auto events = lake::test::audit_events(store, {.limit = 500});
    EXPECT_TRUE(events) << events.error().message;
    std::vector<lake::audit::Event> out;
    for (const auto& entry : events.value_or(std::vector<lake::audit::Event>{})) {
        if (entry.draft.kind == "billing.account.provider_customer_replaced") {
            out.push_back(entry);
        }
    }
    return out;
}

std::shared_ptr<lake::api::PaymentProviders> stripe_providers(FakeStripe& stripe) {
    return lake::api::fixed_payment_providers(
        lake::api::stripe_payment_provider({std::string(test_key), std::string(test_webhook)},
                                           std::string(this_lake),
                                           stripe.http(),
                                           "https://stripe.test"));
}

// LSR-196 (lake#1002): notices that do not authenticate are anyone's requests. However many arrive,
// they add at most one denied audit event a minute, never one each.
TEST(StripeNotices, UnauthenticatedNoticesDoNotEachAppendAnAuditEvent) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const std::string body = event("evt_unsigned", "invoice.paid", "in_1");
    for (int i = 0; i < 20; ++i) {
        const auto outcome = rig.notice(body, "t=1,v1=00");
        EXPECT_EQ(outcome.status, 400);
    }
    const auto events = lake::test::audit_events(*rig.store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    std::size_t denied = 0;
    for (const auto& entry : *events) {
        if (entry.draft.kind == "billing.payment.notice" && entry.draft.outcome == "denied") {
            ++denied;
        }
    }
    EXPECT_LE(denied, 1U);
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

    const auto parse = [](const std::string& body) {
        return lake::api::parse_stripe_event(body, this_lake);
    };
    const auto paid = parse(event("evt_1", "invoice.paid", "in_9"));
    ASSERT_TRUE(paid);
    EXPECT_EQ(paid->reference, "in_9");
    EXPECT_EQ(paid->state, "paid");
    EXPECT_FALSE(paid->foreign);
    const auto failed = parse(event("evt_2", "invoice.payment_failed", "in_9"));
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->state, "");
    EXPECT_EQ(failed->provider_status, "payment_failed");
    const auto voided = parse(event("evt_3", "invoice.voided", "in_9"));
    ASSERT_TRUE(voided);
    EXPECT_EQ(voided->state, "void");
    const auto other = parse(event("evt_4", "customer.created", "cus_1"));
    ASSERT_TRUE(other);
    EXPECT_TRUE(other->state.empty() && other->provider_status.empty());
    EXPECT_FALSE(parse("not json"));
    EXPECT_FALSE(parse(R"({"type":"invoice.paid"})"));

    // Marked paid in Stripe without a payment (lake#1033): the invoice stays due, recorded.
    json out_of_band = json::parse(event("evt_5", "invoice.paid", "in_9"));
    out_of_band["data"]["object"]["paid_out_of_band"] = true;
    const auto marked = parse(out_of_band.dump());
    ASSERT_TRUE(marked);
    EXPECT_EQ(marked->state, "");
    EXPECT_EQ(marked->provider_status, "paid_out_of_band");
    // Nor does it activate an order.
    json order = out_of_band;
    order["data"]["object"]["subscription_details"] = {{"metadata", {{"lake_order_id", "o-1"}}}};
    order["data"]["object"]["subscription"] = "sub_1";
    const auto order_notice = parse(order.dump());
    ASSERT_TRUE(order_notice);
    EXPECT_FALSE(order_notice->order_subscription.has_value());
    EXPECT_EQ(order_notice->provider_status, "paid_out_of_band");
}

// Whose invoice an event is about, in a Stripe account shared with Cloud v1 and other Lakes
// (lake#855): lake_customer_id marks an invoice lake issued, lake_instance which Lake issued it,
// and an invoice Stripe generated from a subscription carries the subscription's metadata in
// subscription_details, never in its own.
TEST(StripeClient, AnInvoiceThatIsNotThisLakesIsForeign) {
    const auto foreign = [](const json& object) {
        const json body{{"id", "evt_1"},
                        {"object", "event"},
                        {"type", "invoice.paid"},
                        {"created", 100},
                        {"data", {{"object", object}}}};
        const auto notice = lake::api::parse_stripe_event(body.dump(), this_lake);
        EXPECT_TRUE(notice);
        return notice && notice->foreign;
    };
    const auto invoice = [](const json& metadata) {
        return json{{"id", "in_1"}, {"object", "invoice"}, {"metadata", metadata}};
    };
    // This Lake's, tagged or from before lake_instance existed.
    EXPECT_FALSE(foreign(invoice({{"lake_customer_id", "c"}, {"lake_instance", this_lake}})));
    EXPECT_FALSE(foreign(invoice({{"lake_customer_id", "c"}})));
    // Cloud v1's (its bare keys, or nothing at all) and another Lake's.
    EXPECT_TRUE(foreign(invoice({{"team_id", "t"}, {"plan_id", "p"}})));
    EXPECT_TRUE(foreign(invoice(json::object())));
    EXPECT_TRUE(foreign({{"id", "in_1"}, {"object", "invoice"}}));
    EXPECT_TRUE(foreign(invoice({{"lake_customer_id", "c"}, {"lake_instance", other_lake}})));
    EXPECT_TRUE(foreign(invoice({{"lake_customer_id", 7}})));
    // An invoice of an order sheet subscription: this Lake's, another Lake's, or Cloud v1's.
    const auto generated = [](const json& subscription) {
        return json{{"id", "in_2"},
                    {"object", "invoice"},
                    {"customer", "cus_2"},
                    {"subscription", "sub_2"},
                    {"lines",
                     {{"has_more", false},
                      {"data",
                       json::array({{{"type", "subscription"},
                                     {"period", {{"start", 100}, {"end", 200}}}}})}}},
                    {"metadata", json::object()},
                    {"subscription_details", {{"metadata", subscription}}}};
    };
    EXPECT_FALSE(foreign(generated({{"lake_order_id", "o"}, {"lake_instance", this_lake}})));
    EXPECT_TRUE(foreign(generated({{"lake_order_id", "o"}, {"lake_instance", other_lake}})));
    EXPECT_TRUE(foreign(generated({{"team_id", "t"}})));
    EXPECT_TRUE(
        foreign({{"id", "in_2"}, {"object", "invoice"}, {"subscription_details", nullptr}}));
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
    EXPECT_EQ(issued.value["total_micros"], 20'000'000);
    // One customer, kept on the account's billing row.
    EXPECT_EQ(stripe.customers, 1);
    EXPECT_EQ(*rig.store->billing_provider_customer(rig.customer, "stripe"), "cus_1");
    // The calls line: $20.00 is 2000 cents, of Lake's usage product; lake's total is the same, so
    // no rounding line.
    EXPECT_EQ(stripe.amounts["in_1"], std::vector<std::int64_t>{2000});
    EXPECT_EQ(stripe.products["in_1"], std::vector<std::string>{"lake_usage"});
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
    EXPECT_EQ(FakeStripe::field(create.body, "description"), "Lake%20usage%2C%20" + rig.period);
    // The line names Lake's usage product with an inline price, and is tagged like the invoice.
    const auto& item = stripe.requests[2];
    ASSERT_TRUE(item.url.ends_with("/v1/invoiceitems"));
    EXPECT_EQ(FakeStripe::header(item, "Idempotency-Key"),
              "lake-invoice-" + rig.customer + "-" + rig.period + "-0-line-0-p");
    EXPECT_EQ(FakeStripe::field(item.body, "customer"), "cus_1");
    EXPECT_EQ(FakeStripe::field(item.body, "invoice"), "in_1");
    EXPECT_EQ(FakeStripe::field(item.body, "quantity"), "1");
    EXPECT_EQ(FakeStripe::field(item.body, "price_data%5Bcurrency%5D"), "usd");
    EXPECT_EQ(FakeStripe::field(item.body, "price_data%5Bproduct%5D"), "lake_usage");
    EXPECT_EQ(FakeStripe::field(item.body, "price_data%5Bunit_amount%5D"), "2000");
    EXPECT_EQ(FakeStripe::field(item.body, "amount"), "");
    EXPECT_EQ(FakeStripe::field(item.body, "metadata%5Borama_app%5D"), "lake");
    EXPECT_EQ(FakeStripe::field(item.body, "metadata%5Blake_instance%5D"), this_lake);
    EXPECT_FALSE(FakeStripe::field(item.body, "description").empty());
    // The customer and the invoice are tagged as this Lake's (lake#855), beside lake's own keys,
    // and nothing lake sends carries Cloud v1's bare keys.
    const auto& customer = stripe.requests[0];
    ASSERT_TRUE(customer.url.ends_with("/v1/customers"));
    EXPECT_EQ(FakeStripe::field(customer.body, "name"), "acme") << "the account's name";
    EXPECT_EQ(FakeStripe::field(customer.body, "email"), "") << "no billing contact, no email";
    EXPECT_EQ(customer.body.find("address"), std::string::npos);
    for (const auto* request : {&customer, &create}) {
        EXPECT_EQ(FakeStripe::field(request->body, "metadata%5Blake_customer_id%5D"), rig.customer);
        EXPECT_EQ(FakeStripe::field(request->body, "metadata%5Borama_app%5D"), "lake");
        EXPECT_EQ(FakeStripe::field(request->body, "metadata%5Blake_instance%5D"), this_lake);
    }
    for (const auto& request : stripe.requests) {
        for (const char* bare : {"team_id", "plan_id", "addon_type", "addon_config"}) {
            EXPECT_EQ(request.body.find(std::format("%5B{}%5D", bare)), std::string::npos)
                << request.url << " sets " << bare;
        }
    }

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

// lake#857: the billing contact reaches the Stripe customer, when it is created at the first
// invoice and at every later save of the contact. A refused update keeps the contact lake stored.
TEST(StripeBilling, TheBillingContactReachesTheStripeCustomer) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const json contact{{"customer_id", rig.customer},
                       {"email", "ap@acme.test"},
                       {"name", "Acme GmbH"},
                       {"line1", "Hauptstrasse 1"},
                       {"city", "Berlin"},
                       {"postal_code", "10115"},
                       {"country", "DE"}};
    // No Stripe customer before the first invoice: the contact waits for it.
    const auto saved = rig.call("set_billing_contact", contact);
    ASSERT_TRUE(saved.ok) << saved.error.message;
    EXPECT_EQ(saved.value["provider_update"], "none");
    EXPECT_TRUE(stripe.requests.empty());

    // The customer is created with the contact's email, name and the address fields it has.
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const auto& created = stripe.requests[0];
    ASSERT_TRUE(created.url.ends_with("/v1/customers")) << created.url;
    EXPECT_EQ(FakeStripe::field(created.body, "email"), "ap%40acme.test");
    EXPECT_EQ(FakeStripe::field(created.body, "name"), "Acme%20GmbH");
    EXPECT_EQ(FakeStripe::field(created.body, "address%5Bline1%5D"), "Hauptstrasse%201");
    EXPECT_EQ(FakeStripe::field(created.body, "address%5Bcity%5D"), "Berlin");
    EXPECT_EQ(FakeStripe::field(created.body, "address%5Bpostal_code%5D"), "10115");
    EXPECT_EQ(FakeStripe::field(created.body, "address%5Bcountry%5D"), "DE");
    EXPECT_EQ(created.body.find("address%5Bline2%5D"), std::string::npos);
    EXPECT_EQ(created.body.find("address%5Bstate%5D"), std::string::npos);
    EXPECT_EQ(FakeStripe::field(created.body, "metadata%5Blake_customer_id%5D"), rig.customer);
    EXPECT_EQ(FakeStripe::field(created.body, "metadata%5Borama_app%5D"), "lake");

    // A later save updates that customer with every address field, so a removed one is cleared.
    auto moved = contact;
    moved["email"] = "billing@acme.test";
    moved["line1"] = "Neue Strasse 2";
    const auto updated = rig.call("set_billing_contact", moved);
    ASSERT_TRUE(updated.ok) << updated.error.message;
    EXPECT_EQ(updated.value["provider_update"], "updated");
    EXPECT_FALSE(updated.value.contains("provider_error"));
    EXPECT_EQ(stripe.customer_updates, 1);
    EXPECT_EQ(stripe.customers, 1) << "the kept customer is updated, never made again";
    const auto update = stripe.requests.back();
    EXPECT_EQ(update.url, "https://stripe.test/v1/customers/cus_1");
    EXPECT_EQ(FakeStripe::field(update.body, "email"), "billing%40acme.test");
    EXPECT_EQ(FakeStripe::field(update.body, "name"), "Acme%20GmbH");
    EXPECT_EQ(FakeStripe::field(update.body, "address%5Bline1%5D"), "Neue%20Strasse%202");
    EXPECT_NE(("&" + update.body + "&").find("&address%5Bline2%5D=&"), std::string::npos)
        << update.body;
    EXPECT_EQ(FakeStripe::header(update, "Idempotency-Key"),
              "lake-customer-" + rig.customer + "-contact-" +
                  updated.value["contact"]["updated_at"].get<std::string>());

    // With no name or address: the account's name, and the address cleared.
    const auto bare =
        rig.call("set_billing_contact", {{"customer_id", rig.customer}, {"email", "ap@acme.test"}});
    ASSERT_TRUE(bare.ok) << bare.error.message;
    EXPECT_EQ(bare.value["provider_update"], "updated");
    const auto cleared = stripe.requests.back();
    EXPECT_EQ(FakeStripe::field(cleared.body, "name"), "acme");
    EXPECT_NE(("&" + cleared.body + "&").find("&address=&"), std::string::npos) << cleared.body;
    EXPECT_EQ(cleared.body.find("address%5B"), std::string::npos);

    // Stripe refuses an update: the contact stays saved, and the refusal is reported.
    stripe.fail_customer_update = true;
    const auto refused = rig.call("set_billing_contact", contact);
    ASSERT_TRUE(refused.ok) << refused.error.message;
    EXPECT_EQ(refused.value["provider_update"], "failed");
    EXPECT_NE(refused.value["provider_error"].get<std::string>().find("no such postal code"),
              std::string::npos)
        << refused.value.dump();
    const auto account = rig.call("billing_account", {{"customer_id", rig.customer}});
    ASSERT_TRUE(account.ok) << account.error.message;
    EXPECT_EQ(account.value["contact"]["name"], "Acme GmbH");
    EXPECT_EQ(account.value["contact"]["line1"], "Hauptstrasse 1");
    EXPECT_EQ(rig.store->billing_contact(rig.customer)->value().email, "ap@acme.test");
}

// The central audit events of `kind` about the invoice `invoice_id`.
std::vector<lake::audit::Event>
invoice_events(lake::control::Store& store, std::string_view kind, const std::string& invoice_id) {
    std::vector<lake::audit::Event> found;
    const auto events = lake::test::audit_events(store, {.limit = 500});
    EXPECT_TRUE(events) << events.error().message;
    for (const auto& entry : events.value_or(std::vector<lake::audit::Event>{})) {
        if (entry.draft.kind == kind && entry.draft.details.value("invoice_id", "") == invoice_id) {
            found.push_back(entry);
        }
    }
    return found;
}

// lake#857: Stripe emails an invoice to the billing email only once lake has stored it, so a
// payment notice that arrives meanwhile finds the invoice it names. Without a billing contact
// nothing is sent.
TEST(StripeBilling, AnInvoiceIsSentToTheBillingEmailOnceLakeHasStoredIt) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const auto unsent =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(unsent.ok) << unsent.error.message;
    EXPECT_EQ(stripe.sends, 0) << "no billing email, nothing to send";
    EXPECT_EQ(unsent.value["provider_status"], "open");
    const std::string unsent_id = unsent.value["id"];
    ASSERT_EQ(invoice_events(*rig.store, "billing.invoice.issue", unsent_id).size(), 1U);
    EXPECT_EQ(
        invoice_events(*rig.store, "billing.invoice.issue", unsent_id)[0].draft.details["send"],
        "none");
    EXPECT_EQ(rig.call("resend_invoice", {{"invoice_id", unsent_id}}).error.code, "conflict");
    EXPECT_EQ(stripe.sends, 0);
    ASSERT_TRUE(
        rig.call("void_invoice", {{"invoice_id", unsent_id}, {"note", "no billing email"}}).ok);

    ASSERT_TRUE(
        rig.call("set_billing_contact", {{"customer_id", rig.customer}, {"email", "ap@acme.test"}})
            .ok);
    bool stored = false;
    stripe.on_send = [&](const std::string& id) {
        const auto invoices = rig.store->list_invoices(rig.customer);
        stored = invoices && std::ranges::any_of(*invoices, [&](const auto& invoice) {
                     return invoice.provider_reference == id && invoice.state == "due";
                 });
    };
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_TRUE(stored) << "sent before lake stored the invoice";
    EXPECT_EQ(stripe.sent, std::vector<std::string>{"in_2"});
    EXPECT_EQ(issued.value["state"], "due");
    EXPECT_EQ(issued.value["provider_status"], "open");
    // The last request, after the finalize, in the invoice's key family (the month's second issue).
    const auto& send = stripe.requests.back();
    EXPECT_EQ(send.url, "https://stripe.test/v1/invoices/in_2/send");
    EXPECT_EQ(FakeStripe::header(send, "Idempotency-Key"),
              "lake-invoice-" + rig.customer + "-" + rig.period + "-1-send");
    EXPECT_TRUE(stripe.requests[stripe.requests.size() - 2].url.ends_with("/in_2/finalize"));
    const auto events = invoice_events(*rig.store, "billing.invoice.issue", issued.value["id"]);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].draft.details["send"], "sent");
}

// lake#857: a send Stripe refuses leaves the invoice issued and payable on its page, send_failed
// and audited. resend_invoice sends it, each time with a key of its own, since Stripe answers a
// key's first answer again, a refusal included. A payment that lands during a send is kept.
TEST(StripeBilling, AFailedSendLeavesTheInvoiceIssuedAndResendSendsIt) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    ASSERT_TRUE(
        rig.call("set_billing_contact", {{"customer_id", rig.customer}, {"email", "ap@acme.test"}})
            .ok);
    stripe.fail_send = true;
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const std::string invoice_id = issued.value["id"];
    EXPECT_EQ(issued.value["state"], "due");
    EXPECT_EQ(issued.value["provider_status"], "send_failed");
    EXPECT_EQ(issued.value["payment_url"], "https://invoice.stripe.test/i/in_1");
    EXPECT_EQ(rig.store->get_invoice(invoice_id)->provider_status, "send_failed");
    EXPECT_EQ(stripe.sends, 1);
    const auto failed = invoice_events(*rig.store, "billing.invoice.send_failed", invoice_id);
    ASSERT_EQ(failed.size(), 1U);
    EXPECT_EQ(failed[0].draft.action, "issue_invoice");
    EXPECT_EQ(failed[0].draft.outcome, "failure");
    EXPECT_EQ(failed[0].draft.customer_id, rig.customer);
    EXPECT_NE(failed[0].draft.details["error"].get<std::string>().find("could not be sent"),
              std::string::npos);
    const auto issue = invoice_events(*rig.store, "billing.invoice.issue", invoice_id);
    ASSERT_EQ(issue.size(), 1U);
    EXPECT_EQ(issue[0].draft.outcome, "success");
    EXPECT_EQ(issue[0].draft.details["send"], "failed");

    // Refused again: still send_failed, and the refusal is the answer.
    const auto refused = rig.call("resend_invoice", {{"invoice_id", invoice_id}});
    EXPECT_EQ(refused.error.code, "store_unavailable");
    EXPECT_NE(refused.error.message.find("could not be sent"), std::string::npos)
        << refused.error.message;
    EXPECT_EQ(rig.store->get_invoice(invoice_id)->provider_status, "send_failed");
    EXPECT_EQ(invoice_events(*rig.store, "billing.invoice.send_failed", invoice_id).size(), 2U);

    // Sent: back to Stripe's status, with no new customer or invoice.
    stripe.fail_send = false;
    const auto resent = rig.call("resend_invoice", {{"invoice_id", invoice_id}});
    ASSERT_TRUE(resent.ok) << resent.error.message;
    EXPECT_EQ(resent.value["provider_status"], "open");
    EXPECT_EQ(resent.value["state"], "due");
    EXPECT_EQ(stripe.sent, std::vector<std::string>{"in_1"});
    EXPECT_EQ(stripe.customers, 1);
    EXPECT_EQ(stripe.invoices, 1);
    EXPECT_EQ(invoice_events(*rig.store, "billing.invoice.send", invoice_id).size(), 1U);
    std::vector<std::string> keys;
    for (const auto& request : stripe.requests) {
        if (request.url.ends_with("/send")) {
            keys.push_back(FakeStripe::header(request, "Idempotency-Key"));
        }
    }
    const std::string family = "lake-invoice-" + rig.customer + "-" + rig.period + "-0-send";
    ASSERT_EQ(keys.size(), 3U);
    EXPECT_EQ(keys[0], family);
    EXPECT_TRUE(keys[1].starts_with(family + "-")) << keys[1];
    EXPECT_TRUE(keys[2].starts_with(family + "-")) << keys[2];
    EXPECT_NE(keys[1], keys[2]);

    // A reminder after a failed payment keeps saying the payment failed: a send changes only its
    // own status.
    ASSERT_TRUE(rig.store->apply_payment_event(
        {"stripe", "evt_failed", "invoice.payment_failed", "in_1", "", "payment_failed"}));
    const auto reminded = rig.call("resend_invoice", {{"invoice_id", invoice_id}});
    ASSERT_TRUE(reminded.ok) << reminded.error.message;
    EXPECT_EQ(reminded.value["provider_status"], "payment_failed");
    EXPECT_EQ(stripe.sent, (std::vector<std::string>{"in_1", "in_1"}));

    // Paid while a send fails: the payment stays, never overwritten by send_failed.
    stripe.fail_send = true;
    stripe.on_send = [&](const std::string& id) {
        ASSERT_TRUE(rig.store->apply_payment_event(
            {"stripe", "evt_paid_during_send", "invoice.paid", id, "paid", "paid"}));
    };
    EXPECT_EQ(rig.call("resend_invoice", {{"invoice_id", invoice_id}}).error.code,
              "store_unavailable");
    const auto paid = *rig.store->get_invoice(invoice_id);
    EXPECT_EQ(paid.state, "paid");
    EXPECT_EQ(paid.provider_status, "paid");
    stripe.on_send = nullptr;
    EXPECT_EQ(rig.call("resend_invoice", {{"invoice_id", invoice_id}}).error.code, "conflict")
        << "a paid invoice has nothing to send";
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
    // 22,505 calls at $1 per 1000 ($22.505: 2251 cents, half up); lake's total is the same cents,
    // so there is no rounding line.
    EXPECT_EQ(stripe.amounts["in_1"], (std::vector<std::int64_t>{2251}))
        << "items are kept by their keys too";
    EXPECT_EQ(issued.value["total_micros"], 22'505'000);
}

TEST(StripeBilling, StripesTotalIsLakesTotalToTheCent) {
    FakeStripe stripe;
    auto provider =
        lake::api::stripe_payment_provider({std::string(test_key), std::string(test_webhook)},
                                           std::string(this_lake),
                                           stripe.http(),
                                           "https://stripe.test");
    lake::control::Invoice invoice;
    invoice.customer_id = "c";
    invoice.period = "2026-09";
    invoice.plan_id = "usage-202609-00000000000000001";
    invoice.plan_name = "Usage";
    invoice.currency = "USD";
    // Two lines of half a cent each round to a cent each; lake's total is one cent.
    invoice.lines = {{"usage", "predictions", "Predictions", "calls", 1, 0, 1, 5'000, 1, 5'000},
                     {"usage", "calls", "Calls", "calls", 1, 0, 1, 5'000, 1, 5'000}};
    invoice.total_micros = 10'000;
    const auto requested = provider->request_payment({"c", "acme", "cus_9", 0}, invoice);
    ASSERT_TRUE(requested) << requested.error();
    EXPECT_EQ(stripe.amounts["in_1"], (std::vector<std::int64_t>{1, 1, -1}));
    // The negative rounding line keeps the bare amount, since an inline price cannot be negative.
    EXPECT_EQ(stripe.products["in_1"], (std::vector<std::string>{"lake_usage", "lake_usage", ""}));
    const auto& rounding = stripe.requests[stripe.requests.size() - 2]; // before the finalize
    ASSERT_TRUE(rounding.url.ends_with("/v1/invoiceitems"));
    EXPECT_EQ(FakeStripe::header(rounding, "Idempotency-Key"), "lake-invoice-c-2026-09-0-rounding");
    EXPECT_EQ(FakeStripe::field(rounding.body, "amount"), "-1");
    EXPECT_EQ(FakeStripe::field(rounding.body, "currency"), "usd");
    EXPECT_EQ(FakeStripe::field(rounding.body, "description"), "Rounding%20to%20the%20cent");
    EXPECT_EQ(FakeStripe::field(rounding.body, "metadata%5Borama_app%5D"), "lake");
    // USD only, and a customer first.
    invoice.currency = "EUR";
    EXPECT_FALSE(provider->request_payment({"c", "acme", "cus_9", 1}, invoice));
    invoice.currency = "USD";
    EXPECT_FALSE(provider->request_payment({"c", "acme", "", 1}, invoice));
    // A wrong key is refused without echoing it.
    auto wrong =
        lake::api::stripe_payment_provider({"sk_test_notthekey00000001", std::string(test_webhook)},
                                           std::string(this_lake),
                                           stripe.http(),
                                           "https://stripe.test");
    const auto refused = wrong->provider_customer({"c", "acme", "", 0});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().find("sk_test"), std::string::npos) << refused.error();
    // Three lines under half a cent bill nothing each; lake's total is one cent, a positive
    // rounding line of Lake's usage product.
    invoice.lines = {{"usage", "predictions", "Predictions", "calls", 1, 0, 1, 4'000, 1, 4'000},
                     {"usage", "calls", "Calls", "calls", 1, 0, 1, 4'000, 1, 4'000},
                     {"usage", "searches", "Searches", "calls", 1, 0, 1, 4'000, 1, 4'000}};
    invoice.total_micros = 12'000;
    ASSERT_TRUE(provider->request_payment({"c", "acme", "cus_9", 2}, invoice));
    EXPECT_EQ(stripe.amounts["in_2"], (std::vector<std::int64_t>{1}));
    EXPECT_EQ(stripe.products["in_2"], (std::vector<std::string>{"lake_usage"}));
}

// Lake's products are the operator's to create in each account (lake#855): until it has, an issue
// fails saying so, nothing is issued in lake, and the same issue goes through once it exists.
TEST(StripeBilling, AMissingProductSaysTheOperatorCreatesIt) {
    FakeStripe stripe;
    stripe.catalog.clear();
    StripeRig rig(stripe_providers(stripe));
    const auto failed =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    EXPECT_EQ(failed.error.code, "store_unavailable");
    EXPECT_NE(failed.error.message.find("Stripe has no product lake_usage in this account; the "
                                        "operator creates it (spec.payments)"),
              std::string::npos)
        << failed.error.message;
    EXPECT_NE(failed.error.message.find("24 hours"), std::string::npos) << failed.error.message;
    EXPECT_TRUE(rig.store->list_invoices(rig.customer)->empty()) << "nothing is issued";
    EXPECT_EQ(stripe.items, 0);
    EXPECT_EQ(stripe.status["in_1"], "draft") << "the Stripe invoice is left as it was";

    // Any other refusal keeps Stripe's own message.
    auto other = lake::api::stripe_payment_provider(
        {std::string(test_key), std::string(test_webhook)},
        std::string(this_lake),
        [](const lake::api::StripeRequest&) {
            return lake::api::StripeResponse{
                400,
                R"({"error":{"code":"resource_missing","param":"customer","message":"No such customer"}})",
                {}};
        },
        "https://stripe.test");
    const auto refused = other->provider_customer({"c", "acme", "", 0});
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("No such customer"), std::string::npos) << refused.error();

    // The operator creates the product; issuing again uses the same Stripe invoice.
    stripe.catalog.insert(std::string(lake::api::stripe_usage_product));
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(issued.value["provider_reference"], "in_1");
    EXPECT_EQ(stripe.invoices, 1);
    EXPECT_EQ(stripe.amounts["in_1"], std::vector<std::int64_t>{2000});
}

// A kept customer is read before it is reused (lake#856): one GET with lake's key and version, and
// no customer created.
TEST(StripeBilling, AKeptCustomerStripeStillHasIsReusedAfterOneRead) {
    FakeStripe stripe;
    stripe.known_customers.insert("cus_kept");
    StripeRig rig(stripe_providers(stripe));
    ASSERT_TRUE(rig.store->set_billing_provider_customer(rig.customer, "stripe", "cus_kept"));
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(stripe.customer_reads, 1);
    EXPECT_EQ(stripe.customers, 0) << "the kept customer is reused";
    const auto& read = stripe.requests[0];
    EXPECT_EQ(read.method, "GET");
    EXPECT_EQ(read.url, "https://stripe.test/v1/customers/cus_kept");
    EXPECT_TRUE(read.body.empty());
    EXPECT_EQ(FakeStripe::header(read, "Authorization"), "Bearer " + std::string(test_key));
    EXPECT_EQ(FakeStripe::header(read, "Stripe-Version"), lake::api::stripe_api_version);
    EXPECT_EQ(FakeStripe::header(read, "Idempotency-Key"), "") << "a read takes no key";
    EXPECT_EQ(FakeStripe::field(stripe.requests[1].body, "customer"), "cus_kept");
    EXPECT_EQ(*rig.store->billing_provider_customer(rig.customer, "stripe"), "cus_kept");
    EXPECT_TRUE(replacements(*rig.store).empty());
}

// A kept customer Stripe does not have (404 resource_missing: a sandbox reset or replaced, a
// switch from test mode to live) or has deleted is replaced by a new one under its own key; the
// new id is kept, the invoice is the new customer's, and the replacement is audited (lake#856).
TEST(StripeBilling, AKeptCustomerStripeNoLongerHasIsReplaced) {
    for (const bool deleted : {false, true}) {
        SCOPED_TRACE(deleted ? "deleted" : "missing");
        FakeStripe stripe;
        if (deleted) {
            stripe.deleted_customers.insert("cus_old");
        }
        StripeRig rig(stripe_providers(stripe));
        ASSERT_TRUE(rig.store->set_billing_provider_customer(rig.customer, "stripe", "cus_old"));
        const auto issued =
            rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
        ASSERT_TRUE(issued.ok) << issued.error.message;
        EXPECT_EQ(stripe.customer_reads, 1);
        EXPECT_EQ(stripe.customers, 1);
        EXPECT_EQ(*rig.store->billing_provider_customer(rig.customer, "stripe"), "cus_1");
        // The plain key would replay the first create within Stripe's 24 hours.
        const auto& created = stripe.requests[1];
        ASSERT_TRUE(created.url.ends_with("/v1/customers"));
        EXPECT_EQ(FakeStripe::header(created, "Idempotency-Key"),
                  "lake-customer-" + rig.customer + "-replaces-cus_old");
        EXPECT_EQ(FakeStripe::field(created.body, "metadata%5Blake_customer_id%5D"), rig.customer);
        EXPECT_EQ(FakeStripe::field(created.body, "metadata%5Borama_app%5D"), "lake");
        EXPECT_EQ(FakeStripe::field(stripe.requests[2].body, "customer"), "cus_1");
        EXPECT_EQ(FakeStripe::field(stripe.requests[3].body, "customer"), "cus_1");
        EXPECT_EQ(issued.value["provider_reference"], "in_1");

        const auto replaced = replacements(*rig.store);
        ASSERT_EQ(replaced.size(), 1U);
        EXPECT_EQ(replaced[0].draft.customer_id, rig.customer);
        EXPECT_EQ(replaced[0].draft.object, "customer/" + rig.customer + "/billing");
        EXPECT_EQ(replaced[0].draft.principal, "admin");
        EXPECT_EQ(replaced[0].draft.outcome, "success");
        EXPECT_EQ(replaced[0].draft.details,
                  (json{{"provider", "stripe"},
                        {"old_provider_customer_id", "cus_old"},
                        {"new_provider_customer_id", "cus_1"}}));

        // The replacement is kept and read like any customer: the next issue reuses it.
        ASSERT_TRUE(
            rig.call("void_invoice", {{"invoice_id", issued.value["id"]}, {"note", "again"}}).ok);
        const auto again =
            rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
        ASSERT_TRUE(again.ok) << again.error.message;
        EXPECT_EQ(stripe.customer_reads, 2);
        EXPECT_EQ(stripe.customers, 1);
        EXPECT_EQ(replacements(*rig.store).size(), 1U);
    }
}

// A read that fails for any reason but the customer's absence fails the issue and keeps the kept
// id: a refused key, a rate limit or Stripe's own failure never replaces a customer (lake#856).
TEST(StripeBilling, AFailedCustomerReadKeepsTheKeptCustomer) {
    for (const int status : {500, 401, 403, 429}) {
        SCOPED_TRACE(status);
        FakeStripe stripe;
        stripe.known_customers.insert("cus_kept");
        stripe.customer_read_status = status;
        StripeRig rig(stripe_providers(stripe));
        ASSERT_TRUE(rig.store->set_billing_provider_customer(rig.customer, "stripe", "cus_kept"));
        const auto failed =
            rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
        EXPECT_EQ(failed.error.code, "store_unavailable");
        EXPECT_NE(failed.error.message.find("read the customer"), std::string::npos)
            << failed.error.message;
        EXPECT_EQ(stripe.customer_reads, 1);
        EXPECT_EQ(stripe.customers, 0);
        EXPECT_EQ(stripe.invoices, 0);
        EXPECT_TRUE(rig.store->list_invoices(rig.customer)->empty()) << "nothing is issued";
        EXPECT_EQ(*rig.store->billing_provider_customer(rig.customer, "stripe"), "cus_kept");
        EXPECT_TRUE(replacements(*rig.store).empty());
    }
}

// The memory store replaces a kept customer only while it still holds the id read, so a second,
// concurrent replacement gets the first one's id back (lake#856).
TEST(StripeBilling, TheMemoryStoreReplacesOnlyTheIdItWasGiven) {
    lake::control::InMemoryStore store;
    const auto user = store.create_user({"taap:1:ed25519:replace", "replace@example.test", "R"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", "replace"});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string id = customer->id;
    EXPECT_EQ(*store.replace_billing_provider_customer(id, "stripe", "cus_1", "cus_2"), "")
        << "nothing is kept to replace";
    EXPECT_EQ(*store.billing_provider_customer(id, "stripe"), "");
    ASSERT_TRUE(store.set_billing_provider_customer(id, "stripe", "cus_1"));
    EXPECT_EQ(*store.replace_billing_provider_customer(id, "stripe", "cus_1", "cus_2"), "cus_2");
    EXPECT_EQ(*store.replace_billing_provider_customer(id, "stripe", "cus_1", "cus_3"), "cus_2")
        << "the first replacement stays";
    EXPECT_EQ(*store.billing_provider_customer(id, "stripe"), "cus_2");
    EXPECT_EQ(store.replace_billing_provider_customer(id, "stripe", "", "cus_3").error().code,
              "invalid_input");
    EXPECT_EQ(
        store.replace_billing_provider_customer("no-such-customer", "stripe", "cus_2", "cus_3")
            .error()
            .code,
        "not_found");
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

    // Events lake does not act on, another app's invoice, and an invoice tagged as lake's that it
    // did not issue, are acknowledged.
    const auto other = rig.signed_notice(event("evt_other", "customer.created", "cus_1"));
    EXPECT_EQ(other.status, 200);
    EXPECT_EQ(other.body["ignored"], true);
    const auto foreign =
        rig.signed_notice(event("evt_foreign", "invoice.paid", "in_999", json::object()));
    EXPECT_EQ(foreign.status, 200);
    EXPECT_EQ(foreign.body["ignored"], true);
    const auto unknown = rig.signed_notice(event("evt_unknown", "invoice.paid", "in_999"));
    EXPECT_EQ(unknown.status, 200);
    EXPECT_EQ(unknown.body["matched"], false);

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

// One Stripe account, several apps (lake#855): every endpoint receives every event of its types,
// so an invoice of Cloud v1 or of another Lake is acknowledged before anything is recorded, even
// one whose Stripe id is one of this Lake's.
TEST(StripeBilling, AnotherAppsOrAnotherLakesInvoiceRecordsNothing) {
    FakeStripe stripe;
    StripeRig rig(stripe_providers(stripe));
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    const std::string invoice_id = issued.value["id"];

    const std::vector<std::pair<std::string, json>> foreign = {
        {"evt_cloud_v1", {{"team_id", "team_1"}, {"plan_id", "plan_1"}}},
        {"evt_untagged", json::object()},
        {"evt_other_lake", {{"lake_customer_id", rig.customer}, {"lake_instance", other_lake}}}};
    for (const auto& [id, metadata] : foreign) {
        const auto ignored = rig.signed_notice(event(id, "invoice.paid", "in_1", metadata));
        EXPECT_EQ(ignored.status, 200) << id;
        EXPECT_EQ(ignored.body, (json{{"received", true}, {"ignored", true}})) << id;
    }
    EXPECT_EQ(rig.store->get_invoice(invoice_id)->state, "due");
    // No billing_payment_events row: each event id is still unused. No audit event names one.
    for (const auto& [id, metadata] : foreign) {
        const auto unused = rig.store->apply_payment_event({"stripe", id, "invoice.paid"});
        ASSERT_TRUE(unused) << unused.error().message;
        EXPECT_FALSE(unused->duplicate) << id;
    }
    const auto events = lake::test::audit_events(*rig.store, {.limit = 500});
    ASSERT_TRUE(events) << events.error().message;
    for (const auto& entry : *events) {
        EXPECT_NE(entry.draft.action, "payment_notice") << entry.draft.details.dump();
    }
    // The signature still comes first: an unsigned foreign event is refused.
    EXPECT_EQ(rig.notice(event("evt_unsigned", "invoice.paid", "in_1", json::object()), "").status,
              400);
    // This Lake's own tag is this Lake's.
    const auto own = rig.signed_notice(
        event("evt_own",
              "invoice.paid",
              "in_1",
              {{"lake_customer_id", rig.customer}, {"lake_instance", this_lake}}));
    EXPECT_EQ(own.body["state"], "paid");
    EXPECT_EQ(rig.store->get_invoice(invoice_id)->state, "paid");
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

// Lake's own Stripe keys are platform environment variables (lake#628), read once at start: set,
// unset, half set or malformed, and a change that takes a restart.
TEST(StripeBilling, KeysComeFromTheEnvironmentAndAChangeTakesARestart) {
    FakeStripe stripe;
    int built = 0;
    const lake::api::StripeProviderFactory factory =
        [&](const lake::api::StripeKeys& keys) -> std::shared_ptr<lake::api::PaymentProvider> {
        ++built;
        return lake::api::stripe_payment_provider(
            keys, std::string(this_lake), stripe.http(), "https://stripe.test");
    };
    const std::string key(test_key);
    const std::string webhook(test_webhook);

    // Unset, half set or malformed: the manual provider, and a reason that names the variable,
    // never a value; Stripe is never built.
    struct Case {
        lake::api::StripeKeys keys;
        std::string reason;
    };
    const std::vector<Case> manual_cases = {
        {{"", ""}, "LAKE_STRIPE_SECRET_KEY and LAKE_STRIPE_WEBHOOK_SECRET are not set"},
        {{key, ""}, "LAKE_STRIPE_WEBHOOK_SECRET is not set"},
        {{"", webhook}, "LAKE_STRIPE_SECRET_KEY is not set"},
        {{"pk_test_publishable000001", webhook}, "LAKE_STRIPE_SECRET_KEY is not a Stripe secret key"},
        {{key, "not-a-webhook-secret-0001"}, "LAKE_STRIPE_WEBHOOK_SECRET is not a Stripe webhook"},
    };
    for (const Case& c : manual_cases) {
        SCOPED_TRACE(c.reason);
        auto providers = lake::api::environment_payment_providers(c.keys, "Pay by wire.", factory);
        StripeRig rig(providers);
        auto ctx = rig.context();
        auto current = liquid::runtime::block_on(providers->current(ctx));
        ASSERT_TRUE(current && *current);
        EXPECT_EQ((**current)->name(), "manual");
        EXPECT_NE(providers->status().reason.find(c.reason), std::string::npos)
            << providers->status().reason;
        EXPECT_EQ(built, 0);
        const auto viewer = rig.call("viewer", json::object());
        ASSERT_TRUE(viewer.ok) << viewer.error.message;
        EXPECT_EQ(viewer.value["billing_payment"]["provider"], "manual");
        EXPECT_EQ(viewer.value["billing_payment"]["online_payment"], false);
        EXPECT_FALSE(viewer.value["billing_payment"].contains("checked_at"));
        const auto info = rig.call("server_info", json::object());
        EXPECT_EQ(info.value["billing"], true);
        EXPECT_EQ(info.value["payment_provider"], "manual");
        EXPECT_EQ(info.value["online_payment"], false);
        const auto account = rig.call("billing_account", {{"customer_id", rig.customer}});
        ASSERT_TRUE(account.ok) << account.error.message;
        EXPECT_EQ(account.value["provider"]["name"], "manual");
        for (const std::string& value : {c.keys.secret_key, c.keys.webhook_secret}) {
            if (!value.empty()) {
                EXPECT_EQ(account.value.dump().find(value), std::string::npos);
                EXPECT_EQ(viewer.value.dump().find(value), std::string::npos);
                EXPECT_EQ(providers->status().reason.find(value), std::string::npos);
            }
        }
        // A notice is not for the manual provider.
        EXPECT_EQ(rig.signed_notice(event("evt_manual", "invoice.paid", "in_1")).status, 404);
    }

    // Both keys set: Stripe, built once, and no key in anything lake says.
    auto providers = lake::api::environment_payment_providers({key, webhook}, {}, factory);
    EXPECT_EQ(built, 1);
    StripeRig rig(providers);
    EXPECT_EQ(providers->status().provider, "stripe");
    EXPECT_TRUE(providers->status().online_payment);
    EXPECT_TRUE(providers->status().reason.empty());
    const auto info = rig.call("server_info", json::object());
    EXPECT_EQ(info.value["payment_provider"], "stripe");
    EXPECT_EQ(info.value["online_payment"], true);
    const auto described = rig.call("billing_account", {{"customer_id", rig.customer}});
    ASSERT_TRUE(described.ok) << described.error.message;
    EXPECT_EQ(described.value["provider"]["name"], "stripe");
    EXPECT_EQ(described.value.dump().find(key), std::string::npos);
    EXPECT_EQ(info.value.dump().find(key), std::string::npos);
    EXPECT_EQ(rig.call("viewer", json::object()).value.dump().find(webhook), std::string::npos);
    const auto issued =
        rig.call("issue_invoice", {{"customer_id", rig.customer}, {"period", rig.period}});
    ASSERT_TRUE(issued.ok) << issued.error.message;
    EXPECT_EQ(rig.signed_notice(event("evt_first", "invoice.paid", "in_1")).status, 200);
    EXPECT_EQ(rig.store->get_invoice(issued.value["id"])->state, "paid");
    EXPECT_EQ(built, 1);

    // Rotation is a restart with the new value: the new lake accepts a notice signed with the
    // new webhook secret and refuses one signed with the old.
    const std::string rotated = "whsec_lake628rotated000002";
    auto restarted = lake::api::environment_payment_providers({key, rotated}, {}, factory);
    StripeRig after(restarted);
    EXPECT_EQ(after.signed_notice(event("evt_old", "invoice.paid", "in_1")).status, 400);
    const auto unmatched = after.signed_notice(event("evt_new", "invoice.paid", "in_1"), rotated);
    EXPECT_EQ(unmatched.status, 200) << unmatched.body.dump();
}

// The server reads the keys from its options, which main fills from LAKE_STRIPE_SECRET_KEY and
// LAKE_STRIPE_WEBHOOK_SECRET; server_info says which provider is in use.
TEST(StripeBilling, TheServerChoosesTheProviderFromItsKeys) {
    for (const bool keys : {false, true}) {
        SCOPED_TRACE(keys ? "keys set" : "keys unset");
        lake::api::ServerOptions options;
        options.port = 0;
        options.store = std::make_shared<lake::control::InMemoryStore>();
        options.admin_token = "stripe-env-operator";
        options.instance_kek = std::string(kek);
        options.billing = true;
        if (keys) {
            options.stripe_keys = {std::string(test_key), std::string(test_webhook)};
        }
        lake::api::ApiServer server(options);
        ASSERT_TRUE(server.start()) << server.reason();
        liquid::http1::request_options request;
        request.url = std::format("http://127.0.0.1:{}/v1/server_info", server.port());
        request.method = "GET";
        const auto answered = lake::test::http_request(std::move(request));
        ASSERT_TRUE(answered);
        ASSERT_EQ(answered->status, 200U);
        const std::string body(reinterpret_cast<const char*>(answered->body.data()),
                               answered->body.size());
        const json value = json::parse(body)["value"];
        EXPECT_EQ(value["payment_provider"], keys ? "stripe" : "manual");
        EXPECT_EQ(value["online_payment"], keys);
        EXPECT_EQ(body.find(test_key), std::string::npos);
        EXPECT_EQ(body.find(test_webhook), std::string::npos);
        server.stop();
    }
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
