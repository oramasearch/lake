// SPDX-License-Identifier: MIT

// Stripe, lake's payment provider (lake#620, docs/api/billing.md#stripe): lake's own
// client for Stripe's REST API and the check of its webhook signatures. No Stripe SDK ("We write
// our own"); the HTTP exchange is injected (StripeHttp), the server's outbound client in lake.

#include "lake/api/stripe.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <optional>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace lake::api {
namespace {

using nlohmann::json;

std::string percent_encode(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0F]);
        }
    }
    return out;
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(),
             key.data(),
             static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()),
             data.size(),
             digest.data(),
             &length) == nullptr) {
        return {};
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index) {
        out.push_back(hex[digest[index] >> 4]);
        out.push_back(hex[digest[index] & 0x0F]);
    }
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Stripe's own message for a failed call, except on 401/403, where it can echo part of the key.
std::string stripe_failure(std::string_view what, const StripeResponse& response) {
    if (!response.error.empty()) {
        return std::format("Stripe could not be reached to {}: {}", what, response.error);
    }
    if (response.status == 401 || response.status == 403) {
        return std::format(
            "Stripe refused Lake's key to {} (HTTP {}); check LAKE_STRIPE_SECRET_KEY",
            what,
            response.status);
    }
    const json body = json::parse(response.body, nullptr, false);
    std::string message;
    if (body.is_object() && body.contains("error") && body["error"].is_object()) {
        message = body["error"].value("message", "");
    }
    if (message.empty()) {
        message = "no message";
    }
    return std::format("Stripe refused to {} (HTTP {}): {}", what, response.status, message);
}

// The product a refused request named, when Stripe answers that it does not exist
// (resource_missing on `price_data[product]`, or `line_items[0][price_data][product]` for a
// Checkout): the operator has not created Lake's products in this account yet. Empty for any other
// refusal.
std::string missing_product(const StripeResponse& response,
                            const std::vector<std::pair<std::string, std::string>>& fields) {
    const json body = json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.contains("error") || !body["error"].is_object()) {
        return {};
    }
    const json& error = body["error"];
    const auto text = [&](const char* key) {
        return error.contains(key) && error[key].is_string() ? error[key].get<std::string>()
                                                             : std::string();
    };
    const std::string param = text("param");
    if (text("code") != "resource_missing" ||
        !(param == "price_data[product]" || param.ends_with("[price_data][product]"))) {
        return {};
    }
    const auto named =
        std::ranges::find(fields, param, &std::pair<std::string, std::string>::first);
    return named == fields.end() ? std::string() : named->second;
}

// Whether Stripe answered that the object read does not exist in this account and mode (404
// resource_missing), as for a customer kept from another sandbox or from test mode (lake#856).
bool missing_object(const StripeResponse& response) {
    if (!response.error.empty() || response.status != 404) {
        return false;
    }
    const json body = json::parse(response.body, nullptr, false);
    return body.is_object() && body.contains("error") && body["error"].is_object() &&
           body["error"].contains("code") && body["error"]["code"] == "resource_missing";
}

// A successful answer's JSON object.
std::expected<json, std::string> answer_object(const StripeResponse& response,
                                               std::string_view what) {
    json body = json::parse(response.body, nullptr, false);
    if (!body.is_object()) {
        return std::unexpected(std::format("Stripe answered {} with no JSON object", what));
    }
    return body;
}

// The tags on every object Lake creates in a Stripe account it shares with Orama Cloud v1 and with
// other Lakes (lake#855), as fields under `prefix` (`metadata`,
// `subscription_data[metadata]`): orama_app=lake, and lake_instance, this Lake's public host, when
// it has one. Lake's own keys are lake_*. It never sets Cloud v1's bare team_id, plan_id,
// addon_type or addon_config: Cloud v1's webhook handlers read those, so a Lake object carrying one
// could be taken for Cloud v1's.
void tag(std::vector<std::pair<std::string, std::string>>& fields,
         std::string_view prefix,
         std::string_view instance) {
    fields.emplace_back(std::format("{}[orama_app]", prefix), "lake");
    if (!instance.empty()) {
        fields.emplace_back(std::format("{}[lake_instance]", prefix), instance);
    }
}

// A customer's billing details (lake#857): the billing contact's name, or else the account's, and,
// once an owner has set a contact, its email and address. An update sends every address field, so
// a field the contact no longer has is cleared, and `address=` when it has no address at all; a new
// customer gets only the fields the contact has.
std::vector<std::pair<std::string, std::string>> billing_details(const PaymentAccount& account,
                                                                 bool update) {
    const std::string& account_name = account.name.empty() ? account.customer_id : account.name;
    if (!account.contact) {
        return {{"name", account_name}};
    }
    const control::BillingContact& contact = *account.contact;
    std::vector<std::pair<std::string, std::string>> fields{
        {"name", contact.name.empty() ? account_name : contact.name}, {"email", contact.email}};
    const std::array<std::pair<std::string_view, const std::string*>, 6> address{{
        {"line1", &contact.line1},
        {"line2", &contact.line2},
        {"city", &contact.city},
        {"postal_code", &contact.postal_code},
        {"state", &contact.state},
        {"country", &contact.country},
    }};
    const bool addressed =
        std::ranges::any_of(address, [](const auto& field) { return !field.second->empty(); });
    if (!addressed) {
        if (update) {
            fields.emplace_back("address", "");
        }
        return fields;
    }
    for (const auto& [key, value] : address) {
        if (update || !value->empty()) {
            fields.emplace_back(std::format("address[{}]", key), *value);
        }
    }
    return fields;
}

// Stripe may expand object references; tolerate both wire shapes without throwing on nulls.
std::string stripe_text(const json& object, std::string_view key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}
std::string stripe_id(const json& object, std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return {};
    }
    return it->is_object() ? stripe_text(*it, "id") : stripe_text(object, key);
}
std::int64_t stripe_epoch(const json& object, std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) {
        return 0;
    }
    if (*it < 0 || *it > 253402300799LL) {
        return 0;
    }
    return it->get<std::int64_t>();
}
const json& subscription_details(const json& object) {
    static const json empty = json::object();
    if (object.contains("parent") && object["parent"].is_object() &&
        object["parent"].contains("subscription_details") &&
        object["parent"]["subscription_details"].is_object()) {
        return object["parent"]["subscription_details"];
    }
    return object.contains("subscription_details") && object["subscription_details"].is_object()
               ? object["subscription_details"]
               : empty;
}
const json& stripe_metadata(const json& object) {
    static const json empty = json::object();
    return object.contains("metadata") && object["metadata"].is_object() ? object["metadata"]
                                                                         : empty;
}
// A subscription has one recurring item today. If there are several, the earliest expiry wins.
std::int64_t subscription_period_end(const json& object) {
    std::int64_t end = stripe_epoch(object, "current_period_end");
    // In the pinned 2024 API this field is on the subscription, not each item.
    if (end > 0) {
        return end;
    }
    if (object.contains("items") && object["items"].is_object()) {
        const auto& items = object["items"];
        if ((!items.contains("has_more") || items["has_more"] != false) ||
            !items.contains("data") || !items["data"].is_array()) {
            return 0;
        }
        for (const auto& item : items["data"]) {
            const auto item_end = stripe_epoch(item, "current_period_end");
            if (item_end == 0) {
                return 0;
            }
            end = end == 0 ? item_end : std::min(end, item_end);
        }
    }
    return end;
}
std::int64_t invoice_subscription_period_end(const json& object) {
    if (!object.contains("lines") || !object["lines"].is_object()) {
        return 0;
    }
    const auto& lines = object["lines"];
    if ((!lines.contains("has_more") || lines["has_more"] != false) || !lines.contains("data") ||
        !lines["data"].is_array()) {
        return 0;
    }
    std::int64_t end = 0;
    for (const auto& line : lines["data"]) {
        const bool recurring = stripe_text(line, "type") == "subscription" ||
                               (line.contains("parent") && line["parent"].is_object() &&
                                stripe_text(line["parent"], "type") == "subscription_item_details");
        if (!recurring) {
            continue;
        }
        if (!line.contains("period") || !line["period"].is_object()) {
            return 0;
        }
        const auto line_end = stripe_epoch(line["period"], "end");
        if (line_end == 0) {
            return 0;
        }
        end = end == 0 ? line_end : std::min(end, line_end);
    }
    return end;
}

// Whether an event's invoice or Checkout Session is another app's or another Lake's (lake#855).
// Lake's `owner` key marks it Lake's: lake_customer_id on an invoice Lake issued, lake_order_id on
// an order sheet session. Stripe does not copy a subscription's metadata into the invoices it
// generates, so an invoice without the key is read by its subscription_details, whose owner key is
// the order's. A lake_instance other than `instance` is another Lake's; an object tagged before
// lake_instance existed is this Lake's.
bool foreign_object(const json& object, std::string_view owner, std::string_view instance) {
    const auto metadata = [](const json& holder) {
        return holder.is_object() && holder.contains("metadata") && holder["metadata"].is_object()
                   ? holder["metadata"]
                   : json::object();
    };
    const auto text = [](const json& tags, std::string_view key) {
        const auto found = tags.find(key);
        return found != tags.end() && found->is_string() ? found->get<std::string>()
                                                         : std::string();
    };
    json tags = metadata(object);
    if (text(tags, owner).empty()) {
        tags = metadata(subscription_details(object));
        owner = "lake_order_id";
    }
    if (text(tags, owner).empty()) {
        return true;
    }
    const std::string lake_instance = text(tags, "lake_instance");
    return !lake_instance.empty() && lake_instance != instance;
}

class StripePaymentProvider final : public PaymentProvider {
public:
    StripePaymentProvider(StripeKeys keys, std::string instance, StripeHttp http, std::string base)
        : keys_(std::move(keys)), instance_(std::move(instance)), http_(std::move(http)),
          base_(std::move(base)) {}

    std::string_view name() const override {
        return "stripe";
    }
    std::string_view title() const override {
        return "Stripe";
    }
    bool records_payments_by_hand() const override {
        return false;
    }

    // The kept customer while Stripe still has it. One it no longer has (404 resource_missing, or
    // deleted: true) is replaced by a new customer under its own key, since the plain key would
    // replay the first create within Stripe's 24 hours in the same account (lake#856). Any other
    // failure is an error and never replaces it.
    std::expected<ProviderCustomer, std::string>
    provider_customer(const PaymentAccount& account) override {
        std::string key = "lake-customer-" + account.customer_id;
        if (!account.provider_customer_id.empty()) {
            auto kept = get("/v1/customers/" + percent_encode(account.provider_customer_id),
                            "read the customer");
            if (!kept) {
                return std::unexpected(kept.error());
            }
            const std::optional<json>& customer = *kept;
            if (customer && !(customer->contains("deleted") && customer->at("deleted") == true)) {
                return ProviderCustomer{account.provider_customer_id, {}};
            }
            key += "-replaces-" + account.provider_customer_id;
        }
        std::vector<std::pair<std::string, std::string>> fields = billing_details(account, false);
        fields.insert(fields.end(),
                      {{"description", "Lake account " + account.customer_id},
                       {"metadata[lake_customer_id]", account.customer_id}});
        tag(fields, "metadata", instance_);
        auto created = post("/v1/customers", fields, key, "create the customer");
        if (!created) {
            return std::unexpected(created.error());
        }
        const std::string id = created->value("id", "");
        if (!id.starts_with("cus_")) {
            return std::unexpected(std::string("Stripe answered a customer without an id"));
        }
        return ProviderCustomer{id, account.provider_customer_id};
    }

    // POST /v1/customers/<id> with the contact's email, name and address. Each save of the contact
    // is its own write, keyed by when it was saved.
    std::expected<void, std::string> update_customer(const PaymentAccount& account) override {
        if (account.provider_customer_id.empty() || !account.contact) {
            return std::unexpected(
                std::string("the account has no Stripe customer or billing contact"));
        }
        auto updated = post("/v1/customers/" + account.provider_customer_id,
                            billing_details(account, true),
                            std::format("lake-customer-{}-contact-{}",
                                        account.customer_id,
                                        account.contact->updated_at),
                            "update the customer");
        if (!updated) {
            return std::unexpected(updated.error());
        }
        return {};
    }

    std::expected<PaymentRequest, std::string>
    request_payment(const PaymentAccount& account, const control::Invoice& invoice) override {
        if (invoice.currency != "USD") {
            return std::unexpected("the Stripe provider bills USD only, not " + invoice.currency);
        }
        if (account.provider_customer_id.empty()) {
            return std::unexpected(std::string("the account has no Stripe customer"));
        }
        // One key family per issue of a month: a retry of the same issue (a lost answer, a
        // failed step) returns Stripe's first result instead of a second invoice.
        const std::string key = invoice_key(account, invoice);
        std::vector<std::pair<std::string, std::string>> fields{
            {"customer", account.provider_customer_id},
            {"currency", "usd"},
            {"collection_method", "send_invoice"},
            {"days_until_due", "30"},
            {"auto_advance", "false"},
            {"pending_invoice_items_behavior", "exclude"},
            {"description", std::format("Lake usage, {}", invoice.period)},
            {"metadata[lake_customer_id]", invoice.customer_id},
            {"metadata[lake_period]", invoice.period},
            {"metadata[lake_price_list_id]", invoice.plan_id}};
        tag(fields, "metadata", instance_);
        auto draft = post("/v1/invoices", fields, key, "create the invoice");
        if (!draft) {
            return std::unexpected(draft.error());
        }
        const std::string stripe_invoice = draft->value("id", "");
        if (!stripe_invoice.starts_with("in_")) {
            return std::unexpected(std::string("Stripe answered an invoice without an id"));
        }
        // An invoice item per lake line with an amount, in cents; then, when rounding each line
        // to the cent does not add up to lake's total to the cent, one line with the difference.
        // Lake's invoice is the source of truth for amounts. Each lake line's key ends in -p
        // since its fields changed (lake#855): Stripe refuses a key sent again with other fields.
        std::int64_t items = 0;
        for (std::size_t index = 0; index < invoice.lines.size(); ++index) {
            const control::InvoiceLine& line = invoice.lines[index];
            const std::int64_t cents = stripe_cents(line.amount_micros);
            if (cents == 0) {
                continue;
            }
            std::string description = line.description;
            if (line.kind == "usage") {
                description += std::format(" ({} {} billed)", line.billable, line.unit);
            }
            auto item = add_item(account.provider_customer_id,
                                 stripe_invoice,
                                 cents,
                                 description,
                                 std::format("{}-line-{}-p", key, index),
                                 "add an invoice line");
            if (!item) {
                return std::unexpected(item.error());
            }
            items += cents;
        }
        const std::int64_t total = stripe_cents(invoice.total_micros);
        if (total != items) {
            auto rounding = add_item(account.provider_customer_id,
                                     stripe_invoice,
                                     total - items,
                                     "Rounding to the cent",
                                     key + "-rounding",
                                     "add the rounding line");
            if (!rounding) {
                return std::unexpected(rounding.error());
            }
        }
        auto finalized = post("/v1/invoices/" + stripe_invoice + "/finalize",
                              {{"auto_advance", "false"}},
                              key + "-finalize",
                              "finalize the invoice");
        if (!finalized) {
            return std::unexpected(finalized.error());
        }
        PaymentRequest request;
        request.reference = stripe_invoice;
        request.payment_url = finalized->value("hosted_invoice_url", "");
        request.provider_status = finalized->value("status", "open");
        request.instructions =
            "Pay this invoice by card or bank transfer on Stripe's invoice page. "
            "Lake marks it paid when Stripe tells it.";
        return request;
    }

    // POST /v1/invoices/<id>/send: Stripe emails the hosted invoice link to the customer's email.
    // The send at issue is keyed in the invoice's family; each resend has its own key, since Stripe
    // answers a key's first answer again for 24 hours, a refusal included.
    std::expected<std::string, std::string> send_payment_request(const PaymentAccount& account,
                                                                 const control::Invoice& invoice,
                                                                 std::string_view resend) override {
        if (invoice.provider_reference.empty()) {
            return std::unexpected(std::string("the invoice has no Stripe invoice"));
        }
        std::string key = invoice_key(account, invoice) + "-send";
        if (!resend.empty()) {
            key += std::format("-{}", resend);
        }
        auto sent = post(
            "/v1/invoices/" + invoice.provider_reference + "/send", {}, key, "send the invoice");
        if (!sent) {
            return std::unexpected(sent.error());
        }
        return sent->value("status", invoice.provider_status);
    }

    std::expected<void, std::string> cancel_payment(const control::Invoice& invoice) override {
        if (invoice.provider_reference.empty()) {
            return {};
        }
        auto voided = post("/v1/invoices/" + invoice.provider_reference + "/void",
                           {},
                           "lake-void-" + invoice.id,
                           "void the invoice");
        if (!voided) {
            return std::unexpected(voided.error());
        }
        return {};
    }

    std::expected<CheckoutSession, std::string>
    create_checkout(const CheckoutRequest& order) override {
        const std::string_view product = stripe_offer_product(order.offer);
        if (order.order_id.empty() || product.empty() || order.unit_amount_cents <= 0 ||
            order.quantity < 1 || order.success_url.empty() || order.cancel_url.empty()) {
            return std::unexpected(
                std::string("a checkout needs an order, an offer, an amount and its URLs"));
        }
        // A subscription to the offer's Lake product with the price inline (price_data), so the
        // order sheet's configuration is the one source of prices; where it runs is the
        // subscription's description. Stripe creates the customer from the email.
        std::vector<std::pair<std::string, std::string>> fields{
            {"mode", "subscription"},
            {"client_reference_id", order.order_id},
            {"success_url", order.success_url},
            {"cancel_url", order.cancel_url},
            {"line_items[0][quantity]", std::to_string(order.quantity)},
            {"line_items[0][price_data][currency]", order.currency},
            {"line_items[0][price_data][product]", std::string(product)},
            {"line_items[0][price_data][unit_amount]", std::to_string(order.unit_amount_cents)},
            {"line_items[0][price_data][recurring][interval]", "month"},
            {"metadata[lake_order_id]", order.order_id},
            {"subscription_data[metadata][lake_order_id]", order.order_id}};
        tag(fields, "metadata", instance_);
        tag(fields, "subscription_data[metadata]", instance_);
        if (!order.description.empty()) {
            fields.emplace_back("subscription_data[description]", order.description);
        }
        if (!order.customer_email.empty()) {
            fields.emplace_back("customer_email", order.customer_email);
        }
        auto session = post("/v1/checkout/sessions",
                            fields,
                            "lake-order-" + order.order_id + "-checkout",
                            "start the checkout");
        if (!session) {
            return std::unexpected(session.error());
        }
        CheckoutSession result{session->value("id", ""), session->value("url", "")};
        if (!result.id.starts_with("cs_") || !result.url.starts_with("https://")) {
            return std::unexpected(std::string("Stripe answered a checkout without an id or URL"));
        }
        return result;
    }

    // A Checkout for the account's prepaid balance (lake#1114) on its Stripe customer: a top-up in
    // payment mode, one line of Lake's usage product (a top-up prepays usage) at the amount inline;
    // or saving a card for auto top-up in setup mode, with the SetupIntent tagged so its
    // setup_intent.succeeded names the account. Each attempt has its own key.
    std::expected<CheckoutSession, std::string>
    create_balance_checkout(const PaymentAccount& account,
                            const BalanceCheckoutRequest& request) override {
        const bool top_up = request.purpose == "top_up";
        if (account.provider_customer_id.empty() || account.customer_id.empty() ||
            (!top_up && request.purpose != "card_setup") || (top_up && request.amount_cents <= 0) ||
            request.success_url.empty() || request.cancel_url.empty() || request.attempt.empty()) {
            return std::unexpected(
                std::string("a balance checkout needs an account, an amount and its URLs"));
        }
        std::vector<std::pair<std::string, std::string>> fields{
            {"customer", account.provider_customer_id},
            {"client_reference_id", account.customer_id},
            {"success_url", request.success_url},
            {"cancel_url", request.cancel_url},
            {"metadata[lake_customer_id]", account.customer_id},
            {"metadata[lake_balance]", request.purpose}};
        if (top_up) {
            fields.insert(
                fields.end(),
                {{"mode", "payment"},
                 {"line_items[0][quantity]", "1"},
                 {"line_items[0][price_data][currency]", "usd"},
                 {"line_items[0][price_data][product]", std::string(stripe_usage_product)},
                 {"line_items[0][price_data][unit_amount]", std::to_string(request.amount_cents)},
                 {"payment_intent_data[description]", "Lake balance top-up"}});
        } else {
            fields.insert(fields.end(),
                          {{"mode", "setup"},
                           {"currency", "usd"},
                           {"payment_method_types[0]", "card"},
                           {"setup_intent_data[description]", "Lake auto top-up card"},
                           {"setup_intent_data[metadata][lake_customer_id]", account.customer_id},
                           {"setup_intent_data[metadata][lake_balance]", "card_setup"}});
            tag(fields, "setup_intent_data[metadata]", instance_);
        }
        tag(fields, "metadata", instance_);
        auto session = post("/v1/checkout/sessions",
                            fields,
                            std::format("lake-{}-{}-{}",
                                        top_up ? "top-up" : "card-setup",
                                        account.customer_id,
                                        request.attempt),
                            top_up ? "start the top-up" : "start saving the card");
        if (!session) {
            return std::unexpected(session.error());
        }
        CheckoutSession result{session->value("id", ""), session->value("url", "")};
        if (!result.id.starts_with("cs_") || !result.url.starts_with("https://")) {
            return std::unexpected(std::string("Stripe answered a checkout without an id or URL"));
        }
        return result;
    }

    // POST /v1/payment_intents with the saved card, off_session and confirm: an auto top-up with
    // nobody present. A decline (Stripe's 402 card_error) or a charge that needs the person to
    // authenticate is an error; the credit comes from payment_intent.succeeded.
    std::expected<OffSessionResult, std::string>
    charge_saved_payment_method(const PaymentAccount& account,
                                const OffSessionCharge& charge) override {
        if (account.provider_customer_id.empty() || charge.payment_method.empty() ||
            charge.amount_cents <= 0 || charge.idempotency_key.empty()) {
            return std::unexpected(
                std::string("an auto top-up needs a customer, a card and an amount"));
        }
        std::vector<std::pair<std::string, std::string>> fields{
            {"amount", std::to_string(charge.amount_cents)},
            {"currency", "usd"},
            {"customer", account.provider_customer_id},
            {"payment_method", charge.payment_method},
            {"off_session", "true"},
            {"confirm", "true"},
            {"description", "Lake balance auto top-up"},
            {"metadata[lake_customer_id]", account.customer_id},
            {"metadata[lake_balance]", "auto_top_up"}};
        tag(fields, "metadata", instance_);
        auto intent =
            post("/v1/payment_intents", fields, charge.idempotency_key, "charge the saved card");
        if (!intent) {
            return std::unexpected(intent.error());
        }
        OffSessionResult result{intent->value("id", ""), intent->value("status", "")};
        if (result.status != "succeeded" && result.status != "processing") {
            return std::unexpected(
                std::format("the saved card needs you to confirm the payment (Stripe status {})",
                            result.status.empty() ? std::string("unknown") : result.status));
        }
        return result;
    }

    std::expected<PaymentNotice, NoticeRejection>
    read_notice(std::string_view body,
                std::string_view signature,
                std::chrono::system_clock::time_point now) override {
        auto verified = verify_stripe_signature(body, signature, keys_.webhook_secret, now);
        if (!verified) {
            return std::unexpected(verified.error());
        }
        return parse_stripe_event(body, instance_);
    }

private:
    // The idempotency key family of one issue of an account's month.
    static std::string invoice_key(const PaymentAccount& account, const control::Invoice& invoice) {
        return std::format(
            "lake-invoice-{}-{}-{}", invoice.customer_id, invoice.period, account.attempt);
    }

    // One invoice item of `cents` on a draft invoice, tagged as this Lake's. A positive amount is
    // Lake's usage product at an inline price; a negative one (a rounding difference) keeps the
    // bare amount, because an inline price cannot be negative.
    std::expected<json, std::string> add_item(const std::string& customer,
                                              const std::string& stripe_invoice,
                                              std::int64_t cents,
                                              const std::string& description,
                                              const std::string& idempotency_key,
                                              std::string_view what) {
        std::vector<std::pair<std::string, std::string>> fields{{"customer", customer},
                                                                {"invoice", stripe_invoice}};
        if (cents > 0) {
            fields.insert(fields.end(),
                          {{"quantity", "1"},
                           {"price_data[currency]", "usd"},
                           {"price_data[product]", std::string(stripe_usage_product)},
                           {"price_data[unit_amount]", std::to_string(cents)}});
        } else {
            fields.insert(fields.end(), {{"currency", "usd"}, {"amount", std::to_string(cents)}});
        }
        fields.emplace_back("description", description);
        tag(fields, "metadata", instance_);
        return post("/v1/invoiceitems", fields, idempotency_key, what);
    }

    std::expected<json, std::string>
    post(const std::string& path,
         const std::vector<std::pair<std::string, std::string>>& fields,
         const std::string& idempotency_key,
         std::string_view what) {
        StripeRequest request;
        request.method = "POST";
        request.url = base_ + path;
        request.body = stripe_form(fields);
        request.headers = {{"Authorization", "Bearer " + keys_.secret_key},
                           {"Content-Type", "application/x-www-form-urlencoded"},
                           {"Stripe-Version", std::string(stripe_api_version)},
                           {"Idempotency-Key", idempotency_key}};
        const StripeResponse response = http_(request);
        if (!response.error.empty() || response.status < 200 || response.status >= 300) {
            if (const std::string product = missing_product(response, fields); !product.empty()) {
                // Stripe may save this answer under the key and replay it for 24 hours.
                return std::unexpected(std::format(
                    "Stripe has no product {} in this account; the operator creates it "
                    "(spec.payments). Try again once it exists, and again after 24 hours if "
                    "Stripe repeats this answer",
                    product));
            }
            return std::unexpected(stripe_failure(what, response));
        }
        return answer_object(response, what);
    }

    // Stripe's object at `path` (GET with the secret key and Stripe-Version a write sends; a read
    // takes no Idempotency-Key), or nullopt when Stripe has no such object in this account and
    // mode. Any other failure is an error, Stripe's message dropped on 401 and 403 as for a write.
    std::expected<std::optional<json>, std::string> get(const std::string& path,
                                                        std::string_view what) {
        StripeRequest request;
        request.method = "GET";
        request.url = base_ + path;
        request.headers = {{"Authorization", "Bearer " + keys_.secret_key},
                           {"Stripe-Version", std::string(stripe_api_version)}};
        const StripeResponse response = http_(request);
        if (missing_object(response)) {
            return std::nullopt;
        }
        if (!response.error.empty() || response.status < 200 || response.status >= 300) {
            return std::unexpected(stripe_failure(what, response));
        }
        return answer_object(response, what);
    }

    StripeKeys keys_;
    std::string instance_;
    StripeHttp http_;
    std::string base_;
};

} // namespace

std::string stripe_form(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [name, value] : fields) {
        if (!out.empty()) {
            out.push_back('&');
        }
        out += percent_encode(name);
        out.push_back('=');
        out += percent_encode(value);
    }
    return out;
}

std::int64_t stripe_cents(std::uint64_t micros) {
    const std::uint64_t cents = micros / 10'000 + (micros % 10'000 >= 5'000 ? 1 : 0);
    return static_cast<std::int64_t>(
        std::min<std::uint64_t>(cents, std::numeric_limits<std::int64_t>::max()));
}

std::string_view stripe_offer_product(std::string_view offer) {
    if (offer == "private_warehouse") {
        return stripe_private_warehouse_product;
    }
    if (offer == "dedicated_cluster") {
        return stripe_dedicated_cluster_product;
    }
    return {};
}

std::expected<void, NoticeRejection>
verify_stripe_signature(std::string_view body,
                        std::string_view header,
                        std::string_view webhook_secret,
                        std::chrono::system_clock::time_point now,
                        std::chrono::seconds tolerance) {
    std::string_view timestamp;
    std::vector<std::string_view> signatures;
    while (!header.empty()) {
        const std::size_t comma = header.find(',');
        const std::string_view item = trim(header.substr(0, comma));
        header = comma == std::string_view::npos ? std::string_view() : header.substr(comma + 1);
        const std::size_t equals = item.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = item.substr(0, equals);
        const std::string_view value = item.substr(equals + 1);
        if (key == "t") {
            timestamp = value;
        } else if (key == "v1") {
            signatures.push_back(value);
        }
    }
    std::int64_t seconds = 0;
    const auto [end, parsed] =
        std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), seconds);
    if (timestamp.empty() || parsed != std::errc{} || end != timestamp.data() + timestamp.size() ||
        signatures.empty()) {
        return std::unexpected(NoticeRejection{
            "invalid_signature", "the Stripe-Signature header has no timestamp or v1 signature"});
    }
    if (webhook_secret.empty()) {
        return std::unexpected(
            NoticeRejection{"invalid_signature", "Lake has no webhook secret to check it with"});
    }
    std::string payload(timestamp);
    payload.push_back('.');
    payload.append(body);
    const std::string expected = hmac_sha256_hex(webhook_secret, payload);
    bool matched = false;
    for (const std::string_view signature : signatures) {
        if (signature.size() == expected.size() &&
            CRYPTO_memcmp(signature.data(), expected.data(), expected.size()) == 0) {
            matched = true;
        }
    }
    if (!matched) {
        return std::unexpected(
            NoticeRejection{"invalid_signature", "no v1 signature matches the webhook secret"});
    }
    const std::int64_t at =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::int64_t skew = at > seconds ? at - seconds : seconds - at;
    if (skew > tolerance.count()) {
        return std::unexpected(NoticeRejection{
            "stale_timestamp",
            std::format("the signature's timestamp is {} s from Lake's clock (at most {} s)",
                        skew,
                        tolerance.count())});
    }
    return {};
}

std::string stripe_signature_header(std::string_view body,
                                    std::string_view webhook_secret,
                                    std::int64_t timestamp) {
    const std::string payload = std::to_string(timestamp) + "." + std::string(body);
    return std::format("t={},v1={}", timestamp, hmac_sha256_hex(webhook_secret, payload));
}

std::expected<PaymentNotice, NoticeRejection> parse_stripe_event(std::string_view body,
                                                                 std::string_view instance) {
    const json event = json::parse(body, nullptr, false);
    if (!event.is_object() || !event.contains("id") || !event["id"].is_string() ||
        !event.contains("type") || !event["type"].is_string()) {
        return std::unexpected(NoticeRejection{"invalid_input", "the body is not a Stripe event"});
    }
    PaymentNotice notice;
    notice.event_id = event["id"].get<std::string>();
    notice.event_type = event["type"].get<std::string>();
    if (notice.event_id.empty() || notice.event_id.size() > 255) {
        return std::unexpected(NoticeRejection{"invalid_input", "the event has no usable id"});
    }
    const json no_object = json::object();
    const json& object = event.contains("data") && event["data"].is_object() &&
                                 event["data"].contains("object") &&
                                 event["data"]["object"].is_object()
                             ? event["data"]["object"]
                             : no_object;
    notice.reference = stripe_text(object, "id");
    // The prepaid balance (lake#1114): a top-up Checkout, a card saved for auto top-up, and an
    // auto top-up's PaymentIntent, each tagged lake_customer_id and lake_balance.
    const std::string balance_purpose = stripe_text(stripe_metadata(object), "lake_balance");
    const bool balance_event =
        (notice.event_type.starts_with("checkout.session.") && !balance_purpose.empty()) ||
        notice.event_type == "setup_intent.succeeded" ||
        notice.event_type == "payment_intent.succeeded" ||
        notice.event_type == "payment_intent.payment_failed";
    if (balance_event) {
        notice.foreign =
            foreign_object(object, "lake_customer_id", instance) || balance_purpose.empty();
        if (notice.foreign) {
            return notice;
        }
        PaymentNotice::Balance about;
        about.customer_id = stripe_text(stripe_metadata(object), "lake_customer_id");
        about.reference = notice.reference;
        const auto cents = [&](const char* key) -> std::int64_t {
            const auto found = object.find(key);
            return found != object.end() && found->is_number_integer() && *found > 0
                       ? found->get<std::int64_t>()
                       : 0;
        };
        if (notice.event_type.starts_with("checkout.session.")) {
            const std::string payment_status = stripe_text(object, "payment_status");
            const bool paid =
                (notice.event_type == "checkout.session.completed" && payment_status == "paid") ||
                notice.event_type == "checkout.session.async_payment_succeeded";
            if (balance_purpose != "top_up" || !paid) {
                return notice; // a card setup session, or a payment not in yet: nothing to do
            }
            about.kind = "credit";
            about.credit_kind = "top_up";
            about.amount_cents = cents("amount_total");
        } else if (notice.event_type == "setup_intent.succeeded") {
            if (balance_purpose != "card_setup") {
                return notice;
            }
            about.kind = "card_saved";
            about.payment_method = stripe_id(object, "payment_method");
        } else if (balance_purpose != "auto_top_up") {
            return notice;
        } else if (notice.event_type == "payment_intent.succeeded") {
            about.kind = "credit";
            about.credit_kind = "auto_top_up";
            about.amount_cents =
                cents("amount_received") > 0 ? cents("amount_received") : cents("amount");
        } else {
            about.kind = "auto_top_up_failed";
            const auto error = object.find("last_payment_error");
            about.message = error != object.end() && error->is_object()
                                ? stripe_text(*error, "message")
                                : std::string();
            if (about.message.empty()) {
                about.message = "the card was declined";
            }
        }
        notice.balance = std::move(about);
        return notice;
    }
    if (notice.event_type.starts_with("checkout.session.")) {
        // The apex order sheet's Checkout (lake#831): the session is the reference, and its
        // client_reference_id is lake's order id. Paid once Stripe says the payment is in; a
        // session that expired or whose delayed payment failed cancels the order, which is never
        // provisioned.
        notice.foreign = foreign_object(object, "lake_order_id", instance);
        const auto text = [&](const char* key) {
            return object.contains(key) && object[key].is_string() ? object[key].get<std::string>()
                                                                   : std::string();
        };
        notice.checkout_session = text("id");
        notice.order_id = text("client_reference_id");
        notice.customer = stripe_id(object, "customer");
        notice.subscription = stripe_id(object, "subscription");
        const std::string payment_status = text("payment_status");
        if (notice.event_type == "checkout.session.completed") {
            if (payment_status == "paid" || payment_status == "no_payment_required") {
                notice.order_state = "paid";
            }
        } else if (notice.event_type == "checkout.session.async_payment_succeeded") {
            notice.order_state = "paid";
        } else if (notice.event_type == "checkout.session.async_payment_failed") {
            notice.order_state = "cancelled";
            notice.order_detail = "the payment failed";
        } else if (notice.event_type == "checkout.session.expired") {
            notice.order_state = "cancelled";
            notice.order_detail = "the checkout expired without payment";
        }
        return notice;
    }
    const bool subscription_event = notice.event_type == "customer.subscription.created" ||
                                    notice.event_type == "customer.subscription.updated" ||
                                    notice.event_type == "customer.subscription.deleted" ||
                                    notice.event_type == "customer.subscription.paused" ||
                                    notice.event_type == "customer.subscription.resumed";
    const bool invoice_event = notice.event_type == "invoice.paid" ||
                               notice.event_type == "invoice.payment_failed" ||
                               notice.event_type == "invoice.voided" ||
                               notice.event_type == "invoice.marked_uncollectible" ||
                               notice.event_type == "invoice.payment_action_required" ||
                               notice.event_type == "invoice.updated";
    if (subscription_event ||
        (invoice_event &&
         !stripe_text(stripe_metadata(subscription_details(object)), "lake_order_id").empty())) {
        const json& holder = subscription_event ? object : subscription_details(object);
        notice.foreign = foreign_object(holder, "lake_order_id", instance);
        if (notice.foreign) {
            return notice;
        }
        // An invoice marked paid without a payment never activates an order (lake#1033).
        if (invoice_event && object.value("paid_out_of_band", false)) {
            notice.provider_status = "paid_out_of_band";
            return notice;
        }
        control::CloudOrderSubscriptionEvent update;
        update.provider = "stripe";
        update.event_id = notice.event_id;
        update.event_type = notice.event_type;
        update.order_id = stripe_text(stripe_metadata(holder), "lake_order_id");
        update.customer_id = stripe_id(object, "customer");
        update.subscription_id =
            subscription_event ? notice.reference : stripe_id(holder, "subscription");
        if (update.subscription_id.empty() && invoice_event) {
            update.subscription_id = stripe_id(object, "subscription");
        }
        update.created = stripe_epoch(event, "created");
        if (subscription_event) {
            update.status = notice.event_type == "customer.subscription.deleted" ? "canceled"
                            : notice.event_type == "customer.subscription.paused"
                                ? "paused"
                                : stripe_text(object, "status");
            update.period_end = update.status == "trialing" ? stripe_epoch(object, "trial_end")
                                                            : subscription_period_end(object);
            update.ended_at = stripe_epoch(object, "ended_at");
            const auto cancel_at = stripe_epoch(object, "cancel_at");
            if (cancel_at > 0 && update.period_end > 0) {
                update.period_end = std::min(update.period_end, cancel_at);
            }
        } else {
            update.invoice_id = notice.reference;
            update.status = notice.event_type == "invoice.paid" ? "active"
                            : (notice.event_type == "invoice.payment_failed" ||
                               notice.event_type == "invoice.payment_action_required")
                                ? "past_due"
                                : "unpaid";
            if (notice.event_type == "invoice.updated") {
                const auto status = stripe_text(object, "status");
                if (status == "paid") {
                    update.status = "active";
                } else if (status == "void" || status == "uncollectible") {
                    update.status = "unpaid";
                } else if (status == "open" && object.contains("attempted") &&
                           object["attempted"] == true) {
                    update.status = "past_due";
                } else {
                    return notice;
                }
            }
            update.period_end = invoice_subscription_period_end(object);
            update.next_payment_attempt = stripe_epoch(object, "next_payment_attempt");
        }
        if (!control::valid_cloud_order_subscription_event(update) ||
            ((update.status == "active" || update.status == "trialing") &&
             update.period_end == 0)) {
            return std::unexpected(
                NoticeRejection{"invalid_input", "the order subscription event is incomplete"});
        }
        notice.order_subscription = std::move(update);
        return notice;
    }
    if (notice.event_type.starts_with("invoice.")) {
        notice.foreign = foreign_object(object, "lake_customer_id", instance);
    }
    if (notice.event_type == "invoice.paid" && object.value("paid_out_of_band", false)) {
        // Marked paid in Stripe without a payment (LSR-140, lake#1033): anything else holding a
        // key for this account can do that, so the invoice stays due and the notice is recorded
        // for an operator to review.
        notice.provider_status = "paid_out_of_band";
    } else if (notice.event_type == "invoice.paid") {
        notice.state = "paid";
        notice.provider_status = "paid";
    } else if (notice.event_type == "invoice.voided") {
        notice.state = "void";
        notice.provider_status = "void";
    } else if (notice.event_type == "invoice.payment_failed") {
        notice.provider_status = "payment_failed";
    }
    return notice;
}

bool stripe_secret_key_shape(std::string_view key) {
    return (key.starts_with("sk_") || key.starts_with("rk_")) && key.size() >= 16 &&
           key.size() <= 255 &&
           std::ranges::all_of(key, [](char c) { return c > ' ' && c < 0x7F; });
}

bool stripe_webhook_secret_shape(std::string_view secret) {
    return secret.starts_with("whsec_") && secret.size() >= 16 && secret.size() <= 255 &&
           std::ranges::all_of(secret, [](char c) { return c > ' ' && c < 0x7F; });
}

std::shared_ptr<PaymentProvider>
stripe_payment_provider(StripeKeys keys, std::string instance, StripeHttp http, std::string base) {
    return std::make_shared<StripePaymentProvider>(
        std::move(keys), std::move(instance), std::move(http), std::move(base));
}

} // namespace lake::api
