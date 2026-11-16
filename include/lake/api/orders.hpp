// SPDX-License-Identifier: MIT

#pragma once

// The apex order sheet (lake#831, docs/api/orders.md, docs/ui/regions.md#the-order-sheet): every
// way to get Lake on the apex host, with its price. Shared regions continue to a
// region's host as the region picker did; a private warehouse or dedicated cluster is paid through
// Stripe Checkout and provisioned by the Orama operator; any offer can instead be sent as an order
// form, which is emailed to support and never provisioned. Pure functions and one small class: no
// HTTP type, no I/O.
//
// Offers, prices, the included usage credit, providers and regions come from one source: the
// order sheet configuration, LAKE_ORDER_SHEET merged (RFC 7386) over the default
// (default_order_sheet_json), read and validated once at startup. The default is off and sells
// nothing: lake carries no offer or price of its own.

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
    // Whether the operator can create a cluster here, so it can be paid for and provisioned
    // without a person. Defaults to the provider's `checkout`; a region the operator cannot reach
    // is listed but ordered with the order form ("we'll confirm availability").
    bool checkout = false;
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
    // Off (the default): the apex is the plain region picker of lake#757.
    bool enabled = false;
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
    std::string promise;            // the lead line; empty: none
    bool show_promise = false;      // shown only when on and non-empty
    std::vector<OrderOffer> offers; // those configured of private_warehouse, dedicated_cluster, on_prem
    std::vector<OrderProvider> providers; // for a dedicated cluster, in order
    std::vector<OrderTrustClaim> trust;
    // Abuse limits on the anonymous form: orders a client address may send an hour, and in all.
    int orders_per_client_per_hour = 5;
    int orders_per_hour = 60;
    // Per-recipient limits (lake#1000): orders that may name one contact email in 24 hours, and
    // one email domain in an hour, whoever sends them. Each order form mails its address once.
    int orders_per_email_per_day = 3;
    int orders_per_domain_per_hour = 20;

    const OrderOffer* find_offer(std::string_view id) const;
    const OrderProvider* find_provider(std::string_view id) const;
};

// The documented default (docs/api/orders.md#configuration): off, no offers, no providers.
std::string_view default_order_sheet_json();

// The default with `overrides` (LAKE_ORDER_SHEET; empty = none) merged over it as an RFC 7386 JSON
// merge patch, then validated. An offer left out is not sold. An error names the field and stops
// startup: invalid JSON, an unknown or incomplete offer, a dedicated cluster with no provider, a price that is not a positive whole number of dollars, a credit
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
// not yet told about this state. An order form whose contact email is not confirmed (lake#1000)
// is due one email only, the customer's confirmation, until it is sent.
std::vector<std::string> order_email_recipients_due(const control::CloudOrder& order);
// The state an email to `recipient` tells, recorded once it is delivered: the order's state, or
// `confirm` for the confirmation an unconfirmed order form's address gets (lake#1000).
std::string order_email_state(const control::CloudOrder& order, std::string_view recipient);

// The token in an order form's confirmation link (lake#1000): HMAC-SHA-256 under `key` over the
// order id, distinct from the status token, base64url. The link works for
// `order_confirmation_max_age_seconds` after the order was sent.
inline constexpr int order_confirmation_max_age_seconds = 7 * 86400;
std::string order_confirmation_token(std::string_view key, std::string_view order_id);
bool order_confirmation_token_matches(std::string_view key,
                                      std::string_view order_id,
                                      std::string_view token);
// The email `recipient` (support or customer) gets about the order's current state: every field
// and the order id, and the status link for the customer. Plain text. To an unconfirmed order
// form's address it is the confirmation (lake#1000): Lake's words, the order id and `status_url`,
// which is then the confirmation link, and nothing else of the order.
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

// A sliding-hour limit on orders, per client address and in all (the apex is anonymous). The
// in-all limit is kept per order path (lake#838): order forms, which anyone can send, can never use
// up the cap that checkout orders, which pay, are counted against, nor the reverse. Thread safe.
// Remembers at most 10,000 addresses; past that, the oldest are forgotten.
class OrderRateLimiter {
public:
    OrderRateLimiter(int per_client_per_hour, int per_hour)
        : per_client_(per_client_per_hour), per_hour_(per_hour) {}
    // Counts one order from `client` on `path` (checkout | order_form) at `now` and answers true,
    // or answers false (and counts nothing) when the client, or every order on that path, has
    // reached the limit within the last hour.
    bool allow(std::string_view client,
               std::string_view path,
               std::chrono::steady_clock::time_point now);

private:
    std::mutex mutex_;
    int per_client_;
    int per_hour_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>, std::less<>> all_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>, std::less<>> clients_;
};

} // namespace lake::api
