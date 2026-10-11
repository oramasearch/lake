#pragma once

// Billing (lake#564, decision 56, docs/api/billing.md): Orama Cloud's price list, each account's
// plan, the bill for a month worked out from its metered usage, and the invoices lake issues.
// Billing records live in lake's control Postgres (migration 0073); usage stays in each workspace
// Zetta and is read through usage_over_time. Off unless LAKE_BILLING turns it on: an enterprise
// install has no billing capability, page or Docs guide, and keeps every usage capability.

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// What a payment provider answers when lake asks it to collect an invoice.
struct PaymentRequest {
    std::string reference;    // the provider's own id for the request; may be empty
    std::string instructions; // what the account is told to do to pay, shown on the invoice
};

// The interface a payment provider implements (docs/api/billing.md#payment-providers). No
// provider has been chosen (lake#564's follow-up); lake ships the manual one, and a provider is
// lake's own code against the provider's HTTP API, never its SDK.
class PaymentProvider {
public:
    virtual ~PaymentProvider() = default;
    // A short lower-case name stored on each invoice (`manual`).
    virtual std::string_view name() const = 0;
    virtual std::string_view title() const = 0;
    // Asks for payment of an invoice about to be issued: `invoice` has its account, month, plan,
    // currency, lines and total, but no id or number yet. An error leaves the invoice unissued.
    virtual std::expected<PaymentRequest, std::string>
    request_payment(const control::Invoice& invoice) = 0;
    // Whether an operator records each payment (record_invoice_payment). A provider that reports
    // payments itself answers false.
    virtual bool records_payments_by_hand() const = 0;
};

// Invoice only: lake issues the invoice and Orama collects payment outside lake (a bank transfer,
// say); the operator records it with record_invoice_payment. `instructions` is what each invoice
// tells the account (LAKE_BILLING_INSTRUCTIONS); empty, lake says Orama will be in touch.
std::shared_ptr<PaymentProvider> manual_payment_provider(std::string instructions = {});

// Whether a LAKE_BILLING value turns billing on: "on", "true" or "1". Anything else, absent
// included, is off: an install bills only when it says so.
bool billing_enabled(std::string_view value);

// The billing capabilities, registered only where billing is on (ApiServer, LAKE_BILLING). A null
// provider is the manual one.
void register_billing_capabilities(Registry& registry,
                                   std::shared_ptr<PaymentProvider> provider = nullptr);

// Whether this registry bills: the billing capabilities are registered.
bool billing_registered(const Registry* registry);

// What `billable` units cost at `unit_micros` for every `per` units, rounded up to a whole
// micro-unit and capped at the largest bigint.
std::uint64_t price_of(std::uint64_t billable, std::uint64_t unit_micros, std::uint64_t per);

// One month's bill on a plan: the plan's fee, then one line per priced dimension (in the plan's
// price order) with its quantity from `quantities` (0 when absent), what the plan includes, what
// is billed beyond it and its amount. `period` is "YYYY-MM".
std::vector<control::InvoiceLine> bill_lines(const control::BillingPlan& plan,
                                             const std::map<std::string, std::uint64_t>& quantities,
                                             std::string_view period);

} // namespace lake::api
