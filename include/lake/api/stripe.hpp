#pragma once

// Stripe, lake's payment provider (decision 57, lake#620, docs/api/billing.md#stripe). Lake's own
// client for Stripe's REST API (form-encoded requests with a bearer key and an Idempotency-Key on
// every write), never Stripe's SDK, and the check of Stripe's webhook signatures. The HTTP
// exchange is the server's outbound client, injected as a StripeHttp the tests replace with a
// fake.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lake/api/billing.hpp"

namespace lake::api {

inline constexpr std::string_view stripe_api_base = "https://api.stripe.com";
// The Stripe API version lake's requests are written against, sent as Stripe-Version so a change
// of the account's default version does not change what lake reads.
inline constexpr std::string_view stripe_api_version = "2024-06-20";
// How far a webhook's timestamp may be from lake's clock (Stripe's libraries use five minutes).
inline constexpr std::chrono::seconds stripe_signature_tolerance{300};

// Lake's products in every Stripe account it bills through (lake#855). Each invoice item and
// Checkout line names one with an inline price (price_data[product]), so Stripe's reports tell
// Lake's revenue from Orama Cloud v1's while Lake's price list and order sheet stay the one source
// of prices. Lake never creates them: the operator does, in each account (spec.payments,
// orama-operator docs/lake/payments.md).
inline constexpr std::string_view stripe_usage_product = "lake_usage";
inline constexpr std::string_view stripe_private_warehouse_product = "lake_private_warehouse";
inline constexpr std::string_view stripe_dedicated_cluster_product = "lake_dedicated_cluster";

// The product of an order sheet offer paid through Checkout; empty for any other (on_prem is
// ordered by form).
std::string_view stripe_offer_product(std::string_view offer);

struct StripeRequest {
    std::string method = "POST";
    std::string url;
    std::string body; // application/x-www-form-urlencoded
    std::vector<std::pair<std::string, std::string>> headers;
};
struct StripeResponse {
    int status = 0;
    std::string body;
    std::string error; // the transport's failure, never the request
};
using StripeHttp = std::function<StripeResponse(const StripeRequest&)>;

// One form field, percent-encoded as application/x-www-form-urlencoded (`metadata[k]=v`).
std::string stripe_form(const std::vector<std::pair<std::string, std::string>>& fields);

// Millionths of a currency unit as cents, rounded half up.
std::int64_t stripe_cents(std::uint64_t micros);

// The Stripe-Signature check: `header` is `t=<unix seconds>,v1=<hex>[,v1=...][,v0=...]`; the
// signed payload is `<t>.<body>` under HMAC-SHA256 with the webhook secret. invalid_signature when
// no v1 matches (compared in constant time), stale_timestamp when `t` is more than `tolerance`
// from `now` either way.
std::expected<void, NoticeRejection>
verify_stripe_signature(std::string_view body,
                        std::string_view header,
                        std::string_view webhook_secret,
                        std::chrono::system_clock::time_point now,
                        std::chrono::seconds tolerance = stripe_signature_tolerance);

// The header Stripe would send for `body` at `timestamp` (tests and documentation).
std::string stripe_signature_header(std::string_view body,
                                    std::string_view webhook_secret,
                                    std::int64_t timestamp);

// What lake does with a verified event: invoice.paid moves a due invoice to paid,
// invoice.voided to void, invoice.payment_failed keeps it due with provider_status
// payment_failed; any other event is acknowledged and ignored. An invoice or Checkout event whose
// object is not this Lake's is `foreign` (docs/api/billing.md#webhooks): it has no
// lake_customer_id (an invoice) or lake_order_id (a session, or an invoice of its subscription),
// or its lake_instance names a Lake other than `instance`, this Lake's public host.
std::expected<PaymentNotice, NoticeRejection> parse_stripe_event(std::string_view body,
                                                                 std::string_view instance);

// Whether a value looks like a Stripe secret key (sk_ or rk_) or a webhook secret (whsec_).
bool stripe_secret_key_shape(std::string_view key);
bool stripe_webhook_secret_shape(std::string_view secret);

// The Stripe provider: customers, invoices with an invoice item of Lake's usage product per lake
// line, finalization and the hosted invoice URL, voids, Checkout for the order sheet's offers, and
// webhook events. A kept customer is read before it is reused and replaced when Stripe no longer
// has it (lake#856). A request that names a product the account lacks fails with a message saying
// the operator creates it. `instance` is this Lake's public host
// (LAKE_PUBLIC_HOST), tagged on every object it creates and checked on every event, since two
// Lakes can bill through one account. `base` is Stripe's API origin (a fake's in tests).
std::shared_ptr<PaymentProvider>
stripe_payment_provider(StripeKeys keys,
                        std::string instance,
                        StripeHttp http,
                        std::string base = std::string(stripe_api_base));

} // namespace lake::api
