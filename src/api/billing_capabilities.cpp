// SPDX-License-Identifier: MIT

// Billing (lake#564, docs/api/billing.md): price_list, set_price_list,
// billing_account, set_billing_contact, bill_preview, issue_invoice, resend_invoice,
// list_invoices, get_invoice, record_invoice_payment and void_invoice, with the manual payment
// provider. Registered only where LAKE_BILLING is on.
// Lake is priced by usage only (lake#635): list rates per usage dimension and class, no plans, no
// fee and nothing included, so every account is billed on the price list without choosing anything.
// The records are control-plane rows (each publication of the price list, stored as a billing_plans
// row, and invoices with their lines); the usage a bill prices is read from each workspace Zetta
// through usage_over_time, and only the quantity billed per dimension is kept, on the invoice's
// lines. Every call is
// policy-checked and audited in the central log: billing is account configuration.

#include "lake/api/balance.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/stripe.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "lake/api/usage_recorder.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

constexpr std::uint64_t max_amount =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

std::uint64_t add_capped(std::uint64_t left, std::uint64_t right) {
    return left > max_amount - std::min(right, max_amount) ? max_amount : left + right;
}

// ------------------------------------------------------------------------------------------------
// Months: a bill covers one UTC calendar month, named "YYYY-MM".

struct Month {
    int year = 0;
    unsigned month = 0;

    std::string name() const {
        return std::format("{:04}-{:02}", year, month);
    }
    std::string first_day() const {
        return name() + "-01";
    }
    std::string start() const {
        return first_day() + "T00:00:00Z";
    }
    Month next() const {
        return month == 12 ? Month{year + 1, 1} : Month{year, month + 1};
    }
    std::chrono::sys_days start_day() const {
        return std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} /
                                     std::chrono::day{1}};
    }
};

std::optional<Month> month_of(std::string_view text) {
    if (text.size() != 7 || text[4] != '-') {
        return std::nullopt;
    }
    int year = 0;
    unsigned month = 0;
    for (std::size_t index = 0; index < 7; ++index) {
        if (index == 4) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
        if (index < 4) {
            year = year * 10 + (text[index] - '0');
        } else {
            month = month * 10 + static_cast<unsigned>(text[index] - '0');
        }
    }
    if (year < 2000 || month < 1 || month > 12) {
        return std::nullopt;
    }
    return Month{year, month};
}

Month this_month() {
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return Month{static_cast<int>(today.year()), static_cast<unsigned>(today.month())};
}

// ------------------------------------------------------------------------------------------------
// Records as JSON.

// A GPU class as a person reads it: "h100" is "H100".
std::string class_title(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

json price_json(const control::BillingPrice& price) {
    const UsageDimension* dimension = find_usage_dimension(price.dimension);
    std::string title = dimension ? std::string(dimension->title) : price.dimension;
    if (!price.class_name.empty()) {
        title += " (" + class_title(price.class_name) + ")";
    }
    return {{"dimension", price.dimension},
            {"class", price.class_name},
            {"title", title},
            {"unit", dimension ? dimension->unit : std::string_view()},
            {"unit_micros", price.unit_micros},
            {"per", price.per},
            {"priced", dimension == nullptr || dimension->priced}};
}

json price_list_json(const PriceList& list) {
    json prices = json::array();
    for (const control::BillingPrice& price : list.prices) {
        prices.push_back(price_json(price));
    }
    return {{"id", list.id},
            {"effective_from", list.effective_from},
            {"published_at", list.published_at},
            {"currency", list.currency},
            {"prices", std::move(prices)}};
}

json optional_list_json(const std::optional<PriceList>& list) {
    return list ? price_list_json(*list) : json(nullptr);
}

json line_json(const control::InvoiceLine& line) {
    return {{"kind", line.kind},
            {"dimension", line.dimension},
            {"class", line.class_name},
            {"description", line.description},
            {"unit", line.unit},
            {"quantity", line.quantity},
            {"included", line.included},
            {"billable", line.billable},
            {"unit_micros", line.unit_micros},
            {"per", line.per},
            {"amount_micros", line.amount_micros}};
}

json lines_json(const std::vector<control::InvoiceLine>& lines) {
    json out = json::array();
    for (const control::InvoiceLine& line : lines) {
        out.push_back(line_json(line));
    }
    return out;
}

json invoice_json(const control::Invoice& invoice) {
    return {{"id", invoice.id},
            {"number", invoice.number},
            {"customer_id", invoice.customer_id},
            {"period", invoice.period},
            {"price_list_id", invoice.plan_id},
            {"currency", invoice.currency},
            {"total_micros", invoice.total_micros},
            {"state", invoice.state},
            {"provider", invoice.provider},
            {"provider_reference", invoice.provider_reference},
            {"instructions", invoice.instructions},
            {"note", invoice.note},
            {"issued_at", invoice.issued_at},
            {"paid_at", invoice.paid_at},
            {"voided_at", invoice.voided_at},
            {"payment_url", invoice.payment_url},
            {"provider_status", invoice.provider_status},
            {"lines", lines_json(invoice.lines)}};
}

json string_schema() {
    return {{"type", "string"}};
}

json integer_schema(std::int64_t minimum = 0) {
    return {{"type", "integer"},
            {"minimum", minimum},
            {"maximum", std::numeric_limits<std::int64_t>::max()}};
}

json object_schema(json properties, json required) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json output_schema(json properties, json required) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)}};
}

json price_list_schema() {
    return output_schema({{"id", string_schema()},
                          {"effective_from", string_schema()},
                          {"published_at", string_schema()},
                          {"currency", string_schema()},
                          {"prices", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
                         json::array({"id", "effective_from", "currency", "prices"}));
}

json nullable_object() {
    return {{"type", json::array({"object", "null"})}};
}

json invoice_schema() {
    return output_schema({{"id", string_schema()},
                          {"number", {{"type", "integer"}}},
                          {"customer_id", string_schema()},
                          {"period", string_schema()},
                          {"price_list_id", string_schema()},
                          {"currency", string_schema()},
                          {"total_micros", {{"type", "integer"}}},
                          {"state", {{"type", "string"}, {"enum", {"due", "paid", "void"}}}},
                          {"provider", string_schema()},
                          {"provider_reference", string_schema()},
                          {"instructions", string_schema()},
                          {"note", string_schema()},
                          {"issued_at", string_schema()},
                          {"paid_at", string_schema()},
                          {"voided_at", string_schema()},
                          {"payment_url", string_schema()},
                          {"provider_status", string_schema()},
                          {"lines", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
                         json::array({"id",
                                      "number",
                                      "customer_id",
                                      "period",
                                      "price_list_id",
                                      "currency",
                                      "total_micros",
                                      "state",
                                      "provider",
                                      "lines"}));
}

// ------------------------------------------------------------------------------------------------
// Authorization.

bool instance_administrator(const ApiContext& context) {
    return context.operator_credential || context.acting_identity == "admin";
}

// Only the operator and the instance administrator, who skip object authorization, reach the
// handler; anyone else is refused before it.
std::expected<std::string, ApiError> administrator_only(const json&, ApiContext&) {
    return std::unexpected(
        ApiError{"unauthorized", "the operator or the instance administrator is required", {}});
}

ApiResult refused() {
    return ApiResult::failure("unauthorized",
                              "the operator or the instance administrator is required");
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> self_object(const json&, ApiContext& context) {
    return "principal/" + context.acting_identity;
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable", "no control-plane store is configured");
}

ApiResult from_store(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

void audit(ApiContext& context,
           const std::string& customer_id,
           std::string kind,
           json details = json::object()) {
    context.audit_customer_id = customer_id;
    context.audit_kind = std::move(kind);
    context.audit_object = "customer/" + customer_id + "/billing";
    context.audit_details = std::move(details);
}

// ------------------------------------------------------------------------------------------------
// The price list.

std::expected<std::vector<PriceList>, ApiResult> published_lists(ApiContext& context) {
    const auto stored = context.store->billing_plans();
    if (!stored) {
        return std::unexpected(from_store(stored.error()));
    }
    return price_lists(*stored);
}

// The list `month` is billed at, or a conflict saying none is published for it.
std::expected<PriceList, ApiResult> list_for(ApiContext& context, const Month& month) {
    auto lists = published_lists(context);
    if (!lists) {
        return std::unexpected(lists.error());
    }
    auto list = price_list_for(*lists, month.name());
    if (!list) {
        return std::unexpected(
            ApiResult::failure("conflict",
                               "no price list is published for " + month.name() +
                                   "; the operator publishes one with set_price_list"));
    }
    return std::move(*list);
}

// ------------------------------------------------------------------------------------------------
// Measuring a month: usage_over_time, called as the same caller, by day.

struct Measured {
    std::optional<ApiResult> failure; // why the usage could not be read at all
    UsageQuantities quantities;
    bool complete = true;
    json unread = json::array();       // the workspaces whose usage could not be read
    json not_measured = json::array(); // what usage_over_time says it cannot include
};

std::uint64_t whole(double value) {
    if (!(value > 0.0)) {
        return 0;
    }
    if (value >= static_cast<double>(max_amount)) {
        return max_amount;
    }
    return static_cast<std::uint64_t>(std::llround(value));
}

// `audited` false reads usage_over_time's handler directly, with no capability call of its own:
// the balance's refresh (lake#1114) reads every account every few minutes as the store root's
// owner, and a central audit event for each read would bury the log.
liquid::runtime::task<Measured> measure(ApiContext& context,
                                        const std::string& customer_id,
                                        const Month& month,
                                        const PriceList& list,
                                        bool audited = true) {
    Measured measured;
    json dimensions = json::array();
    std::set<std::string> asked;
    for (const control::BillingPrice& price : list.prices) {
        if (asked.insert(price.dimension).second) {
            dimensions.push_back(price.dimension);
        }
    }
    // Always inspect model classes: removing every model price must not hide already used tokens.
    if (asked.insert("model_tokens").second) {
        dimensions.push_back("model_tokens");
    }
    if (context.registry == nullptr) {
        measured.failure =
            ApiResult::failure("store_unavailable", "no registry to read usage_over_time");
        co_return measured;
    }
    ApiContext inner = context;
    const json input{{"customer_id", customer_id},
                     {"from", month.start()},
                     {"to", month.next().start()},
                     {"granularity", "day"},
                     {"dimensions", std::move(dimensions)}};
    std::expected<ApiResult, liquid::error> done;
    if (audited) {
        done = co_await context.registry->call_async("usage_over_time", input, inner);
    } else {
        const auto usage = context.registry->find("usage_over_time");
        if (!usage || !usage->async_handler) {
            measured.failure =
                ApiResult::failure("store_unavailable", "this Lake has no usage_over_time");
            co_return measured;
        }
        done = co_await usage->async_handler(input, inner);
    }
    if (!done) {
        measured.failure = ApiResult::failure("store_error", done.error().message);
        co_return measured;
    }
    if (!done->ok) {
        measured.failure = std::move(*done);
        co_return measured;
    }
    const json& reading = done->value;
    for (const json& workspace : reading.value("workspaces", json::array())) {
        if (!workspace.value("measured", true)) {
            measured.complete = false;
            measured.unread.push_back({{"workspace_id", workspace.value("workspace_id", "")},
                                       {"name", workspace.value("name", "")},
                                       {"message", workspace.value("message", "")}});
        }
    }
    measured.not_measured = reading.value("not_measured", json::array());
    for (const json& entry : reading.value("dimensions", json::array())) {
        const std::string name = entry.value("dimension", "");
        if (entry.value("aggregation", "sum") == "last") {
            // A reading (storage): the average of the month's daily readings.
            double sum = 0.0;
            std::size_t count = 0;
            for (const json& point : entry.value("points", json::array())) {
                if (point.is_number()) {
                    sum += point.get<double>();
                    ++count;
                }
            }
            measured.quantities.totals[name] =
                count == 0 ? 0 : whole(sum / static_cast<double>(count));
        } else {
            measured.quantities.totals[name] = whole(entry.value("total", 0.0));
        }
        for (const json& row : entry.value("by_class", json::array())) {
            measured.quantities.by_class[name][row.value("key", "")] +=
                whole(row.value("total", 0.0));
            if (name == "model_tokens" && !row.value("key", "").empty() &&
                row.value("total", 0.0) > 0) {
                const auto key = row.at("key").get<std::string>();
                const auto priced = std::ranges::count_if(list.prices, [&](const auto& price) {
                    return price.dimension == name && price.class_name == key &&
                           price.unit_micros > 0 && price.per > 0;
                });
                if (priced != 1) {
                    measured.failure = ApiResult::failure(
                        "conflict", "model token usage has no positive exact price: " + key);
                    co_return measured;
                }
            }
        }
    }
    co_return measured;
}

json bill_json(const std::string& customer_id,
               const Month& month,
               const PriceList& list,
               const std::vector<control::InvoiceLine>& lines,
               const Measured& measured) {
    std::uint64_t total = 0;
    for (const control::InvoiceLine& line : lines) {
        total = add_capped(total, line.amount_micros);
    }
    return {{"customer_id", customer_id},
            {"period", month.name()},
            {"price_list", {{"id", list.id}, {"effective_from", list.effective_from}}},
            {"currency", list.currency},
            {"lines", lines_json(lines)},
            {"total_micros", total},
            {"complete", measured.complete},
            {"unread_workspaces", measured.unread},
            {"not_measured", measured.not_measured}};
}

// ------------------------------------------------------------------------------------------------
// The manual provider.

class ManualPaymentProvider final : public PaymentProvider {
public:
    explicit ManualPaymentProvider(std::string instructions)
        : instructions_(std::move(instructions)) {}
    std::string_view name() const override {
        return "manual";
    }
    std::string_view title() const override {
        return "Invoice only";
    }
    std::expected<PaymentRequest, std::string> request_payment(const PaymentAccount&,
                                                               const control::Invoice&) override {
        return PaymentRequest{
            {},
            instructions_.empty()
                ? "Orama will send you payment details for this invoice. It is paid once Orama "
                  "records your payment."
                : instructions_,
            {},
            {}};
    }
    bool records_payments_by_hand() const override {
        return true;
    }

private:
    std::string instructions_;
};

json provider_json(const PaymentProvider& provider, const PaymentStatus& status) {
    return {{"name", provider.name()},
            {"title", provider.title()},
            {"records_payments_by_hand", provider.records_payments_by_hand()},
            {"online_payment", !provider.records_payments_by_hand()},
            {"reason", provider.records_payments_by_hand() ? status.reason : std::string()}};
}

json contact_json(const control::BillingContact& contact) {
    return {{"email", contact.email},
            {"name", contact.name},
            {"line1", contact.line1},
            {"line2", contact.line2},
            {"city", contact.city},
            {"postal_code", contact.postal_code},
            {"state", contact.state},
            {"country", contact.country},
            {"updated_at", contact.updated_at}};
}

// The email billing_account suggests while the account has no billing contact (lake#857): the
// signed-in person's, from their user record, when they may set the contact (admin on the account,
// as its owners and admins hold). Nothing is stored until they save it.
json suggested_email(ApiContext& context, const std::string& customer_id) {
    if (context.acting_user_id.empty()) {
        return nullptr;
    }
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return nullptr;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, object->id, "admin", context.authorization_cache);
    if (!decision || !decision->allowed) {
        return nullptr;
    }
    const auto user = context.store->get_user(context.acting_user_id);
    if (!user || control::billing_contact_problem({.email = user->email})) {
        return nullptr;
    }
    return user->email;
}

// The account's billing: the price list it is billed at this month, how invoices are paid, and
// where they go. There is nothing to choose: every account is billed on the price list (lake#635).
ApiResult account_of(ApiContext& context,
                     const std::string& customer_id,
                     const PaymentProvider& provider,
                     const PaymentStatus& status) {
    const auto customer = context.store->get_customer(customer_id);
    if (!customer) {
        return ApiResult::failure("not_found", "no account " + customer_id, "/customer_id");
    }
    auto lists = published_lists(context);
    if (!lists) {
        return lists.error();
    }
    const auto contact = context.store->billing_contact(customer_id);
    if (!contact) {
        return from_store(contact.error());
    }
    const Month now = this_month();
    return ApiResult::success(
        {{"customer_id", customer_id},
         {"month", now.name()},
         {"price_list", optional_list_json(price_list_for(*lists, now.name()))},
         {"provider", provider_json(provider, status)},
         {"contact", *contact ? contact_json(**contact) : json(nullptr)},
         {"suggested_email", *contact ? json(nullptr) : suggested_email(context, customer_id)}});
}

json nullable_string() {
    return {{"type", json::array({"string", "null"})}};
}

json contact_schema() {
    return output_schema({{"email", string_schema()},
                          {"name", string_schema()},
                          {"line1", string_schema()},
                          {"line2", string_schema()},
                          {"city", string_schema()},
                          {"postal_code", string_schema()},
                          {"state", string_schema()},
                          {"country", string_schema()},
                          {"updated_at", string_schema()}},
                         json::array({"email",
                                      "name",
                                      "line1",
                                      "line2",
                                      "city",
                                      "postal_code",
                                      "state",
                                      "country",
                                      "updated_at"}));
}

json account_schema() {
    return output_schema(
        {{"customer_id", string_schema()},
         {"month", string_schema()},
         {"price_list", nullable_object()},
         {"provider", {{"type", "object"}}},
         {"contact", nullable_object()},
         {"suggested_email", nullable_string()}},
        json::array(
            {"customer_id", "month", "price_list", "provider", "contact", "suggested_email"}));
}

json bill_schema() {
    return output_schema(
        {{"customer_id", string_schema()},
         {"period", string_schema()},
         {"price_list", {{"type", "object"}}},
         {"currency", string_schema()},
         {"lines", {{"type", "array"}, {"items", {{"type", "object"}}}}},
         {"total_micros", {{"type", "integer"}}},
         {"complete", {{"type", "boolean"}}},
         {"unread_workspaces", {{"type", "array"}, {"items", {{"type", "object"}}}}},
         {"not_measured", {{"type", "array"}, {"items", string_schema()}}},
         {"invoice", {{"type", json::array({"object", "null"})}}}},
        json::array({"customer_id",
                     "period",
                     "price_list",
                     "currency",
                     "lines",
                     "total_micros",
                     "complete",
                     "not_measured"}));
}

const json period_schema{{"type", "string"},
                         {"pattern", "^[0-9]{4}-(0[1-9]|1[0-2])$"},
                         {"description", "a UTC calendar month, YYYY-MM"}};

// The invoice `invoice_id` names, checked against the account an operator-only caller names in
// the audit; not_found for an unknown id.
std::expected<control::Invoice, ApiResult> invoice_named(ApiContext& context, const json& input) {
    const auto invoice = context.store->get_invoice(input.at("invoice_id"));
    if (!invoice) {
        return std::unexpected(ApiResult::failure(
            invoice.error().code == "invalid_input" ? "not_found" : invoice.error().code,
            invoice.error().message,
            "/invoice_id"));
    }
    return *invoice;
}

// ------------------------------------------------------------------------------------------------
// Where the provider comes from.

using ProviderOr = std::expected<std::shared_ptr<PaymentProvider>, ApiError>;

class FixedPaymentProviders final : public PaymentProviders {
public:
    FixedPaymentProviders(std::shared_ptr<PaymentProvider> provider, std::string reason)
        : provider_(std::move(provider)) {
        status_.provider = std::string(provider_->name());
        status_.title = std::string(provider_->title());
        status_.online_payment = !provider_->records_payments_by_hand();
        status_.reason = status_.online_payment ? std::string() : std::move(reason);
    }
    liquid::runtime::task<std::expected<std::shared_ptr<PaymentProvider>, ApiError>>
    current(ApiContext&, bool) override {
        co_return ProviderOr(provider_);
    }
    PaymentStatus status() const override {
        return status_;
    }

private:
    std::shared_ptr<PaymentProvider> provider_;
    PaymentStatus status_;
};

ApiResult from_api_error(const ApiError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

// The provider to use now, or why there is none.
struct Resolved {
    std::shared_ptr<PaymentProvider> provider;
    ApiResult failure;
};

liquid::runtime::task<Resolved>
resolve(PaymentProviders& providers, ApiContext& context, bool refresh = false) {
    auto current = co_await providers.current(context, refresh);
    if (!current) {
        co_return Resolved{nullptr, ApiResult::failure("store_error", current.error().message)};
    }
    if (!*current) {
        co_return Resolved{nullptr, from_api_error(current->error())};
    }
    co_return Resolved{**current, {}};
}

// Why an awaited resolve gave no provider.
template <class Awaited> ApiResult unresolved(const Awaited& awaited) {
    return awaited ? awaited->failure : ApiResult::failure("store_error", awaited.error().message);
}

// The account as the provider sees it, with its billing contact and the provider's id for it
// (created and kept first, so a failed payment request never loses it). A kept id the provider
// replaced (lake#856) is swapped only while the row still holds it, so of two concurrent
// replacements both use the one stored first; the swap is in the central audit
// (billing.account.provider_customer_replaced).
std::expected<PaymentAccount, ApiResult> payment_account(ApiContext& context,
                                                         PaymentProvider& provider,
                                                         const control::Customer& customer,
                                                         std::uint64_t attempt) {
    PaymentAccount account{customer.id, customer.name, {}, attempt, {}};
    const auto contact = context.store->billing_contact(customer.id);
    if (!contact) {
        return std::unexpected(from_store(contact.error()));
    }
    account.contact = *contact;
    const std::string name(provider.name());
    const auto kept = context.store->billing_provider_customer(customer.id, name);
    if (!kept) {
        return std::unexpected(from_store(kept.error()));
    }
    account.provider_customer_id = *kept;
    auto found = provider.provider_customer(account);
    if (!found) {
        return std::unexpected(ApiResult::failure(
            "store_unavailable", "the payment provider refused: " + found.error()));
    }
    if (!found->replaces.empty()) {
        const auto stored = context.store->replace_billing_provider_customer(
            customer.id, name, found->replaces, found->id);
        if (!stored) {
            return std::unexpected(from_store(stored.error()));
        }
        account.provider_customer_id = *stored;
        if (*stored != found->id) {
            return account; // another request's replacement, stored first, is its to audit
        }
        const audit::EventDraft event{
            .customer_id = customer.id,
            .kind = "billing.account.provider_customer_replaced",
            .principal = context.acting_identity.empty() ? "anonymous" : context.acting_identity,
            .object = "customer/" + customer.id + "/billing",
            .action = "issue_invoice",
            .outcome = "success",
            .request_id = context.request_id,
            .method = context.authentication_method,
            .device = context.device,
            .details = {{"provider", name},
                        {"old_provider_customer_id", found->replaces},
                        {"new_provider_customer_id", found->id}}};
        const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                             .instance_kek = context.instance_kek};
        if (const auto recorded = audit::record(*context.store, event, options); !recorded) {
            return std::unexpected(ApiResult::failure(
                "store_error", "could not record audit event: " + recorded.error().message));
        }
    } else if (!found->id.empty() && found->id != account.provider_customer_id) {
        const auto stored =
            context.store->set_billing_provider_customer(customer.id, name, found->id);
        if (!stored) {
            return std::unexpected(from_store(stored.error()));
        }
        account.provider_customer_id = *stored;
    }
    return account;
}

// An invoice after its payment request was sent to the billing email (lake#857), and the
// provider's refusal, if any.
struct SentRequest {
    control::Invoice invoice;
    std::string error;
};

// Sends a stored invoice's payment request and keeps the outcome on the invoice: provider_status
// `send_failed` when the provider refuses, and a send that goes through moves `send_failed` back to
// the provider's status; any other status is the provider's notices' to change (a reminder keeps
// `payment_failed`). The status moves only from the one read, so a notice applied meanwhile stays.
SentRequest send_request(ApiContext& context,
                         PaymentProvider& provider,
                         const PaymentAccount& account,
                         const control::Invoice& invoice,
                         std::string_view resend) {
    auto sent = provider.send_payment_request(account, invoice, resend);
    SentRequest out{invoice, sent ? std::string() : sent.error()};
    const std::string status = !sent                                      ? "send_failed"
                               : invoice.provider_status == "send_failed" ? *sent
                                                                          : invoice.provider_status;
    if (!status.empty() && status != invoice.provider_status) {
        if (auto moved = context.store->set_invoice_provider_status(
                invoice.id, invoice.provider_status, status)) {
            out.invoice = std::move(*moved);
        }
    }
    return out;
}

json send_details(const SentRequest& sent) {
    json details{{"invoice_id", sent.invoice.id},
                 {"number", sent.invoice.number},
                 {"period", sent.invoice.period},
                 {"provider", sent.invoice.provider},
                 {"provider_reference", sent.invoice.provider_reference},
                 {"provider_status", sent.invoice.provider_status}};
    if (!sent.error.empty()) {
        details["error"] = sent.error;
    }
    return details;
}

} // namespace

std::shared_ptr<PaymentProvider> manual_payment_provider(std::string instructions) {
    return std::make_shared<ManualPaymentProvider>(std::move(instructions));
}

bool billing_enabled(std::string_view value) {
    return value == "on" || value == "true" || value == "1";
}

bool billing_registered(const Registry* registry) {
    return registry != nullptr && registry->find("billing_account") != nullptr;
}

json payment_status_json(const ApiContext& context) {
    if (!billing_registered(context.registry) || !context.payments) {
        return nullptr;
    }
    const PaymentStatus status = context.payments->status();
    return {{"provider", status.provider},
            {"title", status.title},
            {"online_payment", status.online_payment},
            {"reason", status.reason}};
}

std::expected<PaymentAccount, ApiResult> billing_payment_account(ApiContext& context,
                                                                 PaymentProvider& provider,
                                                                 const control::Customer& customer,
                                                                 std::uint64_t attempt) {
    return payment_account(context, provider, customer, attempt);
}

liquid::runtime::task<std::expected<MonthBill, ApiResult>> account_month_bill(
    ApiContext& context, const std::string& customer_id, std::string period, bool audited) {
    using Answer = std::expected<MonthBill, ApiResult>;
    if (!context.store) {
        co_return Answer(std::unexpect, no_store());
    }
    const auto month = month_of(period);
    if (!month) {
        co_return Answer(std::unexpect,
                         ApiResult::failure("invalid_input", "period is YYYY-MM", "/period"));
    }
    auto list = list_for(context, *month);
    if (!list) {
        co_return Answer(std::unexpect, list.error());
    }
    auto measured = co_await measure(context, customer_id, *month, *list, audited);
    if (!measured) {
        co_return Answer(std::unexpect,
                         ApiResult::failure("store_error", measured.error().message));
    }
    if (measured->failure) {
        co_return Answer(std::unexpect, *measured->failure);
    }
    MonthBill bill;
    bill.period = month->name();
    bill.price_list_id = list->id;
    for (const control::InvoiceLine& line : bill_lines(*list, measured->quantities)) {
        bill.total_micros = add_capped(bill.total_micros, line.amount_micros);
    }
    bill.complete = measured->complete;
    for (const json& workspace : measured->unread) {
        bill.unread += (bill.unread.empty() ? "" : ", ") + workspace.value("name", "");
    }
    co_return Answer(std::move(bill));
}

liquid::runtime::task<std::expected<std::shared_ptr<PaymentProvider>, ApiResult>>
current_payment_provider(PaymentProviders& providers, ApiContext& context) {
    using Answer = std::expected<std::shared_ptr<PaymentProvider>, ApiResult>;
    auto resolved = co_await resolve(providers, context);
    if (!resolved || !resolved->provider) {
        co_return Answer(std::unexpect, unresolved(resolved));
    }
    co_return Answer(resolved->provider);
}

std::uint64_t price_of(std::uint64_t billable, std::uint64_t unit_micros, std::uint64_t per) {
    if (billable == 0 || unit_micros == 0) {
        return 0;
    }
    per = std::max<std::uint64_t>(per, 1);
    const unsigned __int128 product =
        static_cast<unsigned __int128>(billable) * static_cast<unsigned __int128>(unit_micros);
    const unsigned __int128 amount = (product + per - 1) / per;
    return amount > max_amount ? max_amount : static_cast<std::uint64_t>(amount);
}

std::string price_list_id(std::string_view effective_from, std::int64_t published_us) {
    const auto month = month_of(effective_from);
    if (!month || published_us < 0) {
        return {};
    }
    return std::format("usage-{:04}{:02}-{:017}", month->year, month->month, published_us);
}

std::vector<PriceList> price_lists(const std::vector<control::BillingPlan>& stored) {
    std::vector<PriceList> lists;
    for (const control::BillingPlan& row : stored) {
        // usage-YYYYMM-<17 digits>: anything else is not a price list publication.
        const std::string& id = row.id;
        if (id.size() != 30 || !id.starts_with("usage-") || id[12] != '-' ||
            !std::all_of(
                id.begin() + 6, id.begin() + 12, [](char c) { return c >= '0' && c <= '9'; }) ||
            !std::all_of(id.begin() + 13, id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        const std::string effective = id.substr(6, 4) + "-" + id.substr(10, 2);
        if (!month_of(effective)) {
            continue;
        }
        PriceList list;
        list.id = id;
        list.effective_from = effective;
        list.published_at = row.created_at.empty() ? row.updated_at : row.created_at;
        list.currency = row.currency;
        list.prices = row.prices;
        lists.push_back(std::move(list));
    }
    std::ranges::sort(lists, {}, &PriceList::id);
    return lists;
}

std::optional<PriceList> price_list_for(const std::vector<PriceList>& lists,
                                        std::string_view period) {
    // Ids sort by the month a list applies from, then by when it was published: the last id from
    // `period` or before is the list in force at the month's end.
    const PriceList* found = nullptr;
    for (const PriceList& list : lists) {
        if (list.effective_from <= period && (found == nullptr || list.id > found->id)) {
            found = &list;
        }
    }
    return found ? std::optional<PriceList>(*found) : std::nullopt;
}

std::uint64_t billable_quantity(std::string_view unit, std::uint64_t quantity) {
    // Time is billed by the minute (lake#637, lake#643): the month's summed usage, rounded up once
    // to a whole minute of the unit (a minute, a vCPU-minute, a GiB-minute), never each job, so
    // many short jobs are not each charged a minute.
    std::uint64_t minute = 0;
    if (unit == "seconds" || unit == "instance_seconds" || unit == "gpu_seconds") {
        minute = 60;
    } else if (unit == "mcpu_seconds") {
        minute = 60 * 1000;
    } else if (unit == "mib_seconds") {
        minute = 60 * 1024;
    }
    if (minute == 0) {
        return quantity;
    }
    const std::uint64_t minutes = quantity / minute + (quantity % minute == 0 ? 0 : 1);
    return minutes > max_amount / minute ? max_amount : minutes * minute;
}

std::vector<control::InvoiceLine> bill_lines(const PriceList& list, const UsageQuantities& usage) {
    // The classes rated on the list, by dimension: what their rates cover is not billed again at
    // the dimension's default rate.
    std::map<std::string, std::set<std::string>> rated;
    for (const control::BillingPrice& price : list.prices) {
        if (!price.class_name.empty()) {
            rated[price.dimension].insert(price.class_name);
        }
    }
    std::vector<control::InvoiceLine> lines;
    for (const control::BillingPrice& price : list.prices) {
        const UsageDimension* dimension = find_usage_dimension(price.dimension);
        const auto total = usage.totals.find(price.dimension);
        std::uint64_t quantity = total == usage.totals.end() ? 0 : total->second;
        const auto classes = usage.by_class.find(price.dimension);
        const auto class_quantity = [&](const std::string& name) -> std::uint64_t {
            if (classes == usage.by_class.end()) {
                return 0;
            }
            const auto found = classes->second.find(name);
            return found == classes->second.end() ? 0 : found->second;
        };
        const auto others = rated.find(price.dimension);
        std::string description =
            dimension ? std::string(dimension->title) : std::string(price.dimension);
        if (!price.class_name.empty()) {
            quantity = class_quantity(price.class_name);
            description += " (" + class_title(price.class_name) + ")";
        } else if (others != rated.end()) {
            // The default rate bills every class without its own, and usage of no known class.
            for (const std::string& name : others->second) {
                quantity -= std::min(quantity, class_quantity(name));
            }
            description += ", other classes";
        }
        const std::string unit = dimension ? std::string(dimension->unit) : std::string();
        const std::uint64_t billable = billable_quantity(unit, quantity);
        if (dimension && dimension->aggregation == "last") {
            description += ", average over the month";
        }
        lines.push_back({"usage",
                         price.dimension,
                         std::move(description),
                         unit,
                         quantity,
                         0,
                         billable,
                         price.unit_micros,
                         std::max<std::uint64_t>(price.per, 1),
                         price_of(billable, price.unit_micros, price.per),
                         price.class_name});
    }
    return lines;
}

std::vector<control::InvoiceLine>
bill_lines(const PriceList& list, const std::map<std::string, std::uint64_t>& quantities) {
    return bill_lines(list, UsageQuantities{quantities, {}});
}

std::shared_ptr<PaymentProviders> fixed_payment_providers(std::shared_ptr<PaymentProvider> provider,
                                                          std::string reason) {
    if (!provider) {
        provider = manual_payment_provider();
    }
    return std::make_shared<FixedPaymentProviders>(std::move(provider), std::move(reason));
}

std::shared_ptr<PaymentProviders> environment_payment_providers(const StripeKeys& keys,
                                                                std::string instructions,
                                                                const StripeProviderFactory& stripe) {
    const auto manual = [&](std::string reason) {
        return fixed_payment_providers(manual_payment_provider(std::move(instructions)),
                                       "Card payment is not set up: " + std::move(reason) + ".");
    };
    if (keys.secret_key.empty() && keys.webhook_secret.empty()) {
        return manual("LAKE_STRIPE_SECRET_KEY and LAKE_STRIPE_WEBHOOK_SECRET are not set");
    }
    if (keys.secret_key.empty()) {
        return manual("LAKE_STRIPE_SECRET_KEY is not set");
    }
    if (keys.webhook_secret.empty()) {
        return manual("LAKE_STRIPE_WEBHOOK_SECRET is not set");
    }
    // The shapes are checked here, so a mistyped value never reaches Stripe; the message names the
    // variable, never its value.
    if (!stripe_secret_key_shape(keys.secret_key)) {
        return manual("LAKE_STRIPE_SECRET_KEY is not a Stripe secret key (sk_ or rk_)");
    }
    if (!stripe_webhook_secret_shape(keys.webhook_secret)) {
        return manual("LAKE_STRIPE_WEBHOOK_SECRET is not a Stripe webhook secret (whsec_)");
    }
    std::shared_ptr<PaymentProvider> provider = stripe ? stripe(keys) : nullptr;
    if (!provider) {
        return manual("this Lake has no Stripe client");
    }
    return fixed_payment_providers(std::move(provider));
}

liquid::runtime::task<NoticeOutcome> receive_payment_notice(PaymentProviders& providers,
                                                            ApiContext& context,
                                                            std::string_view body,
                                                            std::string_view signature) {
    const auto record = [&](std::string customer_id,
                            std::string kind,
                            std::string object,
                            std::string outcome,
                            json details) {
        if (context.store == nullptr) {
            return true;
        }
        audit::EventDraft event{.customer_id = std::move(customer_id),
                                .kind = std::move(kind),
                                .principal = "payment-provider",
                                .object = std::move(object),
                                .action = "payment_notice",
                                .outcome = std::move(outcome),
                                .request_id = context.request_id,
                                .method = "webhook",
                                .details = std::move(details)};
        const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                             .instance_kek = context.instance_kek};
        return audit::record(*context.store, event, options).has_value();
    };
    const auto answer = [](int status, std::string code, std::string message) {
        return NoticeOutcome{status, {{"error", {{"code", code}, {"message", message}}}}};
    };
    if (context.store == nullptr) {
        co_return answer(503, "store_unavailable", "no control-plane store is configured");
    }
    auto resolved = co_await resolve(providers, context);
    if (!resolved || !resolved->provider) {
        const ApiResult failure = unresolved(resolved);
        co_return answer(503, failure.error.code, failure.error.message);
    }
    std::shared_ptr<PaymentProvider> provider = resolved->provider;
    if (provider->records_payments_by_hand()) {
        co_return answer(404, "not_found", "this Lake takes no payment notices");
    }
    const auto now = std::chrono::system_clock::now();
    auto notice = provider->read_notice(body, signature, now);
    if (!notice && notice.error().code == "invalid_signature") {
        // The webhook secret may have been rotated since it was read: check once more with the
        // newest keys before refusing.
        auto fresh = co_await resolve(providers, context, true);
        if (fresh && fresh->provider && fresh->provider != provider &&
            !fresh->provider->records_payments_by_hand()) {
            provider = fresh->provider;
            notice = provider->read_notice(body, signature, now);
        }
    }
    const std::string name(provider->name());
    if (!notice) {
        // A notice that does not authenticate is anyone's request (lake#1002): it is
        // counted, and at most one signed audit event a minute records how many were refused, so
        // unauthenticated traffic cannot append to the central audit log at will.
        static std::atomic<std::uint64_t> refused_since_summary{0};
        static std::atomic<std::int64_t> last_summary_seconds{0};
        refused_since_summary.fetch_add(1);
        const std::int64_t seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        std::int64_t last = last_summary_seconds.load();
        if (seconds - last >= 60 && last_summary_seconds.compare_exchange_strong(last, seconds)) {
            record({},
                   "billing.payment.notice",
                   "billing/" + name,
                   "denied",
                   {{"provider", name},
                    {"reason", notice.error().code},
                    {"refused_since_last_summary", refused_since_summary.exchange(0)}});
        }
        co_return answer(notice.error().code == "not_supported" ? 404 : 400,
                         notice.error().code,
                         notice.error().message);
    }
    if (notice->foreign) {
        // Another app's or another Lake's object in a shared Stripe account (lake#855): every
        // endpoint receives every event of its types, so it is acknowledged with nothing recorded,
        // neither a payment event row nor an audit event.
        co_return NoticeOutcome{200, {{"received", true}, {"ignored", true}}};
    }
    if (notice->balance) {
        // A top-up paid, a card saved for auto top-up, or an auto top-up that failed (lake#1114):
        // applied once per event id and once per payment, audited centrally.
        co_return apply_balance_notice(context, name, *notice);
    }
    if (notice->order_subscription) {
        notice->order_subscription->grace_seconds = context.order_entitlement_grace_seconds;
        const auto applied =
            context.store->apply_cloud_order_subscription(*notice->order_subscription);
        if (!applied) {
            co_return answer(applied.error().code == "invalid_input" ? 400 : 503,
                             applied.error().code,
                             applied.error().message);
        }
        if (applied->duplicate) {
            co_return NoticeOutcome{200, {{"received", true}, {"duplicate", true}}};
        }
        if (!applied->matched) {
            co_return NoticeOutcome{200, {{"received", true}, {"matched", false}}};
        }
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        co_return NoticeOutcome{
            200,
            {{"received", true},
             {"order_id", applied->order.id},
             {"state", applied->order.state},
             {"changed", applied->changed},
             {"entitlement_state",
              control::cloud_order_entitlement_state(applied->order.entitlement, seconds)}}};
    }
    if (!notice->order_state.empty()) {
        // An order of the apex order sheet (lake#831, docs/api/orders.md#paying): applied once per
        // event id, from pending_payment only. The order's event row is what reaches the central
        // audit log (the order audit pass), so a lost audit write never loses the change.
        const auto applied = context.store->apply_cloud_order_payment(
            control::CloudOrderPayment{name,
                                       notice->event_id,
                                       notice->event_type,
                                       notice->checkout_session,
                                       notice->order_id,
                                       notice->order_state,
                                       notice->customer,
                                       notice->subscription,
                                       notice->order_detail,
                                       context.order_entitlement_grace_seconds});
        if (!applied) {
            co_return answer(applied.error().code == "invalid_input" ? 400 : 503,
                             applied.error().code,
                             applied.error().message);
        }
        if (applied->duplicate) {
            co_return NoticeOutcome{200, {{"received", true}, {"duplicate", true}}};
        }
        if (!applied->matched) {
            record({},
                   "billing.payment.notice",
                   "billing/" + name,
                   "ignored",
                   {{"provider", name},
                    {"event_id", notice->event_id},
                    {"event_type", notice->event_type},
                    {"reference", notice->checkout_session}});
            co_return NoticeOutcome{200, {{"received", true}, {"matched", false}}};
        }
        co_return NoticeOutcome{200,
                                {{"received", true},
                                 {"order_id", applied->order.id},
                                 {"state", applied->order.state},
                                 {"changed", applied->changed}}};
    }
    if (notice->state.empty() && notice->provider_status.empty()) {
        // An event lake does not act on: acknowledged, so the provider stops sending it.
        co_return NoticeOutcome{200, {{"received", true}, {"ignored", true}}};
    }
    control::PaymentEvent event{
        name,
        notice->event_id,
        notice->event_type,
        notice->reference,
        notice->state,
        notice->provider_status,
        notice->state == "void" ? "Voided in " + std::string(provider->title()) : std::string()};
    const auto applied = context.store->apply_payment_event(event);
    if (!applied) {
        co_return answer(503, applied.error().code, applied.error().message);
    }
    json details{{"provider", name},
                 {"event_id", notice->event_id},
                 {"event_type", notice->event_type},
                 {"reference", notice->reference}};
    if (applied->duplicate) {
        // A second delivery of an event already applied (a provider's retry, or a replay inside
        // the tolerance window): it changes nothing.
        co_return NoticeOutcome{200, {{"received", true}, {"duplicate", true}}};
    }
    if (!applied->matched) {
        record({}, "billing.payment.notice", "billing/" + name, "ignored", std::move(details));
        co_return NoticeOutcome{200, {{"received", true}, {"matched", false}}};
    }
    const control::Invoice& invoice = applied->invoice;
    details["invoice_id"] = invoice.id;
    details["number"] = invoice.number;
    details["period"] = invoice.period;
    details["state"] = invoice.state;
    details["provider_status"] = invoice.provider_status;
    details["changed"] = applied->changed;
    const std::string kind = !applied->changed         ? "billing.payment.notice"
                             : notice->state == "paid" ? "billing.invoice.paid"
                             : notice->state == "void" ? "billing.invoice.void"
                                                       : "billing.invoice.payment_failed";
    if (!record(invoice.customer_id,
                kind,
                "customer/" + invoice.customer_id + "/billing",
                "success",
                std::move(details))) {
        co_return answer(503, "store_unavailable", "Lake could not record the notice");
    }
    co_return NoticeOutcome{200,
                            {{"received", true},
                             {"invoice_id", invoice.id},
                             {"state", invoice.state},
                             {"provider_status", invoice.provider_status},
                             {"changed", applied->changed}}};
}

void register_billing_capabilities(Registry& registry, std::shared_ptr<PaymentProvider> provider) {
    register_billing_capabilities(registry, fixed_payment_providers(std::move(provider)));
}

void register_billing_capabilities(Registry& registry,
                                   std::shared_ptr<PaymentProviders> providers) {
    if (!providers) {
        providers = fixed_payment_providers(nullptr);
    }
    json dimension_names = json::array();
    for (const UsageDimension& dimension : usage_dimensions()) {
        dimension_names.push_back(dimension.name);
    }

    registry.add(Capability{
        .name = "price_list",
        .title = "Price list",
        .description =
            "Orama Cloud's price list: one rate per usage dimension, `unit_micros` (millionths of "
            "the currency) for every `per` units of the dimension's unit; GPU and GPU memory may "
            "carry a rate per GPU `class` beside their default; model tokens have exact "
            "<model>/input and <model>/output class rates. Compute is priced as CPU "
            "(vCPU-minutes), RAM, GPU and GPU memory (GiB-minutes and GPU-minutes) of what each "
            "workload is scheduled with. Lake is priced by usage "
            "only: no plans, no monthly fee, nothing included; every account is billed at this "
            "list. `list` is the list in force for `period` (YYYY-MM; this month when left out), "
            "null before one is published; `next` is a list published for a later month, if any. "
            "A month is billed at the list in force at its end. `history` (every publication, "
            "newest first) only with `include_history` and the operator.",
        .input_schema = object_schema(
            {{"period", period_schema}, {"include_history", {{"type", "boolean"}}}}, json::array()),
        .output_schema =
            output_schema({{"period", string_schema()},
                           {"list", nullable_object()},
                           {"next", nullable_object()},
                           {"history", {{"type", "array"}, {"items", price_list_schema()}}}},
                          json::array({"period", "list", "next"})),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto month = input.contains("period")
                                   ? month_of(input.at("period").get<std::string>())
                                   : std::optional<Month>(this_month());
            if (!month) {
                return ApiResult::failure("invalid_input", "period is YYYY-MM", "/period");
            }
            auto lists = published_lists(context);
            if (!lists) {
                return lists.error();
            }
            const auto list = price_list_for(*lists, month->name());
            // The next list: the one in force for the first later month that has its own.
            std::optional<PriceList> next;
            for (const PriceList& candidate : *lists) {
                if (candidate.effective_from > month->name() &&
                    (!next || candidate.effective_from < next->effective_from ||
                     (candidate.effective_from == next->effective_from &&
                      candidate.id > next->id))) {
                    next = candidate;
                }
            }
            json out{{"period", month->name()},
                     {"list", optional_list_json(list)},
                     {"next", optional_list_json(next)}};
            if (input.value("include_history", false) && instance_administrator(context)) {
                json history = json::array();
                for (auto it = lists->rbegin(); it != lists->rend(); ++it) {
                    history.push_back(price_list_json(*it));
                }
                out["history"] = std::move(history);
            }
            return ApiResult::success(std::move(out));
        }});

    const json price_input = object_schema(
        {{"dimension", {{"type", "string"}, {"enum", dimension_names}}},
         {"class",
          {{"type", "string"},
           {"pattern", "^([a-z0-9][a-z0-9-]{0,31}|[!-~]{1,128}/(input|output))?$"},
           {"description",
            "a GPU class for gpu/vram, or <model>/input and <model>/output for model_tokens; "
            "empty is the default rate (historical unclassed usage for model_tokens)"}}},
         {"unit_micros", integer_schema()},
         {"per", integer_schema(1)}},
        json::array({"dimension", "unit_micros"}));
    registry.add(Capability{
        .name = "set_price_list",
        .title = "Publish the price list",
        .description =
            "Operator or instance administrator only. Publishes the price list: one rate per "
            "usage dimension (`unit_micros` for every `per` units of the dimension's unit; a "
            "dimension without a rate is not charged), and for gpu and vram optionally one rate "
            "per GPU `class` (the dimension's rate without a class bills every other GPU class). "
            "model_tokens uses <model>/input and <model>/output class rates; gateway calls require "
            "both to be positive, and a used model class without its exact rate refuses billing. "
            "build_compute, run_compute and instances are no longer priced (lake#643). In "
            "`currency` (USD by default), in force "
            "from the first day of `effective_from` (YYYY-MM; this month when left out). A month "
            "is billed at the list in force at its end, so a list published mid-month prices the "
            "whole month; invoices already issued keep their rates. Every publication is kept.",
        .input_schema =
            object_schema({{"currency", {{"type", "string"}, {"pattern", "^[A-Z]{3}$"}}},
                           {"effective_from", period_schema},
                           {"prices", {{"type", "array"}, {"items", price_input}}}},
                          json::array({"prices"})),
        .output_schema = price_list_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing price list",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return refused();
            }
            if (!context.store) {
                return no_store();
            }
            const auto month = input.contains("effective_from")
                                   ? month_of(input.at("effective_from").get<std::string>())
                                   : std::optional<Month>(this_month());
            if (!month) {
                return ApiResult::failure(
                    "invalid_input", "effective_from is YYYY-MM", "/effective_from");
            }
            control::BillingPlan row;
            row.name = "Usage";
            row.description = "Usage price list from " + month->name();
            row.currency = input.value("currency", "USD");
            row.base_micros = 0;
            row.active = true;
            std::set<std::pair<std::string, std::string>> seen;
            const json prices = input.at("prices");
            for (std::size_t index = 0; index < prices.size(); ++index) {
                const json& price = prices[index];
                const std::string at = "/prices/" + std::to_string(index);
                const std::string dimension = price.at("dimension");
                const std::string class_name = price.value("class", std::string());
                const UsageDimension* known = find_usage_dimension(dimension);
                if (known != nullptr && !known->priced) {
                    return ApiResult::failure(
                        "invalid_input",
                        dimension + " is measured but no longer priced: compute is priced as "
                                    "cpu, ram, gpu and vram (lake#643)",
                        at + "/dimension");
                }
                if (!class_name.empty() && (known == nullptr || !known->classes)) {
                    return ApiResult::failure(
                        "invalid_input", dimension + " has no classes to rate", at + "/class");
                }
                if (dimension == "model_tokens" && !class_name.empty() &&
                    !class_name.ends_with("/input") && !class_name.ends_with("/output")) {
                    return ApiResult::failure("invalid_input",
                                              "model token classes end in /input or /output",
                                              at + "/class");
                }
                if ((dimension == "gpu" || dimension == "vram") && !class_name.empty() &&
                    (class_name.size() > 32 || class_name.front() == '-' ||
                     !std::ranges::all_of(class_name, [](unsigned char c) {
                         return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                     }))) {
                    return ApiResult::failure("invalid_input", "invalid GPU class", at + "/class");
                }
                if (!seen.insert({dimension, class_name}).second) {
                    return ApiResult::failure("invalid_input",
                                              class_name.empty()
                                                  ? "each dimension has one rate"
                                                  : "each class of a dimension has one rate",
                                              at + "/dimension");
                }
                row.prices.push_back({dimension,
                                      price.at("unit_micros").get<std::uint64_t>(),
                                      price.value("per", std::uint64_t{1}),
                                      0,
                                      class_name});
            }
            auto lists = published_lists(context);
            if (!lists) {
                return lists.error();
            }
            std::set<std::string> taken;
            for (const PriceList& list : *lists) {
                taken.insert(list.id);
            }
            std::int64_t published_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
            row.id = price_list_id(month->name(), published_us);
            while (taken.contains(row.id)) {
                row.id = price_list_id(month->name(), ++published_us);
            }
            const auto stored = context.store->put_billing_plan(row);
            if (!stored) {
                return from_store(stored.error());
            }
            const auto published = price_lists({*stored});
            if (published.empty()) {
                return ApiResult::failure("store_error", "the price list was not kept");
            }
            context.audit_kind = "billing.price_list.set";
            context.audit_object = "billing_price_list/" + stored->id;
            context.audit_details = {{"price_list_id", stored->id},
                                     {"effective_from", month->name()},
                                     {"currency", stored->currency},
                                     {"prices", stored->prices.size()}};
            return ApiResult::success(price_list_json(published.front()));
        }});

    registry.add(Capability{
        .name = "billing_account",
        .title = "Billing account",
        .description =
            "An account's billing: the price list it is billed at this UTC month (`price_list`, "
            "null before one is published) and how invoices are paid (`provider`). Every account "
            "is billed for its usage at the price list; there is no plan to choose.",
        .input_schema =
            object_schema({{"customer_id", string_schema()}}, json::array({"customer_id"})),
        .output_schema = account_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [providers](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            auto resolved = co_await resolve(*providers, context);
            if (!resolved || !resolved->provider) {
                co_return unresolved(resolved);
            }
            const std::string customer_id = input.at("customer_id");
            ApiResult result =
                account_of(context, customer_id, *resolved->provider, providers->status());
            if (result.ok) {
                audit(context, customer_id, "billing.account.read");
            }
            co_return result;
        }});

    // set_billing_contact's input: the account and the contact's fields, each with the length
    // billing_contacts allows. The registry checks neither maxLength nor pattern, so the handler
    // applies billing_contact_problem.
    const auto contact_input = [] {
        static constexpr std::array<std::pair<const char*, int>, 7> bounded{{{"email", 320},
                                                                             {"name", 256},
                                                                             {"line1", 256},
                                                                             {"line2", 256},
                                                                             {"city", 128},
                                                                             {"postal_code", 32},
                                                                             {"state", 128}}};
        json properties{{"customer_id", string_schema()}};
        for (const auto& [field, most] : bounded) {
            properties[field] = {{"type", "string"}, {"maxLength", most}};
        }
        properties["country"] = {
            {"type", "string"},
            {"pattern", "^([A-Z]{2})?$"},
            {"description", "an ISO 3166-1 alpha-2 code such as DE, or empty"}};
        return object_schema(std::move(properties), json::array({"customer_id", "email"}));
    };
    registry.add(Capability{
        .name = "set_billing_contact",
        .title = "Set the billing contact",
        .description =
            "The account's owners and admins, and the operator. Replaces the account's billing "
            "contact: the `email` invoices go to, and the `name` (the legal or company name) and "
            "address an invoice carries, each empty when left out; `country` is an ISO 3166-1 "
            "alpha-2 code such as DE. When the account already has a customer at the payment "
            "provider (Stripe), Lake sends it the email, name and address (`provider_update` "
            "`updated`); if the provider refuses, the contact stays saved and `provider_update` "
            "is `failed` with `provider_error`. `none`: no provider customer yet, and it gets the "
            "contact when it is created at the account's first invoice.",
        .input_schema = contact_input(),
        .output_schema = output_schema(
            {{"customer_id", string_schema()},
             {"contact", contact_schema()},
             {"provider_update", {{"type", "string"}, {"enum", {"updated", "none", "failed"}}}},
             {"provider_error", string_schema()}},
            json::array({"customer_id", "contact", "provider_update"})),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [providers](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            // Each field as typed, without the whitespace a form leaves around it.
            const auto field = [&](const char* name) {
                std::string value = input.value(name, "");
                const auto blank = [](char c) {
                    return std::isspace(static_cast<unsigned char>(c));
                };
                while (!value.empty() && blank(value.back())) {
                    value.pop_back();
                }
                return value.substr(std::ranges::find_if_not(value, blank) - value.begin());
            };
            const control::BillingContact contact{input.at("customer_id"),
                                                  field("email"),
                                                  field("name"),
                                                  field("line1"),
                                                  field("line2"),
                                                  field("city"),
                                                  field("postal_code"),
                                                  field("state"),
                                                  field("country"),
                                                  {}};
            if (const auto problem = control::billing_contact_problem(contact)) {
                co_return ApiResult::failure("invalid_input",
                                             problem->field + " must be " + problem->message,
                                             "/" + problem->field);
            }
            const auto customer = context.store->get_customer(contact.customer_id);
            if (!customer) {
                co_return ApiResult::failure(
                    "not_found", "no account " + contact.customer_id, "/customer_id");
            }
            auto resolved = co_await resolve(*providers, context);
            if (!resolved || !resolved->provider) {
                co_return unresolved(resolved);
            }
            PaymentProvider& provider = *resolved->provider;
            const auto stored = context.store->set_billing_contact(contact);
            if (!stored) {
                co_return from_store(stored.error());
            }
            // The provider's customer, when the account has one, gets the contact now; one made
            // later gets it at creation (payment_account). A refusal keeps the saved contact.
            std::string update = "none";
            std::string error;
            const auto kept = context.store->billing_provider_customer(
                customer->id, std::string(provider.name()));
            if (!kept) {
                update = "failed";
                error = kept.error().message;
            } else if (!kept->empty()) {
                const PaymentAccount account{customer->id, customer->name, *kept, 0, *stored};
                const auto sent = provider.update_customer(account);
                update = sent ? "updated" : "failed";
                error = sent ? std::string() : sent.error();
            }
            const json saved = contact_json(*stored);
            // The audit names the fields set, never their values.
            json fields = json::array();
            for (const auto& [name, value] : saved.items()) {
                if (name != "updated_at" && !value.get<std::string>().empty()) {
                    fields.push_back(name);
                }
            }
            audit(context,
                  customer->id,
                  "billing.contact.set",
                  {{"fields", std::move(fields)},
                   {"provider", provider.name()},
                   {"provider_update", update}});
            json out{
                {"customer_id", customer->id}, {"contact", saved}, {"provider_update", update}};
            if (!error.empty()) {
                out["provider_error"] = error;
            }
            co_return ApiResult::success(std::move(out));
        }});

    registry.add(Capability{
        .name = "bill_preview",
        .title = "Bill preview",
        .description =
            "The bill for one UTC month (`period`, YYYY-MM; this month when left out): each usage "
            "dimension's quantity times its rate on the price list in force for the month, worked "
            "out now from the usage of every live workspace of the account, read from each "
            "workspace's own records. Nothing is stored. `complete` is "
            "false when a workspace's usage could not be read (`unread_workspaces`); `invoice` is "
            "the invoice already issued for the month, if any.",
        .input_schema = object_schema({{"customer_id", string_schema()}, {"period", period_schema}},
                                      json::array({"customer_id"})),
        .output_schema = bill_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto month = input.contains("period")
                                   ? month_of(input.at("period").get<std::string>())
                                   : std::optional<Month>(this_month());
            if (!month) {
                co_return ApiResult::failure("invalid_input", "period is YYYY-MM", "/period");
            }
            auto list = list_for(context, *month);
            if (!list) {
                co_return list.error();
            }
            auto measured = co_await measure(context, customer_id, *month, *list);
            if (!measured) {
                co_return ApiResult::failure("store_error", measured.error().message);
            }
            if (measured->failure) {
                co_return *measured->failure;
            }
            json bill = bill_json(
                customer_id, *month, *list, bill_lines(*list, measured->quantities), *measured);
            bill["invoice"] = nullptr;
            if (const auto invoices = context.store->list_invoices(customer_id)) {
                for (const control::Invoice& invoice : *invoices) {
                    if (invoice.period == month->name() && invoice.state != "void") {
                        bill["invoice"] = invoice_json(invoice);
                    }
                }
            }
            audit(context,
                  customer_id,
                  "billing.bill.preview",
                  {{"period", month->name()}, {"price_list_id", list->id}});
            co_return ApiResult::success(std::move(bill));
        }});

    registry.add(Capability{
        .name = "issue_invoice",
        .title = "Issue an invoice",
        .description =
            "Operator or instance administrator only. Issues the invoice for one UTC month that "
            "has ended: each usage dimension's quantity times its rate on the price list in force "
            "for that month, with each line's quantity and rate frozen, then asks the payment "
            "provider to collect "
            "it. Refused while any workspace's usage cannot be read (store_unavailable, try "
            "again) and when the month already has an invoice that is not void (conflict). Once "
            "the invoice is stored, and when the account has a billing contact, the provider "
            "sends it to the billing email (Stripe emails its hosted invoice link); a failed "
            "send leaves the invoice issued with `provider_status` `send_failed`, and "
            "resend_invoice sends it again.",
        .input_schema = object_schema({{"customer_id", string_schema()}, {"period", period_schema}},
                                      json::array({"customer_id", "period"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing invoice",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = [providers](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!instance_administrator(context)) {
                co_return refused();
            }
            if (!context.store) {
                co_return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto month = month_of(input.at("period").get<std::string>());
            if (!month) {
                co_return ApiResult::failure("invalid_input", "period is YYYY-MM", "/period");
            }
            if (month->next().start_day() >
                std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())) {
                co_return ApiResult::failure(
                    "conflict", month->name() + " has not ended yet", "/period");
            }
            const auto customer = context.store->get_customer(customer_id);
            if (!customer) {
                co_return ApiResult::failure(
                    "not_found", "no account " + customer_id, "/customer_id");
            }
            const auto existing = context.store->list_invoices(customer_id);
            if (!existing) {
                co_return from_store(existing.error());
            }
            // Which issue of the month this is: a provider keys its writes on it, so a retry of
            // one issue never bills twice and an issue after a void is a new request.
            std::uint64_t attempt = 0;
            for (const control::Invoice& invoice : *existing) {
                if (invoice.period == month->name() && invoice.state != "void") {
                    co_return ApiResult::failure("conflict",
                                                 "invoice " + std::to_string(invoice.number) +
                                                     " already bills " + month->name());
                }
                if (invoice.period == month->name()) {
                    ++attempt;
                }
            }
            auto list = list_for(context, *month);
            if (!list) {
                co_return list.error();
            }
            auto measured = co_await measure(context, customer_id, *month, *list);
            if (!measured) {
                co_return ApiResult::failure("store_error", measured.error().message);
            }
            if (measured->failure) {
                co_return *measured->failure;
            }
            if (!measured->complete) {
                std::string names;
                for (const json& workspace : measured->unread) {
                    names += (names.empty() ? "" : ", ") + workspace.value("name", "");
                }
                co_return ApiResult::failure("store_unavailable",
                                             "the usage of workspace " + names +
                                                 " could not be read; issue the invoice again "
                                                 "once it answers");
            }
            control::Invoice invoice;
            invoice.customer_id = customer_id;
            invoice.period = month->name();
            invoice.plan_id = list->id;
            invoice.plan_name = "Usage";
            invoice.currency = list->currency;
            invoice.lines = bill_lines(*list, measured->quantities);
            for (const control::InvoiceLine& line : invoice.lines) {
                invoice.total_micros = add_capped(invoice.total_micros, line.amount_micros);
            }
            // A prepaid account (lake#1114, docs/api/billing.md#balance) has paid for its usage
            // from its balance as it went: the invoice records what the balance paid, issued paid,
            // and freezes the month's charge to its total. Nothing is collected again.
            const auto balance = context.store->account_balance(customer_id);
            if (!balance) {
                co_return from_store(balance.error());
            }
            // An account on a grant from Orama alone paid from its balance too (lake#1134).
            if (!balance->charges_since().empty()) {
                invoice.provider = "balance";
                invoice.instructions = "Paid from your Lake account balance as you used it. "
                                       "Nothing more is due for this month.";
                invoice.provider_status = "paid";
                const auto stored = context.store->create_invoice(invoice);
                if (!stored) {
                    co_return from_store(stored.error());
                }
                const auto paid =
                    context.store->set_invoice_state(stored->id, "paid", "balance", "");
                if (!paid) {
                    co_return from_store(paid.error());
                }
                if (const auto frozen = context.store->set_balance_charge(
                        {customer_id,
                         paid->period,
                         static_cast<std::int64_t>(paid->total_micros),
                         paid->plan_id,
                         paid->id,
                         {}});
                    !frozen) {
                    co_return from_store(frozen.error());
                }
                audit(context,
                      customer_id,
                      "billing.invoice.issue",
                      {{"invoice_id", paid->id},
                       {"number", paid->number},
                       {"period", paid->period},
                       {"price_list_id", paid->plan_id},
                       {"total_micros", paid->total_micros},
                       {"currency", paid->currency},
                       {"provider", paid->provider},
                       {"provider_reference", paid->provider_reference},
                       {"send", "none"}});
                co_return ApiResult::success(invoice_json(*paid));
            }
            auto resolved = co_await resolve(*providers, context);
            if (!resolved || !resolved->provider) {
                co_return unresolved(resolved);
            }
            PaymentProvider& provider = *resolved->provider;
            invoice.provider = std::string(provider.name());
            auto account = payment_account(context, provider, *customer, attempt);
            if (!account) {
                co_return account.error();
            }
            auto request = provider.request_payment(*account, invoice);
            if (!request) {
                co_return ApiResult::failure("store_unavailable",
                                             "the payment provider refused: " + request.error());
            }
            invoice.provider_reference = request->reference;
            invoice.instructions = request->instructions;
            invoice.payment_url = request->payment_url;
            invoice.provider_status = request->provider_status;
            const auto stored = context.store->create_invoice(invoice);
            if (!stored) {
                co_return from_store(stored.error());
            }
            // The billing email gets the payment request only now that lake has stored the
            // invoice, so a payment notice that arrives meanwhile finds it (lake#857). A failed
            // send leaves the invoice issued and payable on its page, `send_failed` until
            // resend_invoice sends it.
            SentRequest sent{*stored, {}};
            std::string send = "none";
            if (account->contact && !stored->provider_reference.empty()) {
                sent = send_request(context, provider, *account, *stored, {});
                send = sent.error.empty() ? "sent" : "failed";
            }
            if (!sent.error.empty()) {
                // Its own central event beside the issue's: the issue itself succeeded.
                const audit::EventDraft failure{.customer_id = customer_id,
                                                .kind = "billing.invoice.send_failed",
                                                .principal = context.acting_identity,
                                                .object = "customer/" + customer_id + "/billing",
                                                .action = "issue_invoice",
                                                .outcome = "failure",
                                                .request_id = context.request_id,
                                                .method = context.authentication_method,
                                                .details = send_details(sent)};
                const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                                     .instance_kek = context.instance_kek};
                static_cast<void>(audit::record(*context.store, failure, options));
            }
            audit(context,
                  customer_id,
                  "billing.invoice.issue",
                  {{"invoice_id", stored->id},
                   {"number", stored->number},
                   {"period", stored->period},
                   {"price_list_id", stored->plan_id},
                   {"total_micros", stored->total_micros},
                   {"currency", stored->currency},
                   {"provider", stored->provider},
                   {"provider_reference", stored->provider_reference},
                   {"send", send}});
            co_return ApiResult::success(invoice_json(sent.invoice));
        }});

    registry.add(Capability{
        .name = "resend_invoice",
        .title = "Resend an invoice",
        .description =
            "Operator or instance administrator only. Sends a due invoice's payment request to the "
            "account's billing email again (lake#857): Stripe emails its hosted invoice link. For "
            "an invoice whose send failed when it was issued (`provider_status` `send_failed`, "
            "moved back to the provider's status once a send goes through), or as a reminder. "
            "Refused (conflict) for an invoice that is not due or was issued without a payment "
            "provider, and while the account has no billing contact; a refusal by the provider "
            "leaves the invoice `send_failed` (store_unavailable).",
        .input_schema =
            object_schema({{"invoice_id", string_schema()}}, json::array({"invoice_id"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing invoice",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = [providers](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!instance_administrator(context)) {
                co_return refused();
            }
            if (!context.store) {
                co_return no_store();
            }
            auto resolved = co_await resolve(*providers, context);
            if (!resolved || !resolved->provider) {
                co_return unresolved(resolved);
            }
            PaymentProvider& provider = *resolved->provider;
            auto invoice = invoice_named(context, input);
            if (!invoice) {
                co_return invoice.error();
            }
            const std::string named = "invoice " + std::to_string(invoice->number);
            if (invoice->state != "due") {
                co_return ApiResult::failure(
                    "conflict", named + " is " + invoice->state + ", not due", "/invoice_id");
            }
            if (invoice->provider_reference.empty()) {
                co_return ApiResult::failure("conflict",
                                             named + " has no payment request to send; its "
                                                     "instructions say how it is paid",
                                             "/invoice_id");
            }
            if (invoice->provider != provider.name()) {
                co_return ApiResult::failure("store_unavailable",
                                             named + " was issued through " + invoice->provider +
                                                 ", which is not configured now");
            }
            const auto customer = context.store->get_customer(invoice->customer_id);
            if (!customer) {
                co_return from_store(customer.error());
            }
            const auto invoices = context.store->list_invoices(invoice->customer_id);
            if (!invoices) {
                co_return from_store(invoices.error());
            }
            // Which issue of its month the invoice is: the provider's writes are keyed on it.
            const auto attempt = static_cast<std::uint64_t>(
                std::ranges::count_if(*invoices, [&](const control::Invoice& other) {
                    return other.period == invoice->period && other.number < invoice->number;
                }));
            auto account = payment_account(context, provider, *customer, attempt);
            if (!account) {
                co_return account.error();
            }
            if (!account->contact) {
                co_return ApiResult::failure("conflict",
                                             "the account has no billing email; its owners set "
                                             "one with set_billing_contact");
            }
            const SentRequest sent =
                send_request(context, provider, *account, *invoice, control::new_uuid_v4());
            audit(context,
                  invoice->customer_id,
                  sent.error.empty() ? "billing.invoice.send" : "billing.invoice.send_failed",
                  send_details(sent));
            if (!sent.error.empty()) {
                co_return ApiResult::failure("store_unavailable",
                                             "the payment provider refused: " + sent.error);
            }
            co_return ApiResult::success(invoice_json(sent.invoice));
        }});

    registry.add(Capability{
        .name = "list_invoices",
        .title = "List invoices",
        .description = "The account's invoices, newest month first, each with its lines and "
                       "payment state (due, paid or void).",
        .input_schema =
            object_schema({{"customer_id", string_schema()}}, json::array({"customer_id"})),
        .output_schema =
            output_schema({{"customer_id", string_schema()},
                           {"invoices", {{"type", "array"}, {"items", invoice_schema()}}}},
                          json::array({"customer_id", "invoices"})),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto invoices = context.store->list_invoices(customer_id);
            if (!invoices) {
                return from_store(invoices.error());
            }
            json out = json::array();
            for (const control::Invoice& invoice : *invoices) {
                out.push_back(invoice_json(invoice));
            }
            audit(context, customer_id, "billing.invoices.read", {{"invoices", out.size()}});
            return ApiResult::success({{"customer_id", customer_id}, {"invoices", std::move(out)}});
        }});

    registry.add(Capability{
        .name = "get_invoice",
        .title = "Get an invoice",
        .description = "One invoice of the account, with its lines, payment state and how to pay "
                       "it. not_found when the invoice is not this account's.",
        .input_schema =
            object_schema({{"customer_id", string_schema()}, {"invoice_id", string_schema()}},
                          json::array({"customer_id", "invoice_id"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            auto invoice = invoice_named(context, input);
            if (!invoice || invoice->customer_id != customer_id) {
                return invoice ? ApiResult::failure("not_found",
                                                    "no invoice " +
                                                        input.at("invoice_id").get<std::string>() +
                                                        " for this account",
                                                    "/invoice_id")
                               : invoice.error();
            }
            audit(context, customer_id, "billing.invoice.read", {{"invoice_id", invoice->id}});
            return ApiResult::success(invoice_json(*invoice));
        }});

    const auto settle = [providers](std::string state) {
        return [providers, state](const json& input,
                                  ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!instance_administrator(context)) {
                co_return refused();
            }
            if (!context.store) {
                co_return no_store();
            }
            auto resolved = co_await resolve(*providers, context);
            if (!resolved || !resolved->provider) {
                co_return unresolved(resolved);
            }
            PaymentProvider& provider = *resolved->provider;
            auto invoice = invoice_named(context, input);
            if (!invoice) {
                co_return invoice.error();
            }
            // Payments are recorded by hand only for an invoice whose provider takes them so: an
            // invoice issued by the manual provider stays recordable after Stripe is set up, and a
            // Stripe invoice is paid through Stripe, whichever provider is in use now.
            const bool by_hand =
                invoice->provider == "manual" || invoice->provider == "balance" ||
                (invoice->provider == provider.name() && provider.records_payments_by_hand());
            if (state == "paid" && !by_hand) {
                co_return ApiResult::failure("conflict",
                                             "the payment provider reports payments itself");
            }
            if (state == "void" && invoice->state == "due") {
                if (invoice->provider != std::string(provider.name())) {
                    if (!invoice->provider_reference.empty() && invoice->provider != "manual") {
                        co_return ApiResult::failure(
                            "store_unavailable",
                            "invoice " + std::to_string(invoice->number) + " was issued through " +
                                invoice->provider +
                                ", which is not configured now; void it there "
                                "first");
                    }
                } else if (auto cancelled = provider.cancel_payment(*invoice); !cancelled) {
                    co_return ApiResult::failure(
                        "store_unavailable", "the payment provider refused: " + cancelled.error());
                }
            }
            const auto stored = context.store->set_invoice_state(
                invoice->id, state, input.value("reference", ""), input.value("note", ""));
            if (!stored) {
                co_return from_store(stored.error());
            }
            audit(context,
                  stored->customer_id,
                  state == "paid" ? "billing.invoice.paid" : "billing.invoice.void",
                  {{"invoice_id", stored->id},
                   {"number", stored->number},
                   {"period", stored->period},
                   {"total_micros", stored->total_micros},
                   {"reference", stored->provider_reference},
                   {"provider", stored->provider}});
            co_return ApiResult::success(invoice_json(*stored));
        };
    };
    registry.add(Capability{
        .name = "record_invoice_payment",
        .title = "Record an invoice payment",
        .description =
            "Operator or instance administrator only. Marks a due invoice paid, with the payment's "
            "`reference` (a bank transfer's, say) and an optional `note`. For the manual "
            "provider, where Orama collects payment outside Lake.",
        .input_schema = object_schema({{"invoice_id", string_schema()},
                                       {"reference", {{"type", "string"}, {"maxLength", 256}}},
                                       {"note", {{"type", "string"}, {"maxLength", 1024}}}},
                                      json::array({"invoice_id"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing invoice",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = settle("paid")});
    registry.add(Capability{
        .name = "void_invoice",
        .title = "Void an invoice",
        .description =
            "Operator or instance administrator only. Voids a due invoice, saying why in `note`; "
            "the month can then be invoiced again. A paid invoice cannot be voided.",
        .input_schema =
            object_schema({{"invoice_id", string_schema()},
                           {"note", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}},
                          json::array({"invoice_id", "note"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing invoice",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = settle("void")});

    // The prepaid balance (lake#1114): billing's, so registered only where billing is on.
    register_balance_capabilities(registry, providers);
    // Coupon codes for ads (lake#1156): each redemption a grant from Orama.
    register_coupon_capabilities(registry);
}

} // namespace lake::api
