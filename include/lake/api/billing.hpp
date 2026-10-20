#pragma once

// Billing (lake#564, decision 56, docs/api/billing.md): Orama Cloud's price list, the bill for a
// month worked out from an account's metered usage, and the invoices lake issues. Lake is priced
// by usage only (lake#635): one list rate per usage dimension, no plans, no fee, nothing included.
// Billing records live in lake's control Postgres (migration 0073); usage stays in each workspace
// Zetta and is read through usage_over_time. Off unless LAKE_BILLING turns it on: an enterprise
// install has no billing capability, page or Docs guide, and keeps every usage capability.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// The account a payment provider bills: lake's customer id and name, the provider's own id for
// the account when lake has one (kept in billing_accounts, migration 0074), which issue of the
// month this is (0 the first time, 1 after one void invoice, ...), so a provider can make its
// writes idempotent per attempt, and the account's billing contact once an owner has set one
// (lake#857).
struct PaymentAccount {
    std::string customer_id;
    std::string name;
    std::string provider_customer_id;
    std::uint64_t attempt = 0;
    std::optional<control::BillingContact> contact;
};

// The provider's id for an account (PaymentProvider::provider_customer). `replaces` is the kept id
// it stands in for when the provider no longer has that customer (lake#856: deleted, or kept from
// another Stripe account or mode); empty otherwise.
struct ProviderCustomer {
    std::string id;
    std::string replaces;
};

// What a payment provider answers when lake asks it to collect an invoice.
struct PaymentRequest {
    std::string reference;    // the provider's own id for the request; may be empty
    std::string instructions; // what the account is told to do to pay, shown on the invoice
    std::string payment_url;  // where the account pays (Stripe's hosted invoice page); may be empty
    std::string provider_status; // the provider's state of the request (`open`); may be empty
};

// A payment notice a provider sent lake (a Stripe webhook event), once its signature is checked.
// `state` is the lake invoice state it moves a due invoice to (`paid`, `void`), or empty when it
// only changes `provider_status` (`payment_failed`); both empty: an event lake does not act on.
struct PaymentNotice {
    std::string event_id;
    std::string event_type;
    std::string reference; // the provider's id of the invoice (PaymentRequest::reference)
    std::string state;
    std::string provider_status;
    // A Checkout notice for an order of the apex order sheet (lake#831, docs/api/orders.md):
    // `order_state` paid or cancelled (empty: not about an order, or nothing to do yet, as for a
    // completed Checkout whose payment is still processing), the Checkout Session, lake's order id
    // (client_reference_id), and the customer and subscription Stripe created.
    std::string order_state;
    std::string checkout_session;
    std::string order_id;
    std::string customer;
    std::string subscription;
    std::string order_detail;
    // The object is another app's or another Lake's in a shared Stripe account (lake#855): it is
    // acknowledged and nothing is recorded.
    bool foreign = false;
};

// A subscription to pay for an order through the provider's hosted checkout (lake#831): one line,
// the order sheet's `offer` (private_warehouse, dedicated_cluster) at `unit_amount_cents` a month
// in `currency`, for `quantity`; `description`, where it runs; where the person returns after
// paying or giving up; and lake's order id, which comes back on the provider's notice.
struct CheckoutRequest {
    std::string order_id;
    std::string offer;
    std::string description;
    std::int64_t unit_amount_cents = 0; // the monthly fee of one
    int quantity = 1;
    std::string currency = "usd";
    std::string customer_email;
    std::string success_url;
    std::string cancel_url;
};
struct CheckoutSession {
    std::string id;  // cs_...
    std::string url; // where the person pays
};

// Why a notice was refused: `invalid_signature`, `stale_timestamp` (outside the tolerance
// window, a replay), `invalid_input` (not an event) or `not_supported`.
struct NoticeRejection {
    std::string code;
    std::string message;
};

// The interface a payment provider implements (docs/api/billing.md#payment-providers). Lake
// ships the manual one and Stripe (decision 57, lake#620); a provider is lake's own code against
// the provider's HTTP API, never its SDK.
class PaymentProvider {
public:
    virtual ~PaymentProvider() = default;
    // A short lower-case name stored on each invoice (`manual`, `stripe`).
    virtual std::string_view name() const = 0;
    virtual std::string_view title() const = 0;
    // The provider's id for the account, created when it has none, or when the provider no longer
    // has the kept one (`replaces`). Lake keeps it before it asks for payment, so a failed request
    // never loses it. The manual provider answers "".
    virtual std::expected<ProviderCustomer, std::string> provider_customer(const PaymentAccount&) {
        return ProviderCustomer{};
    }
    // Sends the account's billing contact to the provider's customer it already has
    // (`provider_customer_id`): its email, name and address (lake#857). An error leaves the
    // contact lake keeps as it is.
    virtual std::expected<void, std::string> update_customer(const PaymentAccount&) {
        return {};
    }
    // Asks for payment of an invoice about to be issued: `invoice` has its account, month, price
    // list (plan_id), currency, lines and total, but no id or number yet. An error leaves the
    // invoice unissued.
    virtual std::expected<PaymentRequest, std::string>
    request_payment(const PaymentAccount& account, const control::Invoice& invoice) = 0;
    // Withdraws the request of an invoice lake voids, so it can no longer be paid. An error
    // leaves the invoice due.
    virtual std::expected<void, std::string> cancel_payment(const control::Invoice&) {
        return {};
    }
    // Reads a notice the provider sent, checking its signature (header `signature`) at `now`.
    virtual std::expected<PaymentNotice, NoticeRejection>
    read_notice(std::string_view body,
                std::string_view signature,
                std::chrono::system_clock::time_point now) {
        static_cast<void>(body);
        static_cast<void>(signature);
        static_cast<void>(now);
        return std::unexpected(
            NoticeRejection{"not_supported", "this payment provider sends no notices"});
    }
    // Whether an operator records each payment (record_invoice_payment). A provider that reports
    // payments itself answers false.
    virtual bool records_payments_by_hand() const = 0;
    // Starts a hosted checkout for a monthly subscription (lake#831). The manual provider has
    // none.
    virtual std::expected<CheckoutSession, std::string> create_checkout(const CheckoutRequest&) {
        return std::unexpected(std::string("card payment is not set up on this Lake"));
    }
};

// Invoice only: lake issues the invoice and Orama collects payment outside lake (a bank transfer,
// say); the operator records it with record_invoice_payment. `instructions` is what each invoice
// tells the account (LAKE_BILLING_INSTRUCTIONS); empty, lake says Orama will be in touch.
std::shared_ptr<PaymentProvider> manual_payment_provider(std::string instructions = {});

// What lake says about how invoices are paid (billing_account, viewer, server_info): the provider
// in use, whether the account can pay on the provider's page, and, when the manual provider is in
// use because the configured one is not available, why.
struct PaymentStatus {
    std::string provider = "manual";
    std::string title = "Invoice only";
    bool online_payment = false;
    std::string reason; // why card payment is not available; empty when it is
};

// Where the payment provider comes from at the moment of use. The billing capabilities and the
// payment notice route ask it every time. Lake's own provider keys are environment variables of
// the hosting platform (docs/api/billing.md#where-the-keys-live), read once at start.
class PaymentProviders {
public:
    virtual ~PaymentProviders() = default;
    // The provider to use now. The manual provider, with PaymentStatus::reason, when the
    // configured one is not available (a key unset or malformed); an error only when lake cannot
    // tell. `refresh` asks a provider source that can change while lake runs to look again (a
    // notice whose signature fails is checked once more before it is refused).
    virtual liquid::runtime::task<std::expected<std::shared_ptr<PaymentProvider>, ApiError>>
    current(ApiContext& context, bool refresh = false) = 0;
    virtual PaymentStatus status() const = 0;
};

// Always `provider` (tests, and the manual provider where the Stripe keys are not set, with
// `reason` saying why card payment is not available).
std::shared_ptr<PaymentProviders> fixed_payment_providers(std::shared_ptr<PaymentProvider> provider,
                                                          std::string reason = {});

// The Stripe keys: lake's own platform credentials, environment variables of the hosting platform
// (LAKE_STRIPE_SECRET_KEY, LAKE_STRIPE_WEBHOOK_SECRET; lake#628, docs/api/billing.md). Never a
// user's secret store, never logged.
struct StripeKeys {
    std::string secret_key;     // sk_test_..., sk_live_... or a restricted rk_...
    std::string webhook_secret; // whsec_...
};
using StripeProviderFactory = std::function<std::shared_ptr<PaymentProvider>(const StripeKeys&)>;

// The payment provider for the keys lake was started with: Stripe, built with `stripe`, when both
// keys are set and well formed; otherwise the manual provider with `instructions` and a reason
// that names the variable to set, never a value. A key changes only with a restart.
std::shared_ptr<PaymentProviders> environment_payment_providers(const StripeKeys& keys,
                                                                std::string instructions,
                                                                const StripeProviderFactory& stripe);

// What the payment notice route answers: an HTTP status and a JSON body.
struct NoticeOutcome {
    int status = 200;
    nlohmann::json body;
};

// A provider's payment notice (POST /billing/stripe/webhook): checks the signature through the
// current provider (once more with fresh keys when it fails), applies the event once per event id
// (billing_payment_events), moves the invoice, and records the outcome in the central audit. A
// foreign notice is acknowledged (`ignored`) with nothing recorded.
// 400 for a bad signature or a stale timestamp, 404 where the provider takes no notices, 503 when
// the keys or the store cannot be read (the provider tries again), 200 otherwise, a duplicate
// included.
liquid::runtime::task<NoticeOutcome> receive_payment_notice(PaymentProviders& providers,
                                                            ApiContext& context,
                                                            std::string_view body,
                                                            std::string_view signature);

// Whether a LAKE_BILLING value turns billing on: "on", "true" or "1". Anything else, absent
// included, is off: an install bills only when it says so.
bool billing_enabled(std::string_view value);

// The billing capabilities, registered only where billing is on (ApiServer, LAKE_BILLING). Null
// providers: the manual provider. The overload taking one provider always uses it.
void register_billing_capabilities(Registry& registry, std::shared_ptr<PaymentProviders> providers);
void register_billing_capabilities(Registry& registry,
                                   std::shared_ptr<PaymentProvider> provider = nullptr);

// Whether this registry bills: the billing capabilities are registered.
bool billing_registered(const Registry* registry);

// How this lake's invoices are paid, for `viewer` and `server_info`: {provider, title,
// online_payment, reason, checked_at} from the last time lake read the provider's keys; null where
// billing is off.
nlohmann::json payment_status_json(const ApiContext& context);

// What `billable` units cost at `unit_micros` for every `per` units, rounded up to a whole
// micro-unit and capped at the largest bigint.
std::uint64_t price_of(std::uint64_t billable, std::uint64_t unit_micros, std::uint64_t per);

// One publication of the price list (lake#635): a rate per usage dimension (`unit_micros` for
// every `per` units of the dimension's unit), in force from the first day of `effective_from`
// ("YYYY-MM"). Stored as one billing_plans row whose id is `usage-<YYYYMM>-<publish µs>`, so ids
// sort by the month they apply from, then by when they were published.
struct PriceList {
    std::string id;
    std::string effective_from; // "YYYY-MM"
    std::string published_at;
    std::string currency = "USD";
    std::vector<control::BillingPrice> prices; // `included` is always 0
};

// The id a price list published at `published_us` (microseconds since the epoch) for the month
// `effective_from` ("YYYY-MM") is stored under; empty for a malformed month.
std::string price_list_id(std::string_view effective_from, std::int64_t published_us);

// The stored price list publications among billing_plans rows (other rows ignored), oldest first.
std::vector<PriceList> price_lists(const std::vector<control::BillingPlan>& stored);

// The list a month ("YYYY-MM") is billed at: of the publications from that month or before, the
// one with the latest month, and of those the last published. A list published mid-month
// therefore prices the whole month, and a month is billed at the rates in force at its end.
// Nullopt when nothing was published for the month or before.
std::optional<PriceList> price_list_for(const std::vector<PriceList>& lists,
                                        std::string_view period);

// What of a month's `quantity` in `unit` is billed (lake#637, lake#643): time by the minute, the
// month's summed seconds rounded up once to a whole minute (`seconds`, `instance_seconds`,
// `gpu_seconds`), a whole vCPU-minute (`mcpu_seconds`, 60,000) or a whole GiB-minute
// (`mib_seconds`, 61,440); every other unit as measured.
std::uint64_t billable_quantity(std::string_view unit, std::uint64_t quantity);

// A month's measured usage: each dimension's total, and for a dimension with classes (GPU and GPU
// memory) its total per class ("" for usage of no known class).
struct UsageQuantities {
    std::map<std::string, std::uint64_t> totals;
    std::map<std::string, std::map<std::string, std::uint64_t>> by_class;
};

// One month's bill: one usage line per price on the list, in list order, with its quantity, its
// billable quantity (billable_quantity) and its amount at the list rate. A dimension rated per
// class bills each class at its class's rate, and what no class rate covers at the dimension's
// default rate (class ""), if it has one. Nothing is included.
std::vector<control::InvoiceLine> bill_lines(const PriceList& list, const UsageQuantities& usage);
std::vector<control::InvoiceLine>
bill_lines(const PriceList& list, const std::map<std::string, std::uint64_t>& quantities);

} // namespace lake::api
