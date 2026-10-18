#pragma once

// The apex order sheet (lake#831, docs/api/orders.md, docs/ui/regions.md#the-order-sheet): every
// way to get Lake on the apex host (lake.live), with its price. Shared regions continue to a
// region's host as the region picker did; a private warehouse or dedicated cluster is paid through
// Stripe Checkout and provisioned by the Orama operator; any offer can instead be sent as an order
// form, which is emailed to support and never provisioned. Pure functions and one small class: no
// HTTP type, no I/O.
//
// Offers, prices, the included usage credit, providers and regions come from one source: the
// order sheet configuration, the documented default (default_order_sheet_json) with
// LAKE_ORDER_SHEET merged over it (RFC 7386), read and validated once at startup.

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/regions.hpp"
#include "lake/api/resend.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

struct OrderRegion {
    std::string id;    // the cloud's region name, e.g. canadacentral
    std::string label; // what a person reads, e.g. Canada (Toronto)
};

// A cloud provider a dedicated cluster can be ordered on. `checkout`: the operator can provision
// it, so it can be paid for and provisioned without a person; otherwise it goes through the order
// form until the operator supports it.
struct OrderProvider {
    std::string id; // azure, aws, gcp, ...
    std::string label;
    bool checkout = false;
    std::vector<OrderRegion> regions;
};

// One paid offer: private_warehouse, dedicated_cluster or on_prem.
struct OrderOffer {
    std::string id;
    std::string title;
    std::string summary;
    std::vector<std::string> features;
    std::int64_t monthly_cents = 0; // the fee for one, a month
    bool starting_from = false;     // "Starting from" the fee (on-prem)
    int credit_percent = 0;         // usage credit included each month, a share of the fee
    bool checkout = false;          // may be paid through Stripe Checkout
};

// A trust claim on the sheet, shown with the link that backs it.
struct OrderTrustClaim {
    std::string text;
    std::string url;
};

struct OrderSheet {
    // Off: the apex is the plain region picker of lake#757.
    bool enabled = true;
    // Where the order sheet is, for the web app's "New setup…" and "On-prem…" entries
    // (server_info `order_sheet_url`). Empty: https://<LAKE_APEX_HOST>/ where this Lake serves the
    // apex, else none.
    std::string url;
    std::string currency = "USD";
    // The shared option: usage-based, at the published price list.
    std::string shared_title;
    std::string shared_summary;
    std::vector<std::string> shared_features;
    std::string price_list_url;     // empty: the apex's own /order/price-list page
    std::string promise;            // the lead line
    bool show_promise = false;      // off until the test in docs/api/billing.md is recorded
    std::vector<OrderOffer> offers; // private_warehouse, dedicated_cluster, on_prem, in that order
    std::vector<OrderProvider> providers; // for a dedicated cluster, in order
    std::vector<OrderTrustClaim> trust;
    // Abuse limits on the anonymous form: orders a client address may send an hour, and in all.
    int orders_per_client_per_hour = 5;
    int orders_per_hour = 60;

    const OrderOffer* find_offer(std::string_view id) const;
    const OrderProvider* find_provider(std::string_view id) const;
};

// The documented default (docs/api/orders.md#configuration): the offers, prices, credit and
// regions from lake#831.
std::string_view default_order_sheet_json();

// The default with `overrides` (LAKE_ORDER_SHEET; empty = none) merged over it as an RFC 7386 JSON
// merge patch, then validated. An error names the field and stops startup: invalid JSON, an
// unknown or missing offer, a price that is not a positive whole number of dollars, a credit
// outside 0-100, a provider or region without an id or label, two with one id, a URL that is not
// https (or a site path for price_list_url), a limit below 1.
std::expected<OrderSheet, std::string> order_sheet_from_environment(std::string_view overrides);

// The usage credit `quantity` of `offer` include each month, in cents.
std::int64_t included_credit_cents(const OrderOffer& offer, int quantity = 1);
// Whole dollars with thousands separators ("$1,000"); cents only when there are any.
std::string format_usd(std::int64_t cents);
// The URL path segment of an offer (private-warehouse), and back; empty when unknown.
std::string offer_slug(std::string_view offer_id);
std::string offer_from_slug(std::string_view slug);

// What the order form sent (application/x-www-form-urlencoded fields).
struct OrderFormInput {
    std::string offer;    // private_warehouse | dedicated_cluster | on_prem
    std::string path;     // checkout | order_form
    std::string location; // "<provider>:<region>", a shared region's name, or "other"
    std::string contact_name, contact_email, company, details;
    std::string quantity; // decimal, default 1
};
// Field name -> what is wrong, in the words the form shows.
using OrderFormErrors = std::map<std::string, std::string>;

// Checks a form against the sheet and the shared `regions` and builds the order to record.
// `checkout_available`: this Lake can take card payment (Stripe is configured). Checkout is only
// for an offer and location that can be provisioned without a person; anything not listed
// ("other") is an order form, and so is everything when card payment is not available.
std::expected<control::NewCloudOrder, OrderFormErrors>
validate_order_form(const OrderSheet& sheet,
                    const Regions& regions,
                    const OrderFormInput& input,
                    bool checkout_available);

// The location an order names, as a person reads it ("Microsoft Azure, Canada (Toronto)").
std::string order_location_label(const OrderSheet& sheet,
                                 const Regions& regions,
                                 const control::CloudOrder& order);

// The token in an order's status link: HMAC-SHA-256 under `key` (LAKE_INSTANCE_KEK) over the
// order id, base64url. Only someone given the link can read the order.
std::string order_status_token(std::string_view key, std::string_view order_id);
bool order_status_token_matches(std::string_view key,
                                std::string_view order_id,
                                std::string_view token);

// Who is due an email about the order's current state (docs/api/orders.md#email): support for
// submitted, paid and failed; the customer for submitted, paid, ready and failed; each only when
// not yet told about this state.
std::vector<std::string> order_email_recipients_due(const control::CloudOrder& order);
// The email `recipient` (support or customer) gets about the order's current state: every field
// and the order id, and the status link for the customer. Plain text.
Email order_email(const OrderSheet& sheet,
                  const Regions& regions,
                  const ResendSettings& settings,
                  const control::CloudOrder& order,
                  std::string_view recipient,
                  std::string_view status_url);
// How long to wait before the next attempt after `attempts` failed ones: 1, 2, 4 ... minutes, at
// most an hour.
int order_email_retry_seconds(int attempts);

// One rate of the published price list as the apex price list page shows it: what is priced and
// what it costs ("$0.0015 per vCPU-minute").
struct PriceListRow {
    std::string title;
    std::string rate;
};
std::vector<PriceListRow> price_list_rows(const std::vector<control::BillingPrice>& prices);

// A sliding-hour limit on orders, per client address and in all (the apex is anonymous). Thread
// safe. Remembers at most 10,000 addresses; past that, the oldest are forgotten.
class OrderRateLimiter {
public:
    OrderRateLimiter(int per_client_per_hour, int per_hour)
        : per_client_(per_client_per_hour), per_hour_(per_hour) {}
    // Counts one order from `client` at `now` and answers true, or answers false (and counts
    // nothing) when the client or everyone has reached the limit within the last hour.
    bool allow(std::string_view client, std::chrono::steady_clock::time_point now);

private:
    std::mutex mutex_;
    int per_client_;
    int per_hour_;
    std::deque<std::chrono::steady_clock::time_point> all_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>, std::less<>> clients_;
};

} // namespace lake::api
