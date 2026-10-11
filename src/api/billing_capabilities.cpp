// Billing (lake#564, decision 56, docs/api/billing.md): billing_plans, set_billing_plan,
// billing_account, choose_billing_plan, bill_preview, issue_invoice, list_invoices, get_invoice,
// record_invoice_payment and void_invoice, with the manual payment provider. Registered only where
// LAKE_BILLING is on. The records are control-plane rows (plans, subscriptions, invoices and their
// lines); the usage a bill prices is read from each workspace Zetta through usage_over_time, and
// only the quantity billed per dimension is kept, on the invoice's lines. Every call is
// policy-checked and audited in the central log (decision 52): billing is account configuration.

#include "lake/api/billing.hpp"

#include <algorithm>
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

json price_json(const control::BillingPrice& price) {
    const UsageDimension* dimension = find_usage_dimension(price.dimension);
    return {{"dimension", price.dimension},
            {"title", dimension ? dimension->title : std::string_view(price.dimension)},
            {"unit", dimension ? dimension->unit : std::string_view()},
            {"unit_micros", price.unit_micros},
            {"per", price.per},
            {"included", price.included}};
}

json plan_json(const control::BillingPlan& plan) {
    json prices = json::array();
    for (const control::BillingPrice& price : plan.prices) {
        prices.push_back(price_json(price));
    }
    return {{"id", plan.id},
            {"name", plan.name},
            {"description", plan.description},
            {"currency", plan.currency},
            {"base_micros", plan.base_micros},
            {"active", plan.active},
            {"position", plan.position},
            {"prices", std::move(prices)},
            {"updated_at", plan.updated_at}};
}

json line_json(const control::InvoiceLine& line) {
    return {{"kind", line.kind},
            {"dimension", line.dimension},
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
            {"plan_id", invoice.plan_id},
            {"plan_name", invoice.plan_name},
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

json plan_schema() {
    return output_schema(
        {{"id", string_schema()},
         {"name", string_schema()},
         {"description", string_schema()},
         {"currency", string_schema()},
         {"base_micros", {{"type", "integer"}}},
         {"active", {{"type", "boolean"}}},
         {"position", {{"type", "integer"}}},
         {"prices", {{"type", "array"}, {"items", {{"type", "object"}}}}},
         {"updated_at", string_schema()}},
        json::array({"id", "name", "currency", "base_micros", "active", "prices"}));
}

json invoice_schema() {
    return output_schema({{"id", string_schema()},
                          {"number", {{"type", "integer"}}},
                          {"customer_id", string_schema()},
                          {"period", string_schema()},
                          {"plan_id", string_schema()},
                          {"plan_name", string_schema()},
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
                          {"lines", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
                         json::array({"id",
                                      "number",
                                      "customer_id",
                                      "period",
                                      "plan_id",
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
// Plans and subscriptions.

std::expected<control::BillingPlan, ApiResult> find_plan(ApiContext& context,
                                                         const std::string& id) {
    const auto plans = context.store->billing_plans();
    if (!plans) {
        return std::unexpected(from_store(plans.error()));
    }
    for (const control::BillingPlan& plan : *plans) {
        if (plan.id == id) {
            return plan;
        }
    }
    return std::unexpected(ApiResult::failure("not_found", "no plan " + id, "/plan_id"));
}

// The subscription row in effect on the first day of `month`, if any.
std::optional<control::BillingSubscription>
in_effect(const std::vector<control::BillingSubscription>& rows, const Month& month) {
    std::optional<control::BillingSubscription> found;
    const std::string first = month.first_day();
    for (const control::BillingSubscription& row : rows) {
        if (row.effective_from <= first) {
            found = row;
        }
    }
    return found;
}

// The plan an account is billed on for `month`, or a conflict naming why there is none.
std::expected<control::BillingPlan, ApiResult>
plan_for(ApiContext& context, const std::string& customer_id, const Month& month) {
    const auto rows = context.store->billing_subscriptions(customer_id);
    if (!rows) {
        return std::unexpected(from_store(rows.error()));
    }
    const auto row = in_effect(*rows, month);
    if (!row) {
        return std::unexpected(ApiResult::failure("conflict",
                                                  "this account has no plan for " + month.name() +
                                                      "; choose one with "
                                                      "choose_billing_plan"));
    }
    auto plan = find_plan(context, row->plan_id);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return plan;
}

// ------------------------------------------------------------------------------------------------
// Measuring a month: usage_over_time, called as the same caller, by day.

struct Measured {
    std::optional<ApiResult> failure; // why the usage could not be read at all
    std::map<std::string, std::uint64_t> quantities;
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

liquid::runtime::task<Measured> measure(ApiContext& context,
                                        const std::string& customer_id,
                                        const Month& month,
                                        const control::BillingPlan& plan) {
    Measured measured;
    json dimensions = json::array();
    for (const control::BillingPrice& price : plan.prices) {
        dimensions.push_back(price.dimension);
    }
    if (dimensions.empty()) {
        co_return measured;
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
    auto done = co_await context.registry->call_async("usage_over_time", input, inner);
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
            measured.quantities[name] = count == 0 ? 0 : whole(sum / static_cast<double>(count));
        } else {
            measured.quantities[name] = whole(entry.value("total", 0.0));
        }
    }
    co_return measured;
}

json bill_json(const std::string& customer_id,
               const Month& month,
               const control::BillingPlan& plan,
               const std::vector<control::InvoiceLine>& lines,
               const Measured& measured) {
    std::uint64_t total = 0;
    for (const control::InvoiceLine& line : lines) {
        total = add_capped(total, line.amount_micros);
    }
    return {{"customer_id", customer_id},
            {"period", month.name()},
            {"plan", {{"id", plan.id}, {"name", plan.name}}},
            {"currency", plan.currency},
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
    std::expected<PaymentRequest, std::string> request_payment(const control::Invoice&) override {
        return PaymentRequest{
            {},
            instructions_.empty()
                ? "Orama will send you payment details for this invoice. It is paid once Orama "
                  "records your payment."
                : instructions_};
    }
    bool records_payments_by_hand() const override {
        return true;
    }

private:
    std::string instructions_;
};

json provider_json(const PaymentProvider& provider) {
    return {{"name", provider.name()},
            {"title", provider.title()},
            {"records_payments_by_hand", provider.records_payments_by_hand()}};
}

json subscription_json(const control::BillingSubscription& row,
                       const std::vector<control::BillingPlan>& plans) {
    json plan = nullptr;
    for (const control::BillingPlan& candidate : plans) {
        if (candidate.id == row.plan_id) {
            plan = plan_json(candidate);
        }
    }
    return {{"effective_from", row.effective_from}, {"plan_id", row.plan_id}, {"plan", plan}};
}

// The account's billing: its plan this month, the plan chosen for next month, and its history.
ApiResult
account_of(ApiContext& context, const std::string& customer_id, const PaymentProvider& provider) {
    const auto rows = context.store->billing_subscriptions(customer_id);
    if (!rows) {
        return from_store(rows.error());
    }
    const auto plans = context.store->billing_plans();
    if (!plans) {
        return from_store(plans.error());
    }
    const Month now = this_month();
    json current = nullptr;
    json next = nullptr;
    json history = json::array();
    if (const auto row = in_effect(*rows, now)) {
        current = subscription_json(*row, *plans);
    }
    for (const control::BillingSubscription& row : *rows) {
        history.push_back({{"effective_from", row.effective_from}, {"plan_id", row.plan_id}});
        if (row.effective_from > now.first_day()) {
            next = subscription_json(row, *plans);
        }
    }
    return ApiResult::success({{"customer_id", customer_id},
                               {"month", now.name()},
                               {"current", std::move(current)},
                               {"next", std::move(next)},
                               {"history", std::move(history)},
                               {"provider", provider_json(provider)}});
}

json account_schema() {
    return output_schema(
        {{"customer_id", string_schema()},
         {"month", string_schema()},
         {"current", {{"type", json::array({"object", "null"})}}},
         {"next", {{"type", json::array({"object", "null"})}}},
         {"history", {{"type", "array"}, {"items", {{"type", "object"}}}}},
         {"provider", {{"type", "object"}}}},
        json::array({"customer_id", "month", "current", "next", "history", "provider"}));
}

json bill_schema() {
    return output_schema(
        {{"customer_id", string_schema()},
         {"period", string_schema()},
         {"plan", {{"type", "object"}}},
         {"currency", string_schema()},
         {"lines", {{"type", "array"}, {"items", {{"type", "object"}}}}},
         {"total_micros", {{"type", "integer"}}},
         {"complete", {{"type", "boolean"}}},
         {"unread_workspaces", {{"type", "array"}, {"items", {{"type", "object"}}}}},
         {"not_measured", {{"type", "array"}, {"items", string_schema()}}},
         {"invoice", {{"type", json::array({"object", "null"})}}}},
        json::array({"customer_id",
                     "period",
                     "plan",
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

std::vector<control::InvoiceLine> bill_lines(const control::BillingPlan& plan,
                                             const std::map<std::string, std::uint64_t>& quantities,
                                             std::string_view period) {
    std::vector<control::InvoiceLine> lines;
    lines.push_back({"plan",
                     "",
                     plan.name + " plan, " + std::string(period),
                     "month",
                     1,
                     0,
                     1,
                     plan.base_micros,
                     1,
                     plan.base_micros});
    for (const control::BillingPrice& price : plan.prices) {
        const UsageDimension* dimension = find_usage_dimension(price.dimension);
        const auto found = quantities.find(price.dimension);
        const std::uint64_t quantity = found == quantities.end() ? 0 : found->second;
        const std::uint64_t billable = quantity > price.included ? quantity - price.included : 0;
        std::string description =
            dimension ? std::string(dimension->title) : std::string(price.dimension);
        if (dimension && dimension->aggregation == "last") {
            description += ", average over the month";
        }
        lines.push_back({"usage",
                         price.dimension,
                         std::move(description),
                         dimension ? std::string(dimension->unit) : std::string(),
                         quantity,
                         price.included,
                         billable,
                         price.unit_micros,
                         std::max<std::uint64_t>(price.per, 1),
                         price_of(billable, price.unit_micros, price.per)});
    }
    return lines;
}

void register_billing_capabilities(Registry& registry, std::shared_ptr<PaymentProvider> provider) {
    if (!provider) {
        provider = manual_payment_provider();
    }
    json dimension_names = json::array();
    for (const UsageDimension& dimension : usage_dimensions()) {
        dimension_names.push_back(dimension.name);
    }

    registry.add(Capability{
        .name = "billing_plans",
        .title = "Billing plans",
        .description =
            "The price list: each plan's monthly fee and, per usage dimension, its price for every "
            "`per` units of the dimension's unit beyond what the plan includes each month. "
            "Amounts are millionths of the plan's currency. Retired plans are listed only with "
            "`include_retired` (the operator).",
        .input_schema = object_schema({{"include_retired", {{"type", "boolean"}}}}, json::array()),
        .output_schema = output_schema({{"plans", {{"type", "array"}, {"items", plan_schema()}}}},
                                       json::array({"plans"})),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto plans = context.store->billing_plans();
            if (!plans) {
                return from_store(plans.error());
            }
            const bool retired =
                input.value("include_retired", false) && instance_administrator(context);
            json out = json::array();
            for (const control::BillingPlan& plan : *plans) {
                if (plan.active || retired) {
                    out.push_back(plan_json(plan));
                }
            }
            return ApiResult::success({{"plans", std::move(out)}});
        }});

    const json price_input =
        object_schema({{"dimension", {{"type", "string"}, {"enum", dimension_names}}},
                       {"unit_micros", integer_schema()},
                       {"per", integer_schema(1)},
                       {"included", integer_schema()}},
                      json::array({"dimension", "unit_micros"}));
    registry.add(Capability{
        .name = "set_billing_plan",
        .title = "Set a billing plan",
        .description =
            "Operator or instance administrator only. Creates or replaces one plan of the price "
            "list: its name, monthly fee (`base_micros`), currency and its prices, one per usage "
            "dimension (`unit_micros` for every `per` units beyond `included` units a month). A "
            "dimension without a price is not charged. `active: false` retires the plan: no "
            "account can choose it, and accounts on it stay on it.",
        .input_schema =
            object_schema({{"id", {{"type", "string"}, {"pattern", "^[a-z][a-z0-9-]{0,62}$"}}},
                           {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                           {"description", {{"type", "string"}, {"maxLength", 1024}}},
                           {"currency", {{"type", "string"}, {"pattern", "^[A-Z]{3}$"}}},
                           {"base_micros", integer_schema()},
                           {"active", {{"type", "boolean"}}},
                           {"position", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}},
                           {"prices", {{"type", "array"}, {"items", price_input}}}},
                          json::array({"id", "name", "base_micros"})),
        .output_schema = plan_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing plan",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return refused();
            }
            if (!context.store) {
                return no_store();
            }
            control::BillingPlan plan;
            plan.id = input.at("id");
            plan.name = input.at("name");
            plan.description = input.value("description", "");
            plan.currency = input.value("currency", "USD");
            plan.base_micros = input.at("base_micros").get<std::uint64_t>();
            plan.active = input.value("active", true);
            plan.position = input.value("position", 0);
            std::set<std::string> seen;
            const json prices = input.value("prices", json::array());
            for (std::size_t index = 0; index < prices.size(); ++index) {
                const json& price = prices[index];
                const std::string dimension = price.at("dimension");
                if (!seen.insert(dimension).second) {
                    return ApiResult::failure("invalid_input",
                                              "each dimension has one price",
                                              "/prices/" + std::to_string(index) + "/dimension");
                }
                plan.prices.push_back({dimension,
                                       price.at("unit_micros").get<std::uint64_t>(),
                                       price.value("per", std::uint64_t{1}),
                                       price.value("included", std::uint64_t{0})});
            }
            const auto stored = context.store->put_billing_plan(plan);
            if (!stored) {
                return from_store(stored.error());
            }
            context.audit_kind = "billing.plan.set";
            context.audit_object = "billing_plan/" + plan.id;
            context.audit_details = {{"plan_id", plan.id},
                                     {"base_micros", plan.base_micros},
                                     {"currency", plan.currency},
                                     {"active", plan.active},
                                     {"prices", plan.prices.size()}};
            return ApiResult::success(plan_json(*stored));
        }});

    registry.add(Capability{
        .name = "billing_account",
        .title = "Billing account",
        .description =
            "An account's billing: the plan it is billed on this UTC month (`current`), the plan "
            "chosen from next month (`next`, when it changes), every plan change, and how "
            "invoices are paid (`provider`).",
        .input_schema =
            object_schema({{"customer_id", string_schema()}}, json::array({"customer_id"})),
        .output_schema = account_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [provider](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            ApiResult result = account_of(context, customer_id, *provider);
            if (result.ok) {
                audit(context, customer_id, "billing.account.read");
            }
            return result;
        }});

    registry.add(Capability{
        .name = "choose_billing_plan",
        .title = "Choose a billing plan",
        .description =
            "Choose the account's plan. An account without a plan is billed on it from the start "
            "of this UTC month; otherwise the change starts next month, and choosing the current "
            "plan again cancels a change not yet started. Only an active plan can be chosen.",
        .input_schema =
            object_schema({{"customer_id", string_schema()}, {"plan_id", string_schema()}},
                          json::array({"customer_id", "plan_id"})),
        .output_schema = account_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [provider](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string plan_id = input.at("plan_id");
            auto plan = find_plan(context, plan_id);
            if (!plan) {
                return plan.error();
            }
            if (!plan->active) {
                return ApiResult::failure(
                    "invalid_input", "plan " + plan_id + " is retired", "/plan_id");
            }
            const auto rows = context.store->billing_subscriptions(customer_id);
            if (!rows) {
                return from_store(rows.error());
            }
            const Month now = this_month();
            const auto current = in_effect(*rows, now);
            std::string effective_from;
            control::Result<std::vector<control::BillingSubscription>> written;
            if (!current) {
                effective_from = now.first_day();
                written =
                    context.store->set_billing_subscription(customer_id, effective_from, plan_id);
            } else if (current->plan_id == plan_id) {
                effective_from = current->effective_from;
                written = context.store->set_billing_subscription(
                    customer_id, now.next().first_day(), "");
            } else {
                effective_from = now.next().first_day();
                written =
                    context.store->set_billing_subscription(customer_id, effective_from, plan_id);
            }
            if (!written) {
                return from_store(written.error());
            }
            ApiResult result = account_of(context, customer_id, *provider);
            if (result.ok) {
                audit(context,
                      customer_id,
                      "billing.plan.choose",
                      {{"plan_id", plan_id},
                       {"effective_from", effective_from},
                       {"previous_plan_id", current ? current->plan_id : std::string()}});
            }
            return result;
        }});

    registry.add(Capability{
        .name = "bill_preview",
        .title = "Bill preview",
        .description =
            "The bill for one UTC month (`period`, YYYY-MM; this month when left out) on the plan "
            "in effect that month, worked out now from the usage of every live workspace of the "
            "account, read from each workspace's own records. Nothing is stored. `complete` is "
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
            auto plan = plan_for(context, customer_id, *month);
            if (!plan) {
                co_return plan.error();
            }
            auto measured = co_await measure(context, customer_id, *month, *plan);
            if (!measured) {
                co_return ApiResult::failure("store_error", measured.error().message);
            }
            if (measured->failure) {
                co_return *measured->failure;
            }
            json bill = bill_json(customer_id,
                                  *month,
                                  *plan,
                                  bill_lines(*plan, measured->quantities, month->name()),
                                  *measured);
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
                  {{"period", month->name()}, {"plan_id", plan->id}});
            co_return ApiResult::success(std::move(bill));
        }});

    registry.add(Capability{
        .name = "issue_invoice",
        .title = "Issue an invoice",
        .description =
            "Operator or instance administrator only. Issues the invoice for one UTC month that "
            "has ended: the bill worked out from the account's usage on the plan in effect that "
            "month, with each line's quantity frozen, then asks the payment provider to collect "
            "it. Refused while any workspace's usage cannot be read (store_unavailable, try "
            "again) and when the month already has an invoice that is not void (conflict).",
        .input_schema = object_schema({{"customer_id", string_schema()}, {"period", period_schema}},
                                      json::array({"customer_id", "period"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing invoice",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = [provider](const json& input,
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
            if (const auto customer = context.store->get_customer(customer_id); !customer) {
                co_return ApiResult::failure(
                    "not_found", "no account " + customer_id, "/customer_id");
            }
            const auto existing = context.store->list_invoices(customer_id);
            if (!existing) {
                co_return from_store(existing.error());
            }
            for (const control::Invoice& invoice : *existing) {
                if (invoice.period == month->name() && invoice.state != "void") {
                    co_return ApiResult::failure("conflict",
                                                 "invoice " + std::to_string(invoice.number) +
                                                     " already bills " + month->name());
                }
            }
            auto plan = plan_for(context, customer_id, *month);
            if (!plan) {
                co_return plan.error();
            }
            auto measured = co_await measure(context, customer_id, *month, *plan);
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
            invoice.plan_id = plan->id;
            invoice.plan_name = plan->name;
            invoice.currency = plan->currency;
            invoice.lines = bill_lines(*plan, measured->quantities, month->name());
            for (const control::InvoiceLine& line : invoice.lines) {
                invoice.total_micros = add_capped(invoice.total_micros, line.amount_micros);
            }
            invoice.provider = std::string(provider->name());
            auto request = provider->request_payment(invoice);
            if (!request) {
                co_return ApiResult::failure("store_unavailable",
                                             "the payment provider refused: " + request.error());
            }
            invoice.provider_reference = request->reference;
            invoice.instructions = request->instructions;
            const auto stored = context.store->create_invoice(invoice);
            if (!stored) {
                co_return from_store(stored.error());
            }
            audit(context,
                  customer_id,
                  "billing.invoice.issue",
                  {{"invoice_id", stored->id},
                   {"number", stored->number},
                   {"period", stored->period},
                   {"plan_id", stored->plan_id},
                   {"total_micros", stored->total_micros},
                   {"currency", stored->currency},
                   {"provider", stored->provider}});
            co_return ApiResult::success(invoice_json(*stored));
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

    const auto settle = [provider](std::string state) {
        return [provider, state](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return refused();
            }
            if (!context.store) {
                return no_store();
            }
            if (state == "paid" && !provider->records_payments_by_hand()) {
                return ApiResult::failure("conflict",
                                          "the payment provider reports payments itself");
            }
            auto invoice = invoice_named(context, input);
            if (!invoice) {
                return invoice.error();
            }
            const auto stored = context.store->set_invoice_state(
                invoice->id, state, input.value("reference", ""), input.value("note", ""));
            if (!stored) {
                return from_store(stored.error());
            }
            audit(context,
                  stored->customer_id,
                  state == "paid" ? "billing.invoice.paid" : "billing.invoice.void",
                  {{"invoice_id", stored->id},
                   {"number", stored->number},
                   {"period", stored->period},
                   {"total_micros", stored->total_micros},
                   {"reference", stored->provider_reference}});
            return ApiResult::success(invoice_json(*stored));
        };
    };
    registry.add(Capability{
        .name = "record_invoice_payment",
        .title = "Record an invoice payment",
        .description =
            "Operator or instance administrator only. Marks a due invoice paid, with the payment's "
            "`reference` (a bank transfer's, say) and an optional `note`. For the manual "
            "provider, where Orama collects payment outside lake.",
        .input_schema = object_schema({{"invoice_id", string_schema()},
                                       {"reference", {{"type", "string"}, {"maxLength", 256}}},
                                       {"note", {{"type", "string"}, {"maxLength", 1024}}}},
                                      json::array({"invoice_id"})),
        .output_schema = invoice_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing invoice",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = settle("paid")});
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
        .handler = settle("void")});
}

} // namespace lake::api
