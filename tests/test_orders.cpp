// The apex order sheet (lake#831, docs/api/orders.md): its configuration and validation, the order
// form's checks, the status link, the emails and their Resend client (against a test double for
// HTTP), the abuse limit, Stripe Checkout and its webhook, the cloud_orders contract in both stores
// (the operator's writes included), and the pages and routes over HTTP.

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/client_address.hpp"
#include "lake/api/orders.hpp"
#include "lake/api/regions.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/resend.hpp"
#include "lake/api/server.hpp"
#include "lake/api/stripe.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "postgres_fixture.hpp"

namespace {

using nlohmann::json;
using namespace lake::api;

constexpr std::string_view kek = "6c616b652d6f726465722d73686565742d746573742d6b656b2d3833312d3031";
constexpr std::string_view stripe_key = "sk_test_lake831fakekey0001";
constexpr std::string_view stripe_webhook = "whsec_lake831fakesecret0001";
constexpr std::string_view resend_key = "re_lake831fakekey0001";
// This Lake's public host, and another Lake's billing through the same Stripe account (lake#855).
constexpr std::string_view this_lake = "eu.lake.live";
constexpr std::string_view other_lake = "us.lake.live";

constexpr std::string_view production_regions =
    R"([{"name":"swedencentral","label":"Europe","host":"eu.lake.live","latitude":"60.67488","longitude":"17.14127"},)"
    R"({"name":"eastus","label":"United States","host":"us.lake.live","latitude":"37.3719","longitude":"-79.8164"}])";

Regions production() {
    auto regions = regions_from_environment("swedencentral", "", production_regions, "lake.live");
    EXPECT_TRUE(regions) << regions.error();
    return regions ? *regions : Regions{};
}

OrderSheet default_sheet() {
    auto sheet = order_sheet_from_environment("");
    EXPECT_TRUE(sheet) << sheet.error();
    return sheet ? *sheet : OrderSheet{};
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string field(std::string_view body, std::string_view name) {
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

// Stripe Checkout as far as lake uses it: sessions, with Stripe's idempotency, in an account
// that may lack one of Lake's products (`missing_product`).
struct FakeCheckout {
    std::mutex mutex;
    std::vector<StripeRequest> requests;
    std::map<std::string, StripeResponse> answered;
    int sessions = 0;
    bool fail = false;
    std::string missing_product;

    static std::string header(const StripeRequest& request, std::string_view name) {
        for (const auto& [key, value] : request.headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

    StripeResponse answer(const StripeRequest& request) {
        std::lock_guard lock(mutex);
        requests.push_back(request);
        if (header(request, "Authorization") != "Bearer " + std::string(stripe_key)) {
            return {401, R"({"error":{"message":"Invalid API Key"}})", {}};
        }
        if (fail) {
            return {500, R"({"error":{"message":"Stripe is down"}})", {}};
        }
        const std::string key = header(request, "Idempotency-Key");
        if (const auto found = answered.find(key); found != answered.end()) {
            return found->second;
        }
        if (!request.url.ends_with("/v1/checkout/sessions")) {
            return {404, R"({"error":{"message":"no such route"}})", {}};
        }
        if (!missing_product.empty() &&
            field(request.body, "line_items%5B0%5D%5Bprice_data%5D%5Bproduct%5D") ==
                missing_product) {
            return {400,
                    json{{"error",
                          {{"code", "resource_missing"},
                           {"param", "line_items[0][price_data][product]"},
                           {"message", "No such product: '" + missing_product + "'"}}}}
                        .dump(),
                    {}};
        }
        const std::string id = std::format("cs_test_{}", ++sessions);
        StripeResponse response{
            200, json{{"id", id}, {"url", "https://checkout.stripe.test/c/" + id}}.dump(), {}};
        answered[key] = response;
        return response;
    }

    std::shared_ptr<PaymentProviders> providers() {
        return fixed_payment_providers(stripe_payment_provider(
            {std::string(stripe_key), std::string(stripe_webhook)},
            std::string(this_lake),
            [this](const StripeRequest& request) { return answer(request); },
            "https://stripe.test"));
    }

    std::size_t count() {
        std::lock_guard lock(mutex);
        return requests.size();
    }
    StripeRequest last() {
        std::lock_guard lock(mutex);
        return requests.back();
    }
};

// Resend as far as lake uses it: POST /emails, recorded, with its idempotency.
struct FakeResend {
    std::mutex mutex;
    std::vector<ResendRequest> requests;
    std::map<std::string, int> sent_by_key;
    int failures_left = 0;
    bool transport_down = false;

    ResendResponse answer(const ResendRequest& request) {
        std::lock_guard lock(mutex);
        requests.push_back(request);
        if (transport_down) {
            return {0, {}, "connection refused"};
        }
        std::string key;
        std::string authorization;
        for (const auto& [name, value] : request.headers) {
            if (name == "Idempotency-Key") {
                key = value;
            }
            if (name == "Authorization") {
                authorization = value;
            }
        }
        if (authorization != "Bearer " + std::string(resend_key)) {
            return {
                401,
                R"({"statusCode":401,"message":"API key is invalid","name":"validation_error"})",
                {}};
        }
        if (failures_left > 0) {
            --failures_left;
            return {500, R"({"message":"try again"})", {}};
        }
        ++sent_by_key[key];
        return {200, json{{"id", std::format("email_{}", requests.size())}}.dump(), {}};
    }

    ResendHttp http() {
        return [this](const ResendRequest& request) { return answer(request); };
    }

    std::vector<json> bodies() {
        std::lock_guard lock(mutex);
        std::vector<json> out;
        for (const ResendRequest& request : requests) {
            out.push_back(json::parse(request.body));
        }
        return out;
    }
};

ResendSettings resend_settings() {
    auto settings = resend_settings_from_environment(resend_key, "", "");
    EXPECT_TRUE(settings) << settings.error();
    return settings ? *settings : ResendSettings{};
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Configuration

TEST(OrdersConfig, TheDefaultIsTheIssuesOffers) {
    const OrderSheet sheet = default_sheet();
    EXPECT_TRUE(sheet.enabled);
    EXPECT_EQ(sheet.currency, "USD");
    ASSERT_EQ(sheet.offers.size(), 3U);
    EXPECT_EQ(sheet.offers[0].id, "private_warehouse");
    EXPECT_EQ(sheet.offers[0].monthly_cents, 100'000);
    EXPECT_EQ(included_credit_cents(sheet.offers[0]), 40'000);
    EXPECT_TRUE(sheet.offers[0].checkout);
    EXPECT_EQ(sheet.offers[1].id, "dedicated_cluster");
    EXPECT_EQ(sheet.offers[1].monthly_cents, 500'000);
    EXPECT_EQ(included_credit_cents(sheet.offers[1]), 200'000);
    EXPECT_EQ(sheet.offers[2].id, "on_prem");
    EXPECT_EQ(sheet.offers[2].monthly_cents, 1'000'000);
    EXPECT_TRUE(sheet.offers[2].starting_from);
    EXPECT_FALSE(sheet.offers[2].checkout);
    EXPECT_EQ(included_credit_cents(sheet.offers[2], 2), 800'000);
    // The lead line is off until its test is recorded (docs/api/billing.md).
    EXPECT_FALSE(sheet.show_promise);
    EXPECT_EQ(sheet.promise, "Train and serve a model in 5 minutes for under $5");
    // Regions beyond the shared ones, and only the provider the operator provisions is checkout.
    const OrderProvider* azure = sheet.find_provider("azure");
    ASSERT_NE(azure, nullptr);
    EXPECT_TRUE(azure->checkout);
    const auto has = [&](std::string_view id) {
        return std::ranges::any_of(azure->regions,
                                   [&](const OrderRegion& r) { return r.id == id; });
    };
    EXPECT_TRUE(has("canadacentral"));
    EXPECT_TRUE(has("japaneast"));
    // Checkout only where the operator can create a cluster (orama-operator#635): the Asia regions
    // are listed but ordered with the order form.
    const auto checkout = [&](std::string_view id) {
        return std::ranges::find(azure->regions, id, &OrderRegion::id)->checkout;
    };
    EXPECT_TRUE(checkout("canadacentral"));
    EXPECT_TRUE(checkout("eastus"));
    EXPECT_FALSE(checkout("japaneast"));
    EXPECT_FALSE(checkout("southeastasia"));
    EXPECT_FALSE(sheet.find_provider("aws")->regions.front().checkout)
        << "a region defaults to its provider's checkout";
    ASSERT_NE(sheet.find_provider("aws"), nullptr);
    EXPECT_FALSE(sheet.find_provider("aws")->checkout);
    EXPECT_EQ(sheet.trust.size(), 2U);
    EXPECT_EQ(sheet.orders_per_client_per_hour, 5);
    // Every offer's features say backups are included.
    for (const OrderOffer& offer : sheet.offers) {
        EXPECT_TRUE(std::ranges::any_of(offer.features, [](const std::string& f) {
            return f.find("Backups included") != std::string::npos;
        })) << offer.id;
    }
    // Private Warehouse and every offer above it carry their own domain, white-labelled, on
    // request until it is delivered (lake#1129); the shared option does not.
    const auto white_label = [](const std::vector<std::string>& features) {
        return std::ranges::count(
            features, std::string("Your own domain, white-labelled (available on request)"));
    };
    ASSERT_EQ(sheet.offers.size(), 3U);
    for (const OrderOffer& offer : sheet.offers) {
        EXPECT_EQ(white_label(offer.features), 1) << offer.id;
    }
    for (const std::string& feature : sheet.shared_features) {
        EXPECT_EQ(feature.find("white-label"), std::string::npos) << feature;
        EXPECT_EQ(feature.find("domain"), std::string::npos) << feature;
    }
}

TEST(OrdersConfig, OverridesMergeOverTheDefault) {
    const auto sheet = order_sheet_from_environment(
        R"({"shared":{"show_promise":true},"offers":{"dedicated_cluster":{"monthly_usd":6000,"credit_percent":50}},)"
        R"("url":"https://lake.live/","limits":{"orders_per_client_per_hour":2}})");
    ASSERT_TRUE(sheet) << sheet.error();
    EXPECT_TRUE(sheet->show_promise);
    EXPECT_EQ(sheet->find_offer("dedicated_cluster")->monthly_cents, 600'000);
    EXPECT_EQ(included_credit_cents(*sheet->find_offer("dedicated_cluster")), 300'000);
    EXPECT_EQ(sheet->find_offer("private_warehouse")->monthly_cents, 100'000);
    EXPECT_EQ(sheet->url, "https://lake.live/");
    EXPECT_EQ(sheet->orders_per_client_per_hour, 2);
    EXPECT_EQ(sheet->orders_per_hour, 60);
    EXPECT_FALSE(order_sheet_from_environment(R"({"enabled":false})")->enabled);
}

TEST(OrdersConfig, InvalidValuesNameTheField) {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"{", "LAKE_ORDER_SHEET is not valid JSON"},
        {"[]", "must be a JSON object"},
        {R"({"currency":"EUR"})", "currency must be USD"},
        {R"({"offers":{"gold":{}}})", "offers.gold is not an offer"},
        {R"({"offers":{"on_prem":null}})", "offers.on_prem must be an object"},
        {R"({"offers":{"private_warehouse":{"monthly_usd":0}}})", "monthly_usd must be between"},
        {R"({"offers":{"private_warehouse":{"monthly_usd":"1000"}}})",
         "monthly_usd must be a whole number"},
        {R"({"offers":{"private_warehouse":{"credit_percent":140}}})",
         "credit_percent must be between 0 and 100"},
        {R"({"offers":{"on_prem":{"checkout":true}}})", "on-prem is ordered by form"},
        {R"({"providers":[]})", "at least one provider"},
        {R"({"providers":[{"id":"x","label":"X","regions":[]}]})",
         "regions must be a non-empty array"},
        {R"({"providers":[{"id":"x y","label":"X","regions":[{"id":"r","label":"R"}]}]})",
         "providers[0].id must be"},
        {R"({"providers":[{"id":"x","label":"X","regions":[{"id":"r","label":"R"},{"id":"r","label":"S"}]}]})",
         "names the region \"r\" twice"},
        {R"({"providers":[{"id":"x","label":"X","regions":[{"id":"r","label":"R"}]},{"id":"x","label":"Y","regions":[{"id":"r","label":"R"}]}]})",
         "names the provider \"x\" twice"},
        {R"({"providers":[{"id":"x","label":"X","checkout":false,"regions":[{"id":"r","label":"R","checkout":true}]}]})", "regions[0].checkout cannot be true on a provider whose checkout is false"},
        {R"({"providers":[{"id":"x","label":"X","checkout":true,"regions":[{"id":"r","label":"R","checkout":"yes"}]}]})", "regions[0].checkout must be true or false"},
        {R"({"trust":[{"text":"SOC 2","url":"javascript:void"}]})",
         "trust[0].url must be an https or mailto URL"},
        {R"({"url":"http://lake.live/"})", "url must be an https URL"},
        {R"({"shared":{"price_list_url":"ftp://x"}})", "price_list_url must be an https URL"},
        {R"({"limits":{"orders_per_hour":0}})", "orders_per_hour must be between"},
    };
    for (const auto& [value, message] : cases) {
        const auto sheet = order_sheet_from_environment(value);
        ASSERT_FALSE(sheet) << "accepted " << value;
        EXPECT_NE(sheet.error().find(message), std::string::npos) << value << ": " << sheet.error();
    }
}

TEST(OrdersConfig, MoneyAndSlugs) {
    EXPECT_EQ(format_usd(100'000), "$1,000");
    EXPECT_EQ(format_usd(1'000'000), "$10,000");
    EXPECT_EQ(format_usd(12'345'678), "$123,456.78");
    EXPECT_EQ(format_usd(5), "$0.05");
    EXPECT_EQ(offer_slug("dedicated_cluster"), "dedicated-cluster");
    EXPECT_EQ(offer_from_slug("on-prem"), "on_prem");
    EXPECT_EQ(offer_from_slug("shared"), "");
    EXPECT_EQ(offer_slug("gold"), "");
}

// ------------------------------------------------------------------------------------------------
// The order form

TEST(OrdersForm, EachOfferAndPathIsChecked) {
    const OrderSheet sheet = default_sheet();
    const Regions regions = production();
    const auto form = [&](std::string offer,
                          std::string path,
                          std::string location,
                          std::string details = "",
                          std::string quantity = "",
                          bool card = true) {
        return validate_order_form(sheet,
                                   regions,
                                   {std::move(offer),
                                    std::move(path),
                                    std::move(location),
                                    "Ada",
                                    "ada@example.com",
                                    "Acme",
                                    std::move(details),
                                    std::move(quantity)},
                                   card);
    };
    // A private warehouse in a shared region, paid.
    auto warehouse = form("private_warehouse", "checkout", "eastus");
    ASSERT_TRUE(warehouse);
    EXPECT_EQ(warehouse->path, "checkout");
    EXPECT_EQ(warehouse->region, "eastus");
    EXPECT_EQ(warehouse->cloud_provider, "");
    EXPECT_EQ(warehouse->amount_cents, 100'000);
    EXPECT_EQ(warehouse->included_credit_cents, 40'000);
    EXPECT_EQ(warehouse->currency, "usd");
    // Card payment is one at a time: the operator provisions one cluster and writes one host, so
    // three are a larger order, sent as the order form.
    const auto three_paid = form("dedicated_cluster", "checkout", "azure:canadacentral", "", "3");
    ASSERT_FALSE(three_paid);
    EXPECT_EQ(three_paid.error().at("path"),
              "Card payment orders one at a time: send the order form for more.");
    EXPECT_FALSE(form("private_warehouse", "checkout", "eastus", "", "2"));
    EXPECT_TRUE(form("dedicated_cluster", "checkout", "azure:canadacentral", "", "1"));
    // A dedicated cluster in Canada on Azure, three of them, through the order form.
    auto cluster = form("dedicated_cluster", "order_form", "azure:canadacentral", "", "3");
    ASSERT_TRUE(cluster);
    EXPECT_EQ(cluster->cloud_provider, "azure");
    EXPECT_EQ(cluster->region, "canadacentral");
    EXPECT_EQ(cluster->quantity, 3);
    EXPECT_EQ(cluster->amount_cents, 1'500'000);
    EXPECT_EQ(cluster->included_credit_cents, 600'000);
    // A region the operator cannot reach (Asia on Azure) is an order form, though listed.
    const auto asia = form("dedicated_cluster", "checkout", "azure:japaneast");
    ASSERT_FALSE(asia);
    EXPECT_EQ(asia.error().at("path"),
              "This region is ordered with the order form: we will confirm availability.");
    EXPECT_TRUE(form("dedicated_cluster", "checkout", "azure:southeastasia").error().contains("path"));
    auto asia_form = form("dedicated_cluster", "order_form", "azure:japaneast");
    ASSERT_TRUE(asia_form);
    EXPECT_EQ(asia_form->region, "japaneast");
    // A provider the operator does not provision yet is an order form.
    EXPECT_EQ(
        form("dedicated_cluster", "checkout", "aws:ca-central-1").error().at("path").substr(0, 13),
        "This provider");
    EXPECT_TRUE(form("dedicated_cluster", "order_form", "aws:ca-central-1"));
    // Anything not listed: the order form, with the details.
    EXPECT_TRUE(form("dedicated_cluster", "order_form", "other").error().contains("details"));
    EXPECT_TRUE(
        form("dedicated_cluster", "checkout", "other", "OVH in Paris").error().contains("path"));
    EXPECT_TRUE(form("dedicated_cluster", "order_form", "other", "OVH in Paris"));
    EXPECT_TRUE(form("dedicated_cluster", "order_form", "azure:mars").error().contains("location"));
    EXPECT_TRUE(form("private_warehouse", "order_form", "westus2").error().contains("location"));
    // On-prem is only the order form, and says where it runs.
    EXPECT_TRUE(form("on_prem", "checkout", "", "bare metal, H100s").error().contains("path"));
    EXPECT_TRUE(form("on_prem", "order_form", "").error().contains("details"));
    auto on_prem = form("on_prem", "order_form", "", "Bare metal\r\nwith H100s, two regions");
    ASSERT_TRUE(on_prem);
    EXPECT_EQ(on_prem->details, "Bare metal\nwith H100s, two regions");
    EXPECT_EQ(on_prem->amount_cents, 1'000'000);
    // Without card payment, every offer is the order form.
    EXPECT_TRUE(
        form("private_warehouse", "checkout", "eastus", "", "", false).error().contains("path"));
    EXPECT_TRUE(form("private_warehouse", "order_form", "eastus", "", "", false));
    // Contact details and limits.
    EXPECT_TRUE(
        validate_order_form(
            sheet, regions, {"on_prem", "order_form", "", "", "not-an-email", "", "x", ""}, true)
            .error()
            .contains("contact_email"));
    auto unnamed = validate_order_form(
        sheet, regions, {"on_prem", "order_form", "", " \t", "a@b.co", "", "x", ""}, true);
    EXPECT_TRUE(unnamed.error().contains("contact_name"));
    EXPECT_TRUE(form("on_prem", "order_form", "", "x", "0").error().contains("quantity"));
    EXPECT_TRUE(form("on_prem", "order_form", "", "x", "1001").error().contains("quantity"));
    EXPECT_TRUE(form("on_prem", "order_form", "", "x", "2x").error().contains("quantity"));
    EXPECT_TRUE(
        form("on_prem", "order_form", "", std::string(4001, 'x')).error().contains("details"));
    EXPECT_TRUE(form("shared", "order_form", "eastus").error().contains("offer"));
    // A line break in a one-line field cannot break an email subject.
    auto newline =
        validate_order_form(sheet,
                            regions,
                            {"on_prem", "order_form", "", "Ada\r\nBcc: x", "a@b.co", "", "x", ""},
                            true);
    ASSERT_TRUE(newline);
    EXPECT_EQ(newline->contact_name.find('\n'), std::string::npos);
}

TEST(OrdersForm, StatusLinksAreBoundToTheOrderAndTheKey) {
    const std::string token = order_status_token(kek, "order-1");
    EXPECT_FALSE(token.empty());
    EXPECT_TRUE(order_status_token_matches(kek, "order-1", token));
    EXPECT_FALSE(order_status_token_matches(kek, "order-2", token));
    EXPECT_FALSE(order_status_token_matches("another-key-another-key-another-k", "order-1", token));
    EXPECT_FALSE(order_status_token_matches(kek, "order-1", ""));
    EXPECT_FALSE(order_status_token_matches("", "order-1", token));
}

TEST(OrdersForm, TheRateLimitIsPerClientAndInAll) {
    OrderRateLimiter limiter(2, 3);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(limiter.allow("1.1.1.1", "order_form", start));
    EXPECT_TRUE(limiter.allow("1.1.1.1", "order_form", start));
    EXPECT_FALSE(limiter.allow("1.1.1.1", "order_form", start)) << "two an hour from one address";
    EXPECT_TRUE(limiter.allow("2.2.2.2", "order_form", start));
    EXPECT_FALSE(limiter.allow("3.3.3.3", "order_form", start)) << "three an hour in all";
    // lake#838: order forms that used up their cap cannot block a checkout order.
    EXPECT_TRUE(limiter.allow("3.3.3.3", "checkout", start));
    EXPECT_TRUE(limiter.allow("4.4.4.4", "checkout", start));
    // An hour later the window has moved on.
    const auto later = start + std::chrono::minutes(61);
    EXPECT_TRUE(limiter.allow("1.1.1.1", "order_form", later));
    EXPECT_TRUE(limiter.allow("3.3.3.3", "order_form", later));
}

// ------------------------------------------------------------------------------------------------
// Email

TEST(OrdersEmail, WhoIsToldAboutWhichState) {
    lake::control::CloudOrder order;
    const auto due = [&](std::string state, std::string support = "", std::string customer = "") {
        order.state = std::move(state);
        order.support_emailed_state = std::move(support);
        order.customer_emailed_state = std::move(customer);
        return order_email_recipients_due(order);
    };
    using List = std::vector<std::string>;
    EXPECT_EQ(due("submitted"), (List{"support", "customer"}));
    EXPECT_EQ(due("submitted", "submitted"), (List{"customer"}));
    EXPECT_EQ(due("submitted", "submitted", "submitted"), List{});
    EXPECT_EQ(due("pending_payment"), List{});
    EXPECT_EQ(due("paid", "submitted"), (List{"support", "customer"}));
    EXPECT_EQ(due("provisioning", "paid", "paid"), List{});
    EXPECT_EQ(due("ready", "paid", "paid"), (List{"customer"}));
    EXPECT_EQ(due("failed", "paid", "paid"), (List{"support", "customer"}));
    EXPECT_EQ(due("cancelled"), List{});
    EXPECT_EQ(order_email_retry_seconds(1), 60);
    EXPECT_EQ(order_email_retry_seconds(2), 120);
    EXPECT_EQ(order_email_retry_seconds(30), 3600);
}

TEST(OrdersEmail, SupportGetsEveryFieldAndTheCustomerTheStatusLink) {
    const OrderSheet sheet = default_sheet();
    const Regions regions = production();
    const ResendSettings settings = resend_settings();
    lake::control::CloudOrder order;
    order.id = "0b6f2c1e-1111-4222-8333-444455556666";
    order.offer = "dedicated_cluster";
    order.path = "order_form";
    order.cloud_provider = "azure";
    order.region = "canadacentral";
    order.contact_name = "Ada Lovelace";
    order.contact_email = "ada@example.com";
    order.company = "Analytical Engines";
    order.details = "Two clusters, GPUs";
    order.quantity = 2;
    order.amount_cents = 1'000'000;
    order.included_credit_cents = 400'000;
    order.state = "submitted";
    order.created_at = "2026-10-18T12:00:00Z";
    const Email support = order_email(sheet, regions, settings, order, "support", "");
    EXPECT_EQ(support.to, std::vector<std::string>{"support@orama.com"});
    EXPECT_EQ(support.reply_to, "ada@example.com");
    EXPECT_NE(support.subject.find(order.id), std::string::npos);
    EXPECT_NE(support.subject.find("Order form"), std::string::npos);
    for (const std::string_view expected :
         {std::string_view("Order: 0b6f2c1e-1111-4222-8333-444455556666"),
          std::string_view("Offer: Dedicated Cluster"),
          std::string_view("Order form, no payment"),
          std::string_view("Microsoft Azure, Canada (Toronto) (canadacentral)"),
          std::string_view("Quantity: 2"),
          std::string_view("$10,000 USD / month (2 x $5,000)"),
          std::string_view("Included usage credit: $4,000 USD"),
          std::string_view("Name: Ada Lovelace"),
          std::string_view("Email: ada@example.com"),
          std::string_view("Company: Analytical Engines"),
          std::string_view("Two clusters, GPUs"),
          std::string_view("not paid and nothing is provisioned")}) {
        EXPECT_NE(support.text.find(expected), std::string::npos) << expected << "\n"
                                                                  << support.text;
    }
    const Email customer = order_email(
        sheet, regions, settings, order, "customer", "https://lake.live/orders/x?token=t");
    EXPECT_EQ(customer.to, std::vector<std::string>{"ada@example.com"});
    EXPECT_EQ(customer.reply_to, "support@orama.com");
    EXPECT_NE(customer.text.find("https://lake.live/orders/x?token=t"), std::string::npos);
    EXPECT_NE(customer.text.find("Nothing has been charged"), std::string::npos);
    order.state = "ready";
    order.host = "acme.lake.live";
    const Email ready = order_email(sheet, regions, settings, order, "customer", "");
    EXPECT_NE(ready.subject.find("is ready"), std::string::npos);
    EXPECT_NE(ready.text.find("https://acme.lake.live/"), std::string::npos);
    order.state = "failed";
    order.failure_reason = "quota exceeded in canadacentral";
    const Email failed = order_email(sheet, regions, settings, order, "support", "");
    EXPECT_NE(failed.subject.find("Order failed"), std::string::npos);
    EXPECT_NE(failed.text.find("Failure: quota exceeded in canadacentral"), std::string::npos);
}

// The customer's copy goes to whatever address was typed, so it must not relay anything the
// sender wrote: only Lake's wording, the offer, where, quantity, price, the id and the link.
TEST(OrdersEmail, TheCustomersCopyCarriesNothingTheSenderTyped) {
    const OrderSheet sheet = default_sheet();
    const Regions regions = production();
    const ResendSettings settings = resend_settings();
    lake::control::CloudOrder order;
    order.id = "0b6f2c1e-1111-4222-8333-444455556666";
    order.offer = "dedicated_cluster";
    order.path = "order_form";
    order.cloud_provider = "azure";
    order.region = "canadacentral";
    order.contact_name = "Click https://phish.example/name";
    order.contact_email = "victim@example.com";
    order.company = "Visit https://phish.example/company";
    order.details = "Your account is locked: https://phish.example/details";
    order.quantity = 2;
    order.amount_cents = 1'000'000;
    order.included_credit_cents = 400'000;
    order.created_at = "2026-10-18T12:00:00Z";
    order.host = "acme.lake.live";
    const std::string status = "https://lake.live/orders/x?token=t";
    for (const std::string state : {"submitted", "paid", "ready", "failed"}) {
        order.state = state;
        order.failure_reason = state == "failed" ? "quota exceeded" : "";
        const Email customer = order_email(sheet, regions, settings, order, "customer", status);
        for (const std::string& email_part : {customer.subject, customer.text}) {
            EXPECT_EQ(email_part.find("phish.example"), std::string::npos) << state << "\n"
                                                                         << email_part;
            EXPECT_EQ(email_part.find("quota exceeded"), std::string::npos) << state;
        }
        for (const std::string_view expected :
             {std::string_view("Order: 0b6f2c1e-1111-4222-8333-444455556666"),
              std::string_view("Offer: Dedicated Cluster"),
              std::string_view("Microsoft Azure, Canada (Toronto) (canadacentral)"),
              std::string_view("Quantity: 2"),
              std::string_view("$10,000 USD / month (2 x $5,000)"),
              std::string_view("https://lake.live/orders/x?token=t")}) {
            EXPECT_NE(customer.text.find(expected), std::string::npos) << state << ": " << expected;
        }
        if (state == "submitted" || state == "paid" || state == "failed") {
            const Email support = order_email(sheet, regions, settings, order, "support", "");
            EXPECT_NE(support.text.find("https://phish.example/details"), std::string::npos);
            EXPECT_NE(support.text.find("https://phish.example/name"), std::string::npos);
            EXPECT_NE(support.text.find("https://phish.example/company"), std::string::npos);
        }
    }
}

TEST(Resend, SettingsComeFromTheEnvironment) {
    const auto defaults = resend_settings_from_environment("", "", "");
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->support_email, "support@orama.com");
    EXPECT_EQ(defaults->from, "Lake <orders@lake.live>");
    EXPECT_EQ(defaults->unavailable_reason(), "LAKE_RESEND_API_KEY is not set");
    const auto set =
        resend_settings_from_environment(resend_key, "orders@orama.com", "ops@orama.com");
    ASSERT_TRUE(set);
    EXPECT_EQ(set->unavailable_reason(), "");
    EXPECT_EQ(set->support_email, "ops@orama.com");
    const auto bad_key = resend_settings_from_environment("sk_live_abcdefgh", "", "");
    ASSERT_FALSE(bad_key);
    EXPECT_EQ(bad_key.error(), "LAKE_RESEND_API_KEY is not a Resend API key (re_...)");
    EXPECT_FALSE(resend_settings_from_environment("", "Lake <not an address>", ""));
    EXPECT_FALSE(resend_settings_from_environment("", "", "support"));
    EXPECT_TRUE(plausible_email_address("a.b+c@example.co.uk"));
    EXPECT_FALSE(plausible_email_address("a@b"));
    EXPECT_FALSE(plausible_email_address("a b@example.com"));
    EXPECT_FALSE(plausible_email_address("a@@example.com"));
    EXPECT_FALSE(plausible_email_address("<a@example.com>"));
}

TEST(Resend, SendsWithTheKeyAndAnIdempotencyKey) {
    FakeResend resend;
    const ResendSettings settings = resend_settings();
    const Email email{{"ada@example.com"}, "Subject", "Body\nline two", "support@orama.com"};
    const auto sent = send_resend_email(
        settings, email, "lake-order-1-submitted-customer", resend.http(), "https://resend.test");
    ASSERT_TRUE(sent) << sent.error();
    EXPECT_EQ(*sent, "email_1");
    ASSERT_EQ(resend.requests.size(), 1U);
    const ResendRequest& request = resend.requests[0];
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.url, "https://resend.test/emails");
    const json body = json::parse(request.body);
    EXPECT_EQ(body["from"], "Lake <orders@lake.live>");
    EXPECT_EQ(body["to"], json::array({"ada@example.com"}));
    EXPECT_EQ(body["subject"], "Subject");
    EXPECT_EQ(body["text"], "Body\nline two");
    EXPECT_EQ(body["reply_to"], "support@orama.com");
    EXPECT_EQ(resend.sent_by_key["lake-order-1-submitted-customer"], 1);
    // A refused key is reported without echoing it; a failure keeps Resend's message.
    ResendSettings wrong = settings;
    wrong.api_key = "re_wrongwrongwrong";
    const auto refused = send_resend_email(wrong, email, "k", resend.http(), "https://resend.test");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), "Resend refused Lake's key (HTTP 401); check LAKE_RESEND_API_KEY");
    EXPECT_EQ(refused.error().find("re_wrong"), std::string::npos);
    resend.failures_left = 1;
    const auto failed =
        send_resend_email(settings, email, "k2", resend.http(), "https://resend.test");
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), "Resend refused the email (HTTP 500): try again");
    resend.transport_down = true;
    const auto down =
        send_resend_email(settings, email, "k3", resend.http(), "https://resend.test");
    ASSERT_FALSE(down);
    EXPECT_EQ(down.error(), "Resend could not be reached: connection refused");
    // Without a key nothing is sent.
    const std::size_t before = resend.requests.size();
    const auto unset = send_resend_email(ResendSettings{}, email, "k4", resend.http());
    ASSERT_FALSE(unset);
    EXPECT_EQ(unset.error(), "LAKE_RESEND_API_KEY is not set");
    EXPECT_EQ(resend.requests.size(), before);
}

// ------------------------------------------------------------------------------------------------
// Stripe Checkout and its webhook

TEST(OrdersCheckout, ASubscriptionWithTheSheetsPriceInline) {
    FakeCheckout stripe;
    ApiContext context;
    const auto providers = stripe.providers();
    auto current = liquid::runtime::block_on(providers->current(context));
    ASSERT_TRUE(current && *current);
    const std::shared_ptr<PaymentProvider> provider = **current;
    const auto session =
        provider->create_checkout({"order-1",
                                   "dedicated_cluster",
                                   "Microsoft Azure, Canada",
                                   500'000,
                                   2,
                                   "usd",
                                   "ada@example.com",
                                   "https://lake.live/orders/order-1?token=t&checkout=done",
                                   "https://lake.live/orders/order-1?token=t&checkout=cancelled"});
    ASSERT_TRUE(session) << session.error();
    EXPECT_EQ(session->id, "cs_test_1");
    EXPECT_EQ(session->url, "https://checkout.stripe.test/c/cs_test_1");
    const StripeRequest request = stripe.last();
    EXPECT_EQ(request.url, "https://stripe.test/v1/checkout/sessions");
    EXPECT_EQ(FakeCheckout::header(request, "Idempotency-Key"), "lake-order-order-1-checkout");
    EXPECT_EQ(field(request.body, "mode"), "subscription");
    EXPECT_EQ(field(request.body, "client_reference_id"), "order-1");
    EXPECT_EQ(field(request.body, "line_items%5B0%5D%5Bquantity%5D"), "2");
    EXPECT_EQ(field(request.body, "line_items%5B0%5D%5Bprice_data%5D%5Bcurrency%5D"), "usd");
    EXPECT_EQ(field(request.body, "line_items%5B0%5D%5Bprice_data%5D%5Bunit_amount%5D"), "500000");
    EXPECT_EQ(field(request.body, "line_items%5B0%5D%5Bprice_data%5D%5Brecurring%5D%5Binterval%5D"),
              "month");
    // The line is the offer's Lake product (lake#855), whose name the operator gave it; where it
    // runs is the subscription's description.
    EXPECT_EQ(field(request.body, "line_items%5B0%5D%5Bprice_data%5D%5Bproduct%5D"),
              "lake_dedicated_cluster");
    EXPECT_EQ(request.body.find("product_data"), std::string::npos) << request.body;
    EXPECT_EQ(field(request.body, "subscription_data%5Bdescription%5D"),
              "Microsoft%20Azure%2C%20Canada");
    EXPECT_EQ(field(request.body, "customer_email"), "ada%40example.com");
    EXPECT_EQ(field(request.body, "metadata%5Blake_order_id%5D"), "order-1");
    // The session and its subscription are tagged as this Lake's (lake#855).
    for (const std::string prefix : {"metadata", "subscription_data%5Bmetadata%5D"}) {
        EXPECT_EQ(field(request.body, prefix + "%5Blake_order_id%5D"), "order-1") << prefix;
        EXPECT_EQ(field(request.body, prefix + "%5Borama_app%5D"), "lake") << prefix;
        EXPECT_EQ(field(request.body, prefix + "%5Blake_instance%5D"), this_lake) << prefix;
    }
    // The manual provider has no checkout.
    EXPECT_FALSE(manual_payment_provider()->create_checkout({}));
}

TEST(OrdersCheckout, EachPaidOfferIsItsLakeProduct) {
    EXPECT_EQ(stripe_offer_product("private_warehouse"), "lake_private_warehouse");
    EXPECT_EQ(stripe_offer_product("dedicated_cluster"), "lake_dedicated_cluster");
    EXPECT_EQ(stripe_offer_product("on_prem"), "") << "on-prem is ordered by form";

    FakeCheckout stripe;
    ApiContext context;
    const auto providers = stripe.providers();
    auto current = liquid::runtime::block_on(providers->current(context));
    ASSERT_TRUE(current && *current);
    const std::shared_ptr<PaymentProvider> provider = **current;
    const auto order = [](std::string id, std::string offer) {
        return CheckoutRequest{std::move(id),
                               std::move(offer),
                               "East US",
                               100'000,
                               1,
                               "usd",
                               "ada@example.com",
                               "https://lake.live/orders/o?token=t&checkout=done",
                               "https://lake.live/orders/o?token=t&checkout=cancelled"};
    };
    ASSERT_TRUE(provider->create_checkout(order("order-2", "private_warehouse")));
    EXPECT_EQ(field(stripe.last().body, "line_items%5B0%5D%5Bprice_data%5D%5Bproduct%5D"),
              "lake_private_warehouse");
    // An offer with no product is refused before anything is sent.
    const std::size_t sent = stripe.count();
    EXPECT_FALSE(provider->create_checkout(order("order-3", "on_prem")));
    EXPECT_EQ(stripe.count(), sent);
    // An account the operator has not given the product yet: the same message as an invoice's.
    stripe.missing_product = "lake_dedicated_cluster";
    const auto missing = provider->create_checkout(order("order-4", "dedicated_cluster"));
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("Stripe has no product lake_dedicated_cluster in this account; "
                                   "the operator creates it (spec.payments)"),
              std::string::npos)
        << missing.error();
}

TEST(OrdersCheckout, CheckoutEventsBecomeOrderStates) {
    const auto parse = [](std::string type,
                          std::string payment_status,
                          json metadata = {{"lake_order_id", "order-9"}}) {
        const json event{{"id", "evt_1"},
                         {"type", type},
                         {"data",
                          {{"object",
                            {{"id", "cs_test_9"},
                             {"object", "checkout.session"},
                             {"client_reference_id", "order-9"},
                             {"customer", "cus_9"},
                             {"subscription", "sub_9"},
                             {"payment_status", payment_status},
                             {"metadata", metadata}}}}}};
        auto notice = parse_stripe_event(event.dump(), this_lake);
        EXPECT_TRUE(notice);
        return *notice;
    };
    const PaymentNotice paid = parse("checkout.session.completed", "paid");
    EXPECT_EQ(paid.order_state, "paid");
    EXPECT_EQ(paid.checkout_session, "cs_test_9");
    EXPECT_EQ(paid.order_id, "order-9");
    EXPECT_EQ(paid.customer, "cus_9");
    EXPECT_EQ(paid.subscription, "sub_9");
    EXPECT_TRUE(paid.state.empty()) << "an order event never moves an invoice";
    EXPECT_EQ(parse("checkout.session.completed", "unpaid").order_state, "")
        << "a delayed payment waits for async_payment_succeeded";
    EXPECT_EQ(parse("checkout.session.async_payment_succeeded", "paid").order_state, "paid");
    EXPECT_EQ(parse("checkout.session.async_payment_failed", "unpaid").order_state, "cancelled");
    EXPECT_EQ(parse("checkout.session.expired", "unpaid").order_state, "cancelled");
    // Whose session it is (lake#855): lake_order_id marks the order sheet's, and lake_instance
    // which Lake's; one from before lake_instance existed is this Lake's.
    EXPECT_FALSE(paid.foreign);
    EXPECT_FALSE(parse("checkout.session.completed",
                       "paid",
                       {{"lake_order_id", "order-9"}, {"lake_instance", this_lake}})
                     .foreign);
    EXPECT_TRUE(parse("checkout.session.completed",
                      "paid",
                      {{"lake_order_id", "order-9"}, {"lake_instance", other_lake}})
                    .foreign);
    EXPECT_TRUE(parse("checkout.session.completed", "paid", json::object()).foreign)
        << "another app's session";
}

namespace {

// A checkout order recorded and its session kept, as POST /order leaves it.
lake::control::CloudOrder checkout_order(lake::control::Store& store, const std::string& session) {
    auto created = store.create_cloud_order({"dedicated_cluster",
                                             "checkout",
                                             "azure",
                                             "canadacentral",
                                             "Ada",
                                             "ada@example.com",
                                             "Acme",
                                             "",
                                             1,
                                             500'000,
                                             "usd",
                                             200'000,
                                             "",
                                             ""});
    EXPECT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->state, "pending_payment");
    auto kept = store.transition_cloud_order(
        {created->id, "pending_payment", "pending_payment", "lake", "", session});
    EXPECT_TRUE(kept) << kept.error().message;
    return *kept;
}

// A Checkout event for `session`, tagged as POST /order tags it unless `metadata` is given.
std::string checkout_event(const std::string& id,
                           const std::string& type,
                           const std::string& session,
                           const std::string& order_id,
                           const json& metadata = nullptr) {
    return json{{"id", id},
                {"type", type},
                {"data",
                 {{"object",
                   {{"id", session},
                    {"object", "checkout.session"},
                    {"client_reference_id", order_id},
                    {"customer", "cus_" + id},
                    {"subscription", "sub_" + id},
                    {"payment_status", type == "checkout.session.completed" ? "paid" : "unpaid"},
                    {"metadata",
                     metadata.is_null() ? json{{"lake_order_id", order_id},
                                               {"orama_app", "lake"},
                                               {"lake_instance", this_lake}}
                                        : metadata}}}}}}
        .dump();
}

} // namespace

TEST(OrdersCheckout, TheSignedWebhookMarksAnOrderPaidOncePerEvent) {
    FakeCheckout stripe;
    auto providers = stripe.providers();
    lake::control::InMemoryStore store;
    const auto paid_order = checkout_order(store, "cs_test_paid");
    const auto abandoned = checkout_order(store, "cs_test_abandoned");
    const auto notice = [&](const std::string& body) {
        ApiContext context;
        context.store = &store;
        context.instance_kek = std::string(kek);
        auto outcome = liquid::runtime::block_on(receive_payment_notice(
            *providers, context, body, stripe_signature_header(body, stripe_webhook, unix_now())));
        EXPECT_TRUE(outcome);
        return *outcome;
    };
    const std::string completed =
        checkout_event("evt_paid", "checkout.session.completed", "cs_test_paid", paid_order.id);
    const NoticeOutcome first = notice(completed);
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(first.body["order_id"], paid_order.id);
    EXPECT_EQ(first.body["state"], "paid");
    EXPECT_EQ(first.body["changed"], true);
    const auto after = store.get_cloud_order(paid_order.id);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->state, "paid");
    EXPECT_EQ(after->stripe_customer_id, "cus_evt_paid");
    EXPECT_EQ(after->stripe_subscription_id, "sub_evt_paid");
    // A second delivery of the same event changes nothing.
    EXPECT_EQ(notice(completed).body["duplicate"], true);
    // An abandoned checkout expires: the order is cancelled and never provisioned.
    const NoticeOutcome expired = notice(checkout_event(
        "evt_expired", "checkout.session.expired", "cs_test_abandoned", abandoned.id));
    EXPECT_EQ(expired.body["state"], "cancelled");
    // A late completion for a cancelled order does not revive it.
    const NoticeOutcome late = notice(checkout_event(
        "evt_late", "checkout.session.completed", "cs_test_abandoned", abandoned.id));
    EXPECT_EQ(late.body["changed"], false);
    EXPECT_EQ(store.get_cloud_order(abandoned.id)->state, "cancelled");
    // A session tagged as this Lake's that it did not start is acknowledged and unmatched.
    EXPECT_EQ(
        notice(checkout_event("evt_other", "checkout.session.completed", "cs_test_other", "x"))
            .body["matched"],
        false);
    // Another Lake's session, and another app's, are ignored with nothing recorded (lake#855),
    // even for a session id this Lake knows.
    const auto ignored = [&](const std::string& id, const json& metadata) {
        const NoticeOutcome outcome = notice(
            checkout_event(id, "checkout.session.completed", "cs_test_abandoned", "x", metadata));
        EXPECT_EQ(outcome.status, 200) << id;
        EXPECT_EQ(outcome.body, (json{{"received", true}, {"ignored", true}})) << id;
        const auto unused = store.apply_payment_event({"stripe", id, "checkout.session.completed"});
        EXPECT_TRUE(unused && !unused->duplicate) << id << ": no billing_payment_events row";
    };
    ignored(
        "evt_other_lake",
        {{"lake_order_id", abandoned.id}, {"orama_app", "lake"}, {"lake_instance", other_lake}});
    ignored("evt_other_app", json::object());
    // A bad signature is refused before anything is read.
    ApiContext context;
    context.store = &store;
    const auto refused = liquid::runtime::block_on(
        receive_payment_notice(*providers, context, completed, "t=1,v1=00"));
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 400);
    // Every change is an event, for the central audit log.
    const auto events = store.unaudited_cloud_order_events(100);
    ASSERT_TRUE(events);
    std::vector<std::string> moves;
    for (const auto& event : *events) {
        moves.push_back(event.actor + ":" + event.from_state + ">" + event.to_state);
    }
    EXPECT_EQ(moves,
              (std::vector<std::string>{"lake:>pending_payment",
                                        "lake:>pending_payment",
                                        "stripe:pending_payment>paid",
                                        "stripe:pending_payment>cancelled"}));
}

// ------------------------------------------------------------------------------------------------
// The cloud_orders contract, in both stores

namespace {

void expect_order_contract(lake::control::Store& store) {
    // An order form starts submitted, with its first event.
    auto form = store.create_cloud_order({"on_prem",
                                          "order_form",
                                          "",
                                          "",
                                          "Ada",
                                          "ada@example.com",
                                          "Acme",
                                          "Bare metal",
                                          1,
                                          1'000'000,
                                          "usd",
                                          400'000,
                                          "",
                                          "LAKE_RESEND_API_KEY is not set"});
    ASSERT_TRUE(form) << form.error().message;
    EXPECT_EQ(form->state, "submitted");
    EXPECT_EQ(form->email_error, "LAKE_RESEND_API_KEY is not set");
    EXPECT_EQ(form->organization_id, "");
    EXPECT_FALSE(form->created_at.empty());
    // Only the offers lake takes payment for go through Checkout.
    EXPECT_FALSE(store.create_cloud_order(
        {"on_prem", "checkout", "", "", "Ada", "ada@example.com", "", "", 1, 1, "usd", 0, "", ""}));
    const auto paid = checkout_order(store, "cs_contract_1");
    EXPECT_EQ(paid.stripe_checkout_session_id, "cs_contract_1");
    // A move from the wrong state is a conflict.
    const auto wrong =
        store.transition_cloud_order({paid.id, "paid", "provisioning", "operator", ""});
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, "conflict");
    auto applied = store.apply_cloud_order_payment({"stripe",
                                                    "evt_contract_1",
                                                    "checkout.session.completed",
                                                    "cs_contract_1",
                                                    paid.id,
                                                    "paid",
                                                    "cus_1",
                                                    "sub_1",
                                                    ""});
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied->matched);
    EXPECT_TRUE(applied->changed);
    EXPECT_EQ(applied->order.state, "paid");
    auto again = store.apply_cloud_order_payment({"stripe",
                                                  "evt_contract_1",
                                                  "checkout.session.completed",
                                                  "cs_contract_1",
                                                  paid.id,
                                                  "paid",
                                                  "",
                                                  "",
                                                  ""});
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->duplicate);
    EXPECT_FALSE(store.get_cloud_order("not-an-order"));
    EXPECT_EQ(store.get_cloud_order(lake::control::new_uuid_v4()).error().code, "not_found");

    // Email: both orders have mail due (support and customer about submitted, and about paid).
    auto claimed = store.claim_cloud_order_emails(10, 300);
    ASSERT_TRUE(claimed) << claimed.error().message;
    EXPECT_EQ(claimed->size(), 2U);
    // Held: a second pass does not take them.
    EXPECT_TRUE(store.claim_cloud_order_emails(10, 300)->empty());
    ASSERT_TRUE(store.record_cloud_order_email({form->id, "support", "submitted", true, "", 60}));
    ASSERT_TRUE(
        store.record_cloud_order_email({form->id, "customer", "submitted", false, "HTTP 500", 1}));
    const auto failed_once = store.get_cloud_order(form->id);
    EXPECT_EQ(failed_once->support_emailed_state, "submitted");
    EXPECT_EQ(failed_once->customer_emailed_state, "");
    EXPECT_EQ(failed_once->email_attempts, 1);
    EXPECT_EQ(failed_once->email_error, "HTTP 500");
    EXPECT_FALSE(failed_once->email_retry_at.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto retried = store.claim_cloud_order_emails(10, 300);
    ASSERT_TRUE(retried);
    ASSERT_EQ(retried->size(), 1U) << "only the order whose retry is due";
    EXPECT_EQ(retried->front().id, form->id);
    ASSERT_TRUE(store.record_cloud_order_email({form->id, "customer", "submitted", true, "", 60}));
    EXPECT_EQ(store.get_cloud_order(form->id)->email_attempts, 0);

    // The operator's writes: provisioning, then ready with the host.
    ASSERT_TRUE(
        store.transition_cloud_order({paid.id, "paid", "provisioning", "operator", "claimed"}));
    ASSERT_TRUE(store.transition_cloud_order(
        {paid.id, "provisioning", "ready", "operator", "Lake is Ready", "", "acme.lake.live"}));
    EXPECT_EQ(store.get_cloud_order(paid.id)->host, "acme.lake.live");

    // Events reach the audit log once.
    auto events = store.unaudited_cloud_order_events(100);
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 5U) << "two created, paid, provisioning, ready";
    EXPECT_EQ(events->back().to_state, "ready");
    EXPECT_EQ(events->back().actor, "operator");
    std::vector<std::int64_t> ids;
    for (const auto& event : *events) {
        ids.push_back(event.id);
    }
    ASSERT_TRUE(store.mark_cloud_order_events_audited(ids));
    EXPECT_TRUE(store.unaudited_cloud_order_events(100)->empty());
}

} // namespace

TEST(OrdersStore, TheMemoryStoreKeepsTheContract) {
    lake::control::InMemoryStore store;
    expect_order_contract(store);
}

TEST(OrdersStore, ThePostgresStoreKeepsTheContractAndTheOperatorsWrites) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(kek));
    expect_order_contract(*store);

    // The operator's own statements, copied from orama-operator#635
    // (internal/controller/lake_orders_pgx.go): list the paid orders, claim one, then finish it.
    // Each writes the row and its event in one statement. The trigger keeps updated_at, and lake
    // reads the result.
    const auto order = checkout_order(*store, "cs_operator_1");
    ASSERT_TRUE(store->apply_cloud_order_payment({"stripe",
                                                  "evt_operator_1",
                                                  "checkout.session.completed",
                                                  "cs_operator_1",
                                                  order.id,
                                                  "paid",
                                                  "cus",
                                                  "sub",
                                                  ""}));
    constexpr const char* list_sql =
        "SELECT id::text, offer, COALESCE(cloud_provider, ''), COALESCE(region, ''), state, "
        "COALESCE(host, ''), COALESCE(claimed_by, '') FROM cloud_orders WHERE state = 'paid' OR "
        "(claimed_by = $1 AND state IN ('provisioning', 'ready', 'failed')) ORDER BY id";
    constexpr const char* claim_sql =
        "WITH claimed AS (UPDATE cloud_orders SET state = 'provisioning', claimed_by = $2, "
        "claimed_at = now() WHERE id = $1::uuid AND state = 'paid' RETURNING id) INSERT INTO "
        "cloud_order_events (order_id, at, from_state, to_state, actor, detail) SELECT id, now(), "
        "'paid', 'provisioning', 'operator', $3 FROM claimed RETURNING order_id";
    constexpr const char* finish_sql =
        "WITH moved AS (UPDATE cloud_orders SET state = $3, host = COALESCE($4::text, host), "
        "failure_reason = COALESCE($5::text, failure_reason) WHERE id = $1::uuid AND state = "
        "'provisioning' AND claimed_by = $2 RETURNING id) INSERT INTO cloud_order_events "
        "(order_id, at, from_state, to_state, actor, detail) SELECT id, now(), 'provisioning', $3, "
        "'operator', $6 FROM moved RETURNING order_id";
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        const auto paid = tx.exec_params(list_sql, "orama-operator/0");
        ASSERT_EQ(paid.size(), 1U);
        EXPECT_EQ(paid[0][0].as<std::string>(), order.id);
        EXPECT_EQ(paid[0][1].as<std::string>(), "dedicated_cluster");
        EXPECT_EQ(paid[0][2].as<std::string>(), "azure");
        EXPECT_EQ(paid[0][3].as<std::string>(), "canadacentral");
        EXPECT_EQ(paid[0][4].as<std::string>(), "paid");
        // An order still waiting for Stripe, and an order form, are never listed.
        const auto unpaid = checkout_order(*store, "cs_operator_unpaid");
        EXPECT_EQ(tx.exec_params(claim_sql, unpaid.id, "orama-operator/0", "claimed").size(), 0U)
            << "an unpaid order cannot be claimed";
        EXPECT_EQ(tx.exec_params(claim_sql, order.id, "orama-operator/0", "claimed").size(), 1U);
        EXPECT_EQ(tx.exec_params(claim_sql, order.id, "orama-operator/1", "claimed").size(), 0U)
            << "claimed exactly once";
        EXPECT_EQ(tx.exec_params(finish_sql,
                                 order.id,
                                 "orama-operator/1",
                                 "failed",
                                 std::optional<std::string>{},
                                 std::optional<std::string>{"not mine"},
                                 "not mine")
                      .size(),
                  0U)
            << "only the claimant finishes it";
        EXPECT_EQ(tx.exec_params(finish_sql,
                                 order.id,
                                 "orama-operator/0",
                                 "failed",
                                 std::optional<std::string>{},
                                 std::optional<std::string>{"no capacity"},
                                 "no capacity")
                      .size(),
                  1U);
        const auto updated = tx.exec_params(
            "SELECT updated_at > created_at FROM cloud_orders WHERE id = $1::uuid", order.id);
        EXPECT_TRUE(updated[0][0].as<bool>()) << "updated_at follows the operator's write";
        tx.commit();
    }
    const auto seen = store->get_cloud_order(order.id);
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->state, "failed");
    EXPECT_EQ(seen->claimed_by, "orama-operator/0");
    EXPECT_FALSE(seen->claimed_at.empty());
    EXPECT_EQ(seen->failure_reason, "no capacity");
    // The schema refuses what the contract does not allow.
    pqxx::work tx(connection);
    EXPECT_THROW(
        tx.exec_params("UPDATE cloud_orders SET state = 'shipped' WHERE id = $1::uuid", order.id),
        pqxx::check_violation);
}

TEST(OrdersStore, ThePostgresSchemaRefusesAPaidOrderForMoreThanOne) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(kek));
    // One host per order: a paid order is one warehouse or one cluster.
    const auto three = store->create_cloud_order({"dedicated_cluster",
                                                  "checkout",
                                                  "azure",
                                                  "canadacentral",
                                                  "Ada",
                                                  "ada@example.com",
                                                  "",
                                                  "",
                                                  3,
                                                  1'500'000,
                                                  "usd",
                                                  600'000,
                                                  "",
                                                  ""});
    EXPECT_FALSE(three);
    lake::control::InMemoryStore memory;
    EXPECT_FALSE(memory.create_cloud_order({"dedicated_cluster",
                                            "checkout",
                                            "azure",
                                            "canadacentral",
                                            "Ada",
                                            "ada@example.com",
                                            "",
                                            "",
                                            3,
                                            1'500'000,
                                            "usd",
                                            600'000,
                                            "",
                                            ""}));
}

// ------------------------------------------------------------------------------------------------
// Over HTTP

namespace {

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers; // names lowercased
    std::string body;
};

Response send(unsigned short port,
              std::string_view method,
              std::string_view target,
              std::string_view host,
              const std::map<std::string, std::string>& headers = {},
              std::string_view body = {}) {
    Response parsed;
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return parsed;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return parsed;
    }
    std::string wire = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\n";
    wire += "Host: " + std::string(host) + "\r\nConnection: close\r\n";
    for (const auto& [name, value] : headers) {
        wire += name + ": " + value + "\r\n";
    }
    if (!body.empty() || method == "POST") {
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    wire += "\r\n" + std::string(body);
    for (std::size_t sent = 0; sent < wire.size();) {
        const ssize_t count = ::send(socket_fd, wire.data() + sent, wire.size() - sent, 0);
        if (count <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || response.size() < 12) {
        return parsed;
    }
    parsed.status = std::stoi(response.substr(9, 3));
    std::size_t line = response.find("\r\n") + 2;
    while (line < header_end) {
        const std::size_t end = response.find("\r\n", line);
        const std::size_t colon = response.find(':', line);
        if (colon < end) {
            std::string name = response.substr(line, colon - line);
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            parsed.headers[name] = response.substr(colon + 2, end - colon - 2);
        }
        line = end + 2;
    }
    parsed.body = response.substr(header_end + 4);
    return parsed;
}

const std::map<std::string, std::string> form_headers{
    {"Content-Type", "application/x-www-form-urlencoded"}, {"X-Real-IP", "203.0.113.7"}};

struct OrderRig {
    FakeCheckout stripe;
    FakeResend resend;
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    std::unique_ptr<ApiServer> server;

    explicit OrderRig(bool with_resend = true,
                      int per_client = 5,
                      bool billing = true,
                      int per_hour = 60,
                      std::string_view trusted_proxies = "",
                      bool proxy_listener = false) {
        ServerOptions options;
        options.port = 0;
        options.store = store;
        options.instance_kek = std::string(kek);
        options.regions = production();
        options.billing = billing;
        options.payment_providers = stripe.providers();
        options.order_sheet.orders_per_client_per_hour = per_client;
        options.order_sheet.orders_per_hour = per_hour;
        if (with_resend) {
            options.resend = resend_settings();
        }
        options.resend_http = resend.http();
        options.order_pass_interval = std::chrono::milliseconds(0);
        options.trusted_proxies = *lake::api::parse_trusted_proxies(trusted_proxies);
        if (proxy_listener) {
            options.proxy_port = 0;
        }
        server = std::make_unique<ApiServer>(options);
        EXPECT_TRUE(server->start()) << server->reason();
    }
    ~OrderRig() {
        server->stop();
    }

    unsigned short port() const {
        return server->port();
    }

    // Runs the order pass until `done` holds, at most five seconds.
    template <class Done> bool pass_until(Done done) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (done()) {
                return true;
            }
        }
        return false;
    }

    lake::control::CloudOrder only_order() {
        // The store has one order whose events are the first ones; find it through its events.
        const auto events = store->unaudited_cloud_order_events(1000);
        EXPECT_TRUE(events && !events->empty());
        return *store->get_cloud_order(events->back().order_id);
    }
};

std::string order_id_in(const std::string& html) {
    const std::size_t at = html.find("<p class=\"eyebrow\">Order ");
    return at == std::string::npos ? std::string() : html.substr(at + 25, 36);
}

} // namespace

TEST(OrdersApex, TheSheetShowsEveryOptionAndKeepsTheRegionChoice) {
    OrderRig rig;
    const Response page =
        send(rig.port(), "GET", "/w/abc?tab=1", "lake.live", {{"CF-IPCountry", "US"}});
    ASSERT_EQ(page.status, 200) << page.body;
    EXPECT_EQ(page.headers.at("cache-control"), "no-store");
    for (const std::string_view expected :
         {std::string_view("<h1>Get Lake</h1>"),
          std::string_view("id=\"shared\""),
          std::string_view("id=\"private-warehouse\""),
          std::string_view("id=\"dedicated-cluster\""),
          std::string_view("id=\"on-prem\""),
          std::string_view("$1,000 <small>USD / month</small>"),
          std::string_view("$5,000 <small>USD / month</small>"),
          std::string_view("<small>Starting from</small> $10,000"),
          std::string_view("Includes $400 USD of usage credit each month"),
          std::string_view("Includes $2,000 USD of usage credit each month"),
          std::string_view("Includes at least $4,000 USD of usage credit"),
          std::string_view("billed at the published price list"),
          std::string_view("Prices are in US dollars (USD)"),
          std::string_view("SOC 2 compliant"),
          std::string_view("Source available"),
          std::string_view("Full service (Metis)"),
          std::string_view("multi-region deployments with geo-based load balancing and failover"),
          std::string_view("Canada"),
          std::string_view("Asia"),
          std::string_view("href=\"/order/price-list\""),
          std::string_view("href=\"/order/dedicated-cluster?location=other\">Contact us"),
          std::string_view("href=\"/order/on-prem\">Send an order form")}) {
        EXPECT_NE(page.body.find(expected), std::string::npos) << expected;
    }
    // Backups on every option.
    std::size_t backups = 0;
    for (std::size_t at = page.body.find("Backups included"); at != std::string::npos;
         at = page.body.find("Backups included", at + 1)) {
        ++backups;
    }
    EXPECT_EQ(backups, 4U);
    // Their own domain, white-labelled, on the three paid cards and not on the shared one, which
    // comes first (lake#1129).
    constexpr std::string_view white_label_line =
        "Your own domain, white-labelled (available on request)";
    const std::size_t paid_cards = page.body.find("id=\"private-warehouse\"");
    ASSERT_NE(paid_cards, std::string::npos);
    std::size_t white_label = 0;
    for (std::size_t at = page.body.find(white_label_line); at != std::string::npos;
         at = page.body.find(white_label_line, at + 1)) {
        EXPECT_GT(at, paid_cards) << "the shared card must not offer a white-labelled domain";
        ++white_label;
    }
    EXPECT_EQ(white_label, 3U);
    // The lead line is off by default.
    EXPECT_EQ(page.body.find("Train and serve a model"), std::string::npos);
    // The shared choice is exactly the picker's: preselected by country, posting back to the path.
    EXPECT_NE(page.body.find("value=\"eastus\" checked>"), std::string::npos);
    EXPECT_NE(page.body.find("action=\"/w/abc?tab=1\""), std::string::npos);
    const Response chosen = send(
        rig.port(),
        "POST",
        "/w/abc?tab=1",
        "lake.live",
        {{"Content-Type", "application/x-www-form-urlencoded"}, {"X-Forwarded-Proto", "https"}},
        "region=eastus");
    EXPECT_EQ(chosen.status, 302);
    EXPECT_EQ(chosen.headers.at("location"), "https://us.lake.live/w/abc?tab=1");
    EXPECT_EQ(chosen.headers.at("set-cookie"),
              "lake_region=eastus; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax; Secure");
    // An unknown offer path is the sheet, like every other path.
    EXPECT_NE(send(rig.port(), "GET", "/order/gold", "lake.live").body.find("<h1>Get Lake</h1>"),
              std::string::npos);
    // server_info points the web app at the sheet.
    const Response info = send(rig.port(), "GET", "/v1/server_info", "eu.lake.live");
    EXPECT_EQ(json::parse(info.body)["value"]["order_sheet_url"], "https://lake.live/");
    // The price list page answers before one is published.
    const Response prices = send(rig.port(), "GET", "/order/price-list", "lake.live");
    EXPECT_EQ(prices.status, 200);
    EXPECT_NE(prices.body.find("not published yet"), std::string::npos);
}

TEST(OrdersApex, ThePromiseShowsOnlyWhenSwitchedOn) {
    const Regions regions = production();
    auto sheet = order_sheet_from_environment(R"({"shared":{"show_promise":true}})");
    ASSERT_TRUE(sheet);
    EXPECT_NE(web_ui_order_sheet(*sheet, regions, regions.list[0], "/", "")
                  .find("Train and serve a model in 5 minutes for under $5"),
              std::string::npos);
    // Off: the plain picker of lake#757.
    auto off = order_sheet_from_environment(R"({"enabled":false})");
    ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.regions = regions;
    options.order_sheet = *off;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const Response page = send(server.port(), "GET", "/", "lake.live");
    EXPECT_NE(page.body.find("<h1>Choose a Region</h1>"), std::string::npos);
    EXPECT_NE(
        send(server.port(), "GET", "/order/on-prem", "lake.live").body.find("Choose a Region"),
        std::string::npos);
    server.stop();
}

TEST(OrdersApex, AnOrderFormIsRecordedAndEmailedAndNeverProvisioned) {
    OrderRig rig;
    const Response form = send(rig.port(), "GET", "/order/on-prem", "lake.live");
    ASSERT_EQ(form.status, 200);
    EXPECT_NE(form.body.find("action=\"/order\""), std::string::npos);
    EXPECT_NE(form.body.find("name=\"website\""), std::string::npos) << "the field bots fill";
    EXPECT_EQ(form.body.find("value=\"checkout\""), std::string::npos) << "on-prem is a form only";
    const Response sent = send(
        rig.port(),
        "POST",
        "/order",
        "lake.live",
        form_headers,
        "offer=on_prem&path=order_form&location=&contact_name=Ada+Lovelace&contact_email=ada%40"
        "example.com&company=Analytical+Engines&details=Bare+metal%2C+H100s%2C+two+regions&"
        "quantity=1&website=");
    ASSERT_EQ(sent.status, 200) << sent.body;
    EXPECT_NE(sent.body.find("<h1>Order Received</h1>"), std::string::npos);
    const std::string id = order_id_in(sent.body);
    ASSERT_EQ(id.size(), 36U) << sent.body;
    const auto order = rig.store->get_cloud_order(id);
    ASSERT_TRUE(order);
    EXPECT_EQ(order->state, "submitted");
    EXPECT_EQ(order->path, "order_form");
    EXPECT_EQ(order->details, "Bare metal, H100s, two regions");
    EXPECT_EQ(rig.stripe.count(), 0U) << "an order form never reaches Stripe";
    // The status link on the page works.
    const std::string token = order_status_token(kek, id);
    EXPECT_NE(sent.body.find("/orders/" + id + "?token=" + token), std::string::npos);
    // Support and the customer are emailed with the order id, once each.
    ASSERT_TRUE(rig.pass_until([&] {
        const auto now = rig.store->get_cloud_order(id);
        return now->support_emailed_state == "submitted" &&
               now->customer_emailed_state == "submitted";
    }));
    const auto bodies = rig.resend.bodies();
    ASSERT_EQ(bodies.size(), 2U);
    EXPECT_EQ(bodies[0]["to"], json::array({"support@orama.com"}));
    EXPECT_NE(bodies[0]["text"].get<std::string>().find("Order: " + id), std::string::npos);
    EXPECT_NE(bodies[0]["text"].get<std::string>().find("Bare metal, H100s"), std::string::npos);
    EXPECT_EQ(bodies[1]["to"], json::array({"ada@example.com"}));
    EXPECT_NE(bodies[1]["text"].get<std::string>().find("https://lake.live/orders/" + id),
              std::string::npos);
    // Every event reached the central audit log.
    ASSERT_TRUE(
        rig.pass_until([&] { return rig.store->unaudited_cloud_order_events(10)->empty(); }));
    lake::audit::Filter filter;
    filter.object = "cloud_order/" + id;
    const auto audited = rig.store->list_audit_events(filter);
    ASSERT_TRUE(audited);
    ASSERT_EQ(audited->size(), 1U);
    EXPECT_EQ(audited->front().draft.kind, "cloud_order.submitted");
    EXPECT_EQ(audited->front().draft.principal, "lake");
    // The status page.
    const Response status =
        send(rig.port(), "GET", "/orders/" + id + "?token=" + token, "lake.live");
    EXPECT_EQ(status.status, 200);
    EXPECT_NE(status.body.find("<h1>Order Received</h1>"), std::string::npos);
    EXPECT_EQ(send(rig.port(), "GET", "/orders/" + id + "?token=nope", "lake.live").status, 404);
    EXPECT_EQ(send(rig.port(), "GET", "/orders/" + id, "lake.live").status, 404);
}

TEST(OrdersApex, APaidOrderGoesThroughCheckoutAndTheWebhook) {
    OrderRig rig;
    const Response form = send(
        rig.port(), "GET", "/order/dedicated-cluster?location=azure%3Acanadacentral", "lake.live");
    EXPECT_NE(form.body.find("value=\"azure:canadacentral\" selected"), std::string::npos);
    EXPECT_NE(form.body.find("value=\"checkout\">Pay $5,000 USD / month with card"),
              std::string::npos);
    EXPECT_NE(form.body.find("Amazon Web Services (order form)"), std::string::npos);
    const Response paid =
        send(rig.port(),
             "POST",
             "/order",
             "lake.live",
             form_headers,
             "offer=dedicated_cluster&path=checkout&location=azure%3Acanadacentral&"
             "contact_name=Ada&contact_email=ada%40example.com&quantity=1");
    ASSERT_EQ(paid.status, 302) << paid.body;
    EXPECT_EQ(paid.headers.at("location"), "https://checkout.stripe.test/c/cs_test_1");
    const auto order = rig.only_order();
    EXPECT_EQ(order.state, "pending_payment");
    EXPECT_EQ(order.stripe_checkout_session_id, "cs_test_1");
    EXPECT_EQ(order.amount_cents, 500'000);
    EXPECT_EQ(order.included_credit_cents, 200'000);
    const std::string token = order_status_token(kek, order.id);
    EXPECT_EQ(field(rig.stripe.last().body, "success_url"),
              "https%3A%2F%2Flake.live%2Forders%2F" + order.id + "%3Ftoken%3D" + token +
                  "%26checkout%3Ddone");
    // Before Stripe says so, nothing is emailed: the order waits for payment.
    rig.server->drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(rig.resend.bodies().empty());
    const Response waiting = send(rig.port(),
                                  "GET",
                                  "/orders/" + order.id + "?token=" + token + "&checkout=cancelled",
                                  "lake.live");
    EXPECT_NE(waiting.body.find("The payment was not completed"), std::string::npos);
    // Stripe's signed webhook, on the region's host, marks it paid.
    const std::string body =
        checkout_event("evt_http_paid", "checkout.session.completed", "cs_test_1", order.id);
    const Response hook =
        send(rig.port(),
             "POST",
             "/billing/stripe/webhook",
             "eu.lake.live",
             {{"Content-Type", "application/json"},
              {"Stripe-Signature", stripe_signature_header(body, stripe_webhook, unix_now())}},
             body);
    ASSERT_EQ(hook.status, 200) << hook.body;
    EXPECT_EQ(json::parse(hook.body)["state"], "paid");
    EXPECT_EQ(rig.store->get_cloud_order(order.id)->state, "paid");
    ASSERT_TRUE(rig.pass_until(
        [&] { return rig.store->get_cloud_order(order.id)->customer_emailed_state == "paid"; }));
    // The operator provisions it (orama-operator#634) and the customer hears it is ready.
    ASSERT_TRUE(
        rig.store->transition_cloud_order({order.id, "paid", "provisioning", "operator", ""}));
    ASSERT_TRUE(rig.store->transition_cloud_order(
        {order.id, "provisioning", "ready", "operator", "", "", "acme.lake.live"}));
    ASSERT_TRUE(rig.pass_until(
        [&] { return rig.store->get_cloud_order(order.id)->customer_emailed_state == "ready"; }));
    const auto bodies = rig.resend.bodies();
    ASSERT_EQ(bodies.size(), 3U) << "support and customer on paid, the customer on ready";
    EXPECT_NE(bodies[2]["subject"].get<std::string>().find("is ready"), std::string::npos);
    EXPECT_NE(bodies[2]["text"].get<std::string>().find("https://acme.lake.live/"),
              std::string::npos);
    const Response ready =
        send(rig.port(), "GET", "/orders/" + order.id + "?token=" + token, "lake.live");
    EXPECT_NE(ready.body.find("href=\"https://acme.lake.live/\""), std::string::npos);
}

TEST(OrdersApex, AFailedCheckoutCancelsTheOrderAndOffersTheForm) {
    OrderRig rig;
    rig.stripe.fail = true;
    const Response refused = send(rig.port(),
                                  "POST",
                                  "/order",
                                  "lake.live",
                                  form_headers,
                                  "offer=private_warehouse&path=checkout&location=eastus&"
                                  "contact_name=Ada&contact_email=ada%40example.com");
    EXPECT_EQ(refused.status, 503);
    EXPECT_NE(refused.body.find("Card payment could not start"), std::string::npos);
    EXPECT_EQ(rig.only_order().state, "cancelled");
    // Without Stripe at all (billing off), the pay button is not offered and checkout is refused.
    OrderRig manual(true, 5, false);
    const Response form = send(manual.port(), "GET", "/order/private-warehouse", "lake.live");
    EXPECT_EQ(form.body.find("value=\"checkout\""), std::string::npos);
    EXPECT_NE(form.body.find("Card payment is not available right now"), std::string::npos);
    const Response no_card = send(manual.port(),
                                  "POST",
                                  "/order",
                                  "lake.live",
                                  form_headers,
                                  "offer=private_warehouse&path=checkout&location=eastus&"
                                  "contact_name=Ada&contact_email=ada%40example.com");
    EXPECT_EQ(no_card.status, 400);
}

// lake#838: order forms that used up the hour's cap, from many addresses, cannot block a checkout
// order: it is still recorded and sent to Stripe.
TEST(OrdersApex, OrderFormsCannotUseUpTheCheckoutCap) {
    OrderRig rig(true, 100, true, 2);
    const std::string order_form = "offer=on_prem&path=order_form&contact_name=Ada&contact_email="
                                   "ada%40example.com&details=Bare+metal";
    for (const char* address : {"198.51.100.11", "198.51.100.12"}) {
        auto headers = form_headers;
        headers["X-Real-IP"] = address;
        EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", headers, order_form).status, 200);
    }
    auto spammer = form_headers;
    spammer["X-Real-IP"] = "198.51.100.13";
    EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", spammer, order_form).status, 429)
        << "the order forms' cap is used up";
    auto customer = form_headers;
    customer["X-Real-IP"] = "203.0.113.50";
    const Response paid =
        send(rig.port(),
             "POST",
             "/order",
             "lake.live",
             customer,
             "offer=dedicated_cluster&path=checkout&location=azure%3Acanadacentral&"
             "contact_name=Bo&contact_email=bo%40example.com&quantity=1");
    ASSERT_EQ(paid.status, 302) << paid.body;
    EXPECT_EQ(paid.headers.at("location"), "https://checkout.stripe.test/c/cs_test_1");
}

TEST(OrdersApex, AbuseIsRefused) {
    OrderRig rig(true, 2);
    const std::string order = "offer=on_prem&path=order_form&contact_name=Ada&contact_email=ada%40"
                              "example.com&details=Bare+metal";
    // The field only a bot fills: answered as received, nothing recorded or sent.
    const Response bot = send(rig.port(),
                              "POST",
                              "/order",
                              "lake.live",
                              form_headers,
                              order + "&website=http%3A%2F%2Fspam.example");
    EXPECT_EQ(bot.status, 200);
    EXPECT_TRUE(rig.store->unaudited_cloud_order_events(10)->empty());
    // Two an hour from one address, then 429.
    EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", form_headers, order).status, 200);
    EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", form_headers, order).status, 200);
    EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", form_headers, order).status, 429);
    // lake#838: headers a sender can write do not move it into a fresh bucket; only the address the
    // ingress set (X-Real-IP) does.
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"CF-Connecting-IP", "198.51.100.1"},
             {"X-Forwarded-For", "198.51.100.2"},
             {"X-Forwarded-For", "198.51.100.3, 203.0.113.7"}}) {
        auto forged = form_headers;
        forged[name] = value;
        EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", forged, order).status, 429)
            << name << ": " << value;
    }
    auto other = form_headers;
    other["X-Real-IP"] = "198.51.100.9";
    EXPECT_EQ(send(rig.port(), "POST", "/order", "lake.live", other, order).status, 200);
    // An invalid form is answered with the form and its errors, and records nothing.
    const Response invalid = send(rig.port(),
                                  "POST",
                                  "/order",
                                  "lake.live",
                                  other,
                                  "offer=on_prem&path=order_form&contact_name=&contact_email=x");
    EXPECT_EQ(invalid.status, 400);
    EXPECT_NE(invalid.body.find("Enter your name"), std::string::npos);
    EXPECT_NE(invalid.body.find("aria-invalid=\"true\""), std::string::npos);
    // A body over 32 KiB is refused.
    const Response large = send(rig.port(),
                                "POST",
                                "/order",
                                "lake.live",
                                other,
                                order + "&details=" + std::string(40 * 1024, 'x'));
    EXPECT_EQ(large.status, 413);
    EXPECT_EQ(rig.store->unaudited_cloud_order_events(10)->size(), 3U);
}

// lake#1006 (LSR-200): hosted code that reaches lake directly (a workspace pod on the main port)
// cannot rotate X-Real-IP or X-Forwarded-For into fresh order buckets. Its peer is not a trusted
// proxy, so every order counts against its own address: the per-client limit holds and the
// in-all budget is never used up by it. Only the proxy listener, which only the ingress reaches,
// names a client address.
TEST(OrdersApex, ForwardingHeadersFromAnUntrustedPeerNeverMakeAFreshBucket) {
    OrderRig rig(true, 2, true, 60, "none", true);
    ASSERT_NE(rig.server->proxy_port(), 0);
    const std::string order = "offer=on_prem&path=order_form&contact_name=Ada&contact_email=ada%40"
                              "example.com&details=Bare+metal";
    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        auto rotated = form_headers;
        rotated["X-Real-IP"] = "198.51.100." + std::to_string(i + 1);
        rotated["X-Forwarded-For"] = "203.0.113." + std::to_string(i + 1);
        const int status = send(rig.port(), "POST", "/order", "lake.live", rotated, order).status;
        EXPECT_TRUE(status == 200 || status == 429) << status;
        accepted += status == 200 ? 1 : 0;
    }
    EXPECT_EQ(accepted, 2) << "ten rotated addresses from one peer are one client";
    // Through the proxy listener the same header is the ingress's word: a new visitor.
    auto visitor = form_headers;
    visitor["X-Real-IP"] = "198.51.100.200";
    EXPECT_EQ(
        send(rig.server->proxy_port(), "POST", "/order", "lake.live", visitor, order).status, 200);
}

TEST(OrdersApex, WithoutTheResendKeyTheOrderIsKeptAndItsEmailWaits) {
    OrderRig rig(false);
    const Response sent = send(rig.port(),
                               "POST",
                               "/order",
                               "lake.live",
                               form_headers,
                               "offer=on_prem&path=order_form&contact_name=Ada&contact_email=ada%40"
                               "example.com&details=Bare+metal");
    ASSERT_EQ(sent.status, 200) << sent.body;
    EXPECT_NE(sent.body.find("<h1>Order Received</h1>"), std::string::npos);
    const auto order = rig.store->get_cloud_order(order_id_in(sent.body));
    ASSERT_TRUE(order);
    EXPECT_EQ(order->state, "submitted");
    EXPECT_EQ(order->email_error, "LAKE_RESEND_API_KEY is not set");
    ASSERT_TRUE(
        rig.pass_until([&] { return rig.store->unaudited_cloud_order_events(10)->empty(); }));
    EXPECT_TRUE(rig.resend.bodies().empty());
    EXPECT_EQ(rig.store->get_cloud_order(order->id)->support_emailed_state, "")
        << "still due: it goes once the key is set";
}

TEST(OrdersApex, AFailedEmailIsRetried) {
    OrderRig rig;
    rig.resend.failures_left = 1;
    const Response sent = send(rig.port(),
                               "POST",
                               "/order",
                               "lake.live",
                               form_headers,
                               "offer=on_prem&path=order_form&contact_name=Ada&contact_email=ada%40"
                               "example.com&details=Bare+metal");
    const std::string id = order_id_in(sent.body);
    ASSERT_TRUE(
        rig.pass_until([&] { return rig.store->get_cloud_order(id)->email_attempts == 1; }));
    EXPECT_EQ(rig.store->get_cloud_order(id)->email_error,
              "Resend refused the email (HTTP 500): try again");
}

TEST(OrdersApex, AnAsiaClusterCannotBePaidForAndGoesToTheOrderForm) {
    OrderRig rig;
    // The form lists Tokyo and Singapore, marked for the order form.
    const Response form = send(rig.port(), "GET", "/order/dedicated-cluster", "lake.live");
    EXPECT_NE(form.body.find("value=\"azure:japaneast\">Asia (Tokyo) (order form: we'll confirm "
                             "availability)</option>"),
              std::string::npos);
    EXPECT_NE(form.body.find("value=\"azure:canadacentral\">Canada (Toronto)</option>"),
              std::string::npos);
    // Paying for Tokyo is refused before anything is recorded or sent to Stripe.
    const Response refused = send(rig.port(), "POST", "/order", "lake.live", form_headers,
                                  "offer=dedicated_cluster&path=checkout&location=azure%3Ajapaneast&"
                                  "contact_name=Ada&contact_email=ada%40example.com");
    EXPECT_EQ(refused.status, 400);
    EXPECT_NE(refused.body.find("This region is ordered with the order form: we will confirm "
                                "availability."),
              std::string::npos);
    EXPECT_NE(refused.body.find("value=\"azure:japaneast\" selected"), std::string::npos)
        << "the form keeps the choice, ready to send as an order form";
    EXPECT_EQ(rig.stripe.count(), 0U);
    EXPECT_TRUE(rig.store->unaudited_cloud_order_events(10)->empty());
    // The same order sent as an order form is recorded and never reaches Stripe.
    const Response sent = send(rig.port(), "POST", "/order", "lake.live", form_headers,
                               "offer=dedicated_cluster&path=order_form&location=azure%3Ajapaneast&"
                               "contact_name=Ada&contact_email=ada%40example.com");
    ASSERT_EQ(sent.status, 200) << sent.body;
    const auto order = rig.store->get_cloud_order(order_id_in(sent.body));
    ASSERT_TRUE(order);
    EXPECT_EQ(order->state, "submitted");
    EXPECT_EQ(order->cloud_provider, "azure");
    EXPECT_EQ(order->region, "japaneast");
    EXPECT_EQ(rig.stripe.count(), 0U);
}

TEST(OrdersStore, BoundedTextIsNeverCutInsideAUtf8Sequence) {
    using lake::control::utf8_prefix;
    // "é" is two bytes: a cut at 2 would leave half of it.
    EXPECT_EQ(utf8_prefix("a\xC3\xA9", 2), "a");
    EXPECT_EQ(utf8_prefix("a\xC3\xA9", 3), "a\xC3\xA9");
    // "€" is three bytes.
    EXPECT_EQ(utf8_prefix("\xE2\x82\xAC\xE2\x82\xAC", 5), "\xE2\x82\xAC");
    EXPECT_EQ(utf8_prefix("plain", 3), "pla");
    EXPECT_EQ(utf8_prefix("", 3), "");
    // An email error of 1,023 ASCII bytes and a two-byte character is cut before the character,
    // not inside it (both stores use utf8_prefix for the error and the event detail).
    lake::control::InMemoryStore store;
    const auto created = store.create_cloud_order({"on_prem",
                                                   "order_form",
                                                   "",
                                                   "",
                                                   "Ada",
                                                   "ada@example.com",
                                                   "",
                                                   "x",
                                                   1,
                                                   1'000'000,
                                                   "usd",
                                                   400'000,
                                                   "",
                                                   std::string(1023, 'e') + "\xC3\xA9"});
    ASSERT_TRUE(created);
    EXPECT_EQ(created->email_error, std::string(1023, 'e'));
}

namespace {

json subscription_event(const lake::control::CloudOrder& order,
                        const std::string& id,
                        const std::string& type,
                        const std::string& status,
                        std::int64_t created,
                        std::int64_t end) {
    return {{"id", id},
            {"type", type},
            {"created", created},
            {"data",
             {{"object",
               {{"id", order.stripe_subscription_id},
                {"customer", order.stripe_customer_id},
                {"status", status},
                {"current_period_end", end},
                {"trial_end", end},
                {"metadata",
                 {{"lake_order_id", order.id},
                  {"lake_instance", this_lake},
                  {"orama_app", "lake"}}}}}}}};
}

json renewal_event(const lake::control::CloudOrder& order,
                   const std::string& id,
                   const std::string& type,
                   std::int64_t created,
                   std::int64_t end,
                   bool modern = false) {
    json details{
        {"subscription", order.stripe_subscription_id},
        {"metadata",
         {{"lake_order_id", order.id}, {"lake_instance", this_lake}, {"orama_app", "lake"}}}};
    json object{{"id", "in_" + id},
                {"customer", order.stripe_customer_id},
                {"subscription", order.stripe_subscription_id},
                {"lines",
                 {{"has_more", false},
                  {"data",
                   json::array({{{"type", "subscription"},
                                 {"period", {{"start", end - 100}, {"end", end}}}}})}}}};
    if (modern) {
        object["parent"] = {{"type", "subscription_details"}, {"subscription_details", details}};
        object["lines"]["data"][0].erase("type");
        object["lines"]["data"][0]["parent"] = {{"type", "subscription_item_details"}};
        object.erase("subscription");
    } else {
        object["subscription_details"] = details;
    }
    return {{"id", id}, {"type", type}, {"created", created}, {"data", {{"object", object}}}};
}

void expect_order_subscription_contract(lake::control::Store& store) {
    FakeCheckout stripe;
    auto providers = stripe.providers();
    ApiContext context;
    context.store = &store;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto deliver = [&](const json& event) {
        const auto body = event.dump();
        auto reply = liquid::runtime::block_on(receive_payment_notice(
            *providers, context, body, stripe_signature_header(body, stripe_webhook, now)));
        EXPECT_TRUE(reply);
        EXPECT_EQ(reply->status, 200) << reply->body;
        return reply->body;
    };
    const auto paid_order = [&](const std::string& key) {
        auto order = checkout_order(store, "cs_" + key);
        deliver(json::parse(
            checkout_event("evt_" + key, "checkout.session.completed", "cs_" + key, order.id)));
        EXPECT_TRUE(
            store.transition_cloud_order({order.id, "paid", "provisioning", "operator", ""}));
        EXPECT_TRUE(store.transition_cloud_order(
            {order.id, "provisioning", "ready", "operator", "", "", "acme.lake.live"}));
        return *store.get_cloud_order(order.id);
    };
    auto order = paid_order("entitled");
    auto paid = renewal_event(order, "evt_first_month", "invoice.paid", now, now + 100);
    EXPECT_EQ(deliver(paid).value("changed", false), true);
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.paid_through, now + 100);
    const auto duplicate = deliver(paid);
    EXPECT_TRUE(duplicate.value("duplicate", false));

    auto failure = renewal_event(
        order, "evt_renewal_failed", "invoice.payment_failed", now + 1, now + 200, true);
    failure["data"]["object"]["next_payment_attempt"] = now + 150;
    deliver(failure);
    auto entitlement = store.get_cloud_order(order.id)->entitlement;
    EXPECT_EQ(entitlement.subscription_status, "past_due");
    EXPECT_EQ(entitlement.paid_through, now + 100);
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 149), "active");
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 150), "grace");
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 150 + 3 * 86400),
              "suspended");

    // Subscription snapshots must retain an already-known retry window. Stripe Automations
    // publishes its next attempt on invoice.updated rather than invoice.payment_failed.
    deliver(subscription_event(order,
                               "evt_retry_status",
                               "customer.subscription.updated",
                               "past_due",
                               now + 1,
                               now + 200));
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.access_until, now + 150);
    auto retry_update =
        renewal_event(order, "evt_retry_update", "invoice.updated", now + 1, now + 200, true);
    retry_update["data"]["object"]["id"] = failure["data"]["object"]["id"];
    retry_update["data"]["object"]["status"] = "open";
    retry_update["data"]["object"]["attempted"] = true;
    retry_update["data"]["object"]["next_payment_attempt"] = now + 160;
    deliver(retry_update);
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.access_until, now + 160);
    auto next_month =
        renewal_event(order, "evt_next_unpaid_month", "invoice.payment_failed", now + 1, now + 500);
    next_month["data"]["object"]["next_payment_attempt"] = now + 400;
    deliver(next_month);
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.access_until, now + 160);
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.retry_invoice_id,
              failure["data"]["object"]["id"].get<std::string>());

    // A successful retry restores access without recreating the order or discarding its host.
    deliver(renewal_event(order, "evt_renewal_paid", "invoice.paid", now + 2, now + 200, true));
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.paid_through, now + 200);
    EXPECT_EQ(store.get_cloud_order(order.id)->host, "acme.lake.live");
    auto ended = subscription_event(
        order, "evt_deleted", "customer.subscription.deleted", "canceled", now + 3, now + 200);
    ended["data"]["object"]["ended_at"] = now + 3;
    deliver(ended);
    entitlement = store.get_cloud_order(order.id)->entitlement;
    EXPECT_EQ(entitlement.subscription_status, "canceled");
    EXPECT_EQ(entitlement.access_until, now + 3);
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 3 + 3 * 86400),
              "suspended");
    // A delayed successful invoice, stale active snapshot, or duplicate cannot revive a deleted
    // subscription.
    deliver(renewal_event(order, "evt_late_payment", "invoice.paid", now + 4, now + 500));
    deliver(subscription_event(
        order, "evt_stale_active", "customer.subscription.updated", "active", now + 2, now + 500));
    deliver(ended);
    EXPECT_EQ(store.get_cloud_order(order.id)->entitlement.access_until, now + 3);
    EXPECT_EQ(store.get_cloud_order(order.id)->state, "ready");

    // Incorrect subscription, order, customer, instance, and signatures cannot modify entitlement.
    auto unrelated = paid_order("unrelated");
    auto wrong = renewal_event(unrelated, "evt_wrong_order", "invoice.paid", now + 10, now + 1000);
    wrong["data"]["object"]["subscription_details"]["metadata"]["lake_order_id"] = order.id;
    EXPECT_FALSE(deliver(wrong).value("matched", true));
    wrong = renewal_event(unrelated, "evt_wrong_customer", "invoice.paid", now + 10, now + 1000);
    wrong["data"]["object"]["customer"] = "cus_someone_else";
    EXPECT_FALSE(deliver(wrong).value("matched", true));
    wrong =
        renewal_event(unrelated, "evt_wrong_subscription", "invoice.paid", now + 10, now + 1000);
    wrong["data"]["object"]["subscription"] = "sub_someone_else";
    wrong["data"]["object"]["subscription_details"]["subscription"] = "sub_someone_else";
    EXPECT_FALSE(deliver(wrong).value("matched", true));
    wrong =
        renewal_event(unrelated, "evt_wrong_instance", "invoice.paid", now + 10, now + 1000, true);
    wrong["data"]["object"]["parent"]["subscription_details"]["metadata"]["lake_instance"] =
        other_lake;
    EXPECT_TRUE(deliver(wrong).value("ignored", false));
    const auto forged = liquid::runtime::block_on(
        receive_payment_notice(*providers, context, paid.dump(), "t=1,v1=00"));
    ASSERT_TRUE(forged);
    EXPECT_EQ(forged->status, 400);
    EXPECT_EQ(store.get_cloud_order(unrelated.id)->entitlement.paid_through, 0);

    // A trial ends by the clock, and an unpaid subscription regains entitlement only on a newer
    // signed event.
    context.order_entitlement_grace_seconds = 7;
    deliver(subscription_event(
        unrelated, "evt_trial", "customer.subscription.updated", "trialing", now + 11, now + 20));
    entitlement = store.get_cloud_order(unrelated.id)->entitlement;
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 27), "suspended");
    deliver(subscription_event(
        unrelated, "evt_unpaid", "customer.subscription.updated", "unpaid", now + 21, now + 200));
    EXPECT_EQ(store.get_cloud_order(unrelated.id)->entitlement.access_until, now + 20);
    auto unpaid_retry =
        renewal_event(unrelated, "evt_unpaid_retry", "invoice.payment_failed", now + 21, now + 400);
    unpaid_retry["data"]["object"]["next_payment_attempt"] = now + 300;
    deliver(unpaid_retry);
    EXPECT_EQ(store.get_cloud_order(unrelated.id)->entitlement.access_until, now + 20);
    EXPECT_EQ(store.get_cloud_order(unrelated.id)->entitlement.subscription_status, "unpaid");
    EXPECT_FALSE(deliver(subscription_event(unrelated,
                                            "evt_ambiguous_active",
                                            "customer.subscription.updated",
                                            "active",
                                            now + 21,
                                            now + 200))
                     .value("changed", true));
    deliver(subscription_event(
        unrelated, "evt_restored", "customer.subscription.updated", "active", now + 22, now + 200));
    entitlement = store.get_cloud_order(unrelated.id)->entitlement;
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 22), "active");
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now + 207), "suspended");

    // A failure cannot restore expired access, even when its next retry is in the future or
    // the configured grace was increased. Only new positive entitlement evidence restores it.
    deliver(subscription_event(unrelated,
                               "evt_expired_period",
                               "customer.subscription.updated",
                               "active",
                               now + 23,
                               now - 20));
    const auto expired_until = store.get_cloud_order(unrelated.id)->entitlement.access_until;
    context.order_entitlement_grace_seconds = 30 * 86400;
    auto late_failure =
        renewal_event(unrelated, "evt_late_failure", "invoice.payment_failed", now + 24, now + 300);
    late_failure["data"]["object"]["next_payment_attempt"] = now + 200;
    deliver(late_failure);
    entitlement = store.get_cloud_order(unrelated.id)->entitlement;
    EXPECT_EQ(entitlement.access_until, expired_until);
    EXPECT_EQ(entitlement.grace_seconds, 7);
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now), "suspended");
    deliver(renewal_event(unrelated, "evt_paid_after_expiry", "invoice.paid", now + 25, now + 300));
    entitlement = store.get_cloud_order(unrelated.id)->entitlement;
    EXPECT_EQ(lake::control::cloud_order_entitlement_state(entitlement, now), "active");
    EXPECT_TRUE(entitlement.retry_invoice_id.empty());

    // Subscription events can arrive before the Checkout completion, including a cancellation.
    auto early = checkout_order(store, "cs_early");
    early.stripe_subscription_id = "sub_evt_early";
    early.stripe_customer_id = "cus_evt_early";
    deliver(subscription_event(
        early, "evt_early_canceled", "customer.subscription.deleted", "canceled", now, now + 100));
    deliver(json::parse(
        checkout_event("evt_early", "checkout.session.completed", "cs_early", early.id)));
    EXPECT_EQ(store.get_cloud_order(early.id)->entitlement.subscription_status, "canceled");
    EXPECT_EQ(store.get_cloud_order(early.id)->entitlement.access_until, 0);
    auto incomplete = checkout_order(store, "cs_incomplete");
    incomplete.stripe_subscription_id = "sub_evt_incomplete";
    incomplete.stripe_customer_id = "cus_evt_incomplete";
    deliver(subscription_event(incomplete,
                               "evt_incomplete_created",
                               "customer.subscription.created",
                               "incomplete",
                               now,
                               0));
    EXPECT_EQ(store.get_cloud_order(incomplete.id)->entitlement.access_until, 0);
    deliver(json::parse(checkout_event(
        "evt_incomplete", "checkout.session.completed", "cs_incomplete", incomplete.id)));
    EXPECT_GT(store.get_cloud_order(incomplete.id)->entitlement.access_until, 0);
}

} // namespace

TEST(OrdersEntitlement, SignedSubscriptionLifecycleInMemory) {
    lake::control::InMemoryStore store;
    expect_order_subscription_contract(store);
}

TEST(OrdersEntitlement, SignedSubscriptionLifecycleInPostgres) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(kek));
    expect_order_subscription_contract(*store);
}

TEST(OrdersEntitlement, DesiredStateExpiresWithoutAnotherWebhookAndRestoresOnRenewal) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(kek));
    auto order = checkout_order(*store, "cs_desired");
    ASSERT_TRUE(store->apply_cloud_order_payment({"stripe",
                                                  "evt_desired",
                                                  "checkout.session.completed",
                                                  "cs_desired",
                                                  order.id,
                                                  "paid",
                                                  "cus_desired",
                                                  "sub_desired",
                                                  "",
                                                  0}));
    ASSERT_TRUE(store->transition_cloud_order({order.id, "paid", "provisioning", "operator", ""}));
    ASSERT_TRUE(store->transition_cloud_order(
        {order.id, "provisioning", "ready", "operator", "", "", "acme.lake.live"}));
    pqxx::connection connection(uri);
    {
        pqxx::work tx(connection);
        const auto initial = tx.exec_params("SELECT desired_state FROM cloud_order_desired_state "
                                            "WHERE id = $1::uuid",
                                            order.id);
        EXPECT_EQ(initial[0][0].as<std::string>(), "suspended")
            << "zero grace requires a signed period before provisioning";
        tx.exec_params("UPDATE cloud_orders SET entitlement_access_until = extract(epoch FROM "
                       "now())::bigint - 10, entitlement_grace_seconds = 5 WHERE id = $1::uuid",
                       order.id);
        const auto rows = tx.exec_params("SELECT desired_state, entitlement_state, host, state "
                                         "FROM cloud_order_desired_state WHERE id = $1::uuid",
                                         order.id);
        EXPECT_EQ(rows[0][0].as<std::string>(), "suspended");
        EXPECT_EQ(rows[0][1].as<std::string>(), "suspended");
        EXPECT_EQ(rows[0][2].as<std::string>(), "acme.lake.live");
        EXPECT_EQ(rows[0][3].as<std::string>(), "ready");
        tx.commit();
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    lake::control::CloudOrderSubscriptionEvent renewal{"stripe",
                                                       "evt_desired_renewal",
                                                       "invoice.paid",
                                                       order.id,
                                                       "cus_desired",
                                                       "sub_desired",
                                                       "active",
                                                       now,
                                                       now + 100};
    renewal.invoice_id = "in_desired_renewal";
    ASSERT_TRUE(store->apply_cloud_order_subscription(renewal));
    {
        pqxx::work tx(connection);
        const auto rows = tx.exec_params("SELECT desired_state, entitlement_state FROM "
                                         "cloud_order_desired_state WHERE id = $1::uuid",
                                         order.id);
        EXPECT_EQ(rows[0][0].as<std::string>(), "present");
        EXPECT_EQ(rows[0][1].as<std::string>(), "active");
    }
}

// The desired-state view names its columns, so a later column on cloud_orders cannot make
// applying the schema again fail (lake#972 review).
TEST(OrdersEntitlement, DesiredStateViewSurvivesALaterColumnOnCloudOrders) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    std::ifstream file(LAKE_TEST_SOURCE_DIR "/db/schema.sql");
    const std::string schema(std::istreambuf_iterator<char>(file), {});
    ASSERT_FALSE(schema.empty());
    pqxx::connection connection(uri);
    const auto columns = [&](pqxx::work& tx, const std::string& relation) {
        std::vector<std::string> names;
        for (const auto& row :
             tx.exec_params("SELECT column_name FROM information_schema.columns WHERE "
                            "table_schema = current_schema() AND table_name = $1 "
                            "ORDER BY ordinal_position",
                            relation))
            names.push_back(row[0].as<std::string>());
        return names;
    };
    pqxx::work tx(connection);
    auto expected = columns(tx, "cloud_orders");
    expected.push_back("entitlement_state");
    expected.push_back("desired_state");
    EXPECT_EQ(columns(tx, "cloud_order_desired_state"), expected)
        << "the view lists every cloud_orders column, then its two own";
    tx.exec("ALTER TABLE cloud_orders ADD COLUMN later_column text");
    tx.exec(schema);
    EXPECT_EQ(columns(tx, "cloud_order_desired_state"), expected)
        << "a later column leaves the view as it was";
    tx.exec(schema);
    tx.commit();
}

TEST(OrdersEntitlement, CurrentSubscriptionItemsAndExpandedReferencesAreReadSafely) {
    lake::control::CloudOrder order;
    order.id = "order_1";
    order.stripe_customer_id = "cus_1";
    order.stripe_subscription_id = "sub_1";
    auto event = subscription_event(
        order, "evt_modern", "customer.subscription.updated", "active", 100, 200);
    auto& object = event["data"]["object"];
    object.erase("current_period_end");
    object["items"] = {
        {"has_more", false},
        {"data", json::array({{{"current_period_end", 200}}, {{"current_period_end", 180}}})}};
    object["customer"] = {{"id", "cus_1"}};
    auto parsed = parse_stripe_event(event.dump(), this_lake);
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->order_subscription);
    EXPECT_EQ(parsed->order_subscription->period_end, 180);
    EXPECT_EQ(parsed->order_subscription->customer_id, "cus_1");
    object["items"]["has_more"] = true;
    EXPECT_FALSE(parse_stripe_event(event.dump(), this_lake));
    object["items"]["has_more"] = "false";
    EXPECT_FALSE(parse_stripe_event(event.dump(), this_lake));
    object["current_period_end"] = 200;
    // The pinned legacy API keeps period_end on the subscription, not its item rows.
    object["items"] = {{"data", json::array({{{"id", "si_1"}}})}, {"has_more", false}};
    EXPECT_TRUE(parse_stripe_event(event.dump(), this_lake));
    auto invoice = renewal_event(order, "evt_modern_invoice", "invoice.paid", 100, 200, true);
    invoice["data"]["object"]["parent"]["subscription_details"]["subscription"] = {{"id", "sub_1"}};
    parsed = parse_stripe_event(invoice.dump(), this_lake);
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->order_subscription);
    EXPECT_EQ(parsed->order_subscription->subscription_id, "sub_1");
    invoice["data"]["object"]["lines"]["data"][0]["period"]["end"] = nullptr;
    EXPECT_FALSE(parse_stripe_event(invoice.dump(), this_lake));
    auto checkout = json::parse(
        checkout_event("evt_expanded_checkout", "checkout.session.completed", "cs_1", order.id));
    checkout["data"]["object"]["customer"] = {{"id", "cus_1"}};
    checkout["data"]["object"]["subscription"] = {{"id", "sub_1"}};
    parsed = parse_stripe_event(checkout.dump(), this_lake);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->customer, "cus_1");
    EXPECT_EQ(parsed->subscription, "sub_1");
}

TEST(OrdersEntitlement, CancellationWinsEvenWhenALaterInvoiceArrivedFirst) {
    lake::control::CloudOrderEntitlement entitlement;
    ASSERT_TRUE(lake::control::apply_cloud_order_entitlement(
        entitlement,
        {"stripe", "evt_invoice", "invoice.paid", "order", "cus", "sub", "active", 120, 300}));
    ASSERT_TRUE(lake::control::apply_cloud_order_entitlement(entitlement,
                                                             {"stripe",
                                                              "evt_canceled",
                                                              "customer.subscription.deleted",
                                                              "order",
                                                              "cus",
                                                              "sub",
                                                              "canceled",
                                                              110,
                                                              300}));
    EXPECT_EQ(entitlement.access_until, 110);
    EXPECT_FALSE(lake::control::apply_cloud_order_entitlement(entitlement,
                                                              {"stripe",
                                                               "evt_active",
                                                               "customer.subscription.updated",
                                                               "order",
                                                               "cus",
                                                               "sub",
                                                               "active",
                                                               130,
                                                               500}));
    EXPECT_EQ(entitlement.access_until, 110);
}

TEST(OrdersApex, TheOrderSheetCarriesACouponCodeToTheRegion) {
    // With the order sheet on, the region choice is its first card, and it carries `?code=` the
    // same way (lake#1156).
    const Regions regions = production();
    const OrderSheet sheet = default_sheet();
    const std::string html =
        web_ui_order_sheet(sheet, regions, regions.list[0], "/?code=LAKE50", "");
    EXPECT_NE(html.find("action=\"/?code=LAKE50\""), std::string::npos);
    EXPECT_NE(html.find("Your code <strong>LAKE50</strong> goes with you"), std::string::npos);
    EXPECT_EQ(web_ui_order_sheet(sheet, regions, regions.list[0], "/", "").find("Your code"),
              std::string::npos);
}
