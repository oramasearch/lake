// Stripe, lake's payment provider (decision 57, lake#620, docs/api/billing.md#stripe): lake's own
// client for Stripe's REST API and the check of its webhook signatures. No Stripe SDK ("We write
// our own"); the HTTP exchange is injected (StripeHttp), the server's outbound client in lake.

#include "lake/api/stripe.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>

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
        return std::format("Stripe refused lake's key to {} (HTTP {}); check LAKE_STRIPE_SECRET_KEY",
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

class StripePaymentProvider final : public PaymentProvider {
public:
    StripePaymentProvider(StripeKeys keys, StripeHttp http, std::string base)
        : keys_(std::move(keys)), http_(std::move(http)), base_(std::move(base)) {}

    std::string_view name() const override {
        return "stripe";
    }
    std::string_view title() const override {
        return "Stripe";
    }
    bool records_payments_by_hand() const override {
        return false;
    }

    std::expected<std::string, std::string>
    provider_customer(const PaymentAccount& account) override {
        if (!account.provider_customer_id.empty()) {
            return account.provider_customer_id;
        }
        auto created = post("/v1/customers",
                            {{"name", account.name.empty() ? account.customer_id : account.name},
                             {"description", "lake account " + account.customer_id},
                             {"metadata[lake_customer_id]", account.customer_id}},
                            "lake-customer-" + account.customer_id,
                            "create the customer");
        if (!created) {
            return std::unexpected(created.error());
        }
        const std::string id = created->value("id", "");
        if (!id.starts_with("cus_")) {
            return std::unexpected(std::string("Stripe answered a customer without an id"));
        }
        return id;
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
        const std::string key = std::format(
            "lake-invoice-{}-{}-{}", invoice.customer_id, invoice.period, account.attempt);
        auto draft = post("/v1/invoices",
                          {{"customer", account.provider_customer_id},
                           {"currency", "usd"},
                           {"collection_method", "send_invoice"},
                           {"days_until_due", "30"},
                           {"auto_advance", "false"},
                           {"pending_invoice_items_behavior", "exclude"},
                           {"description", std::format("Orama Cloud usage, {}", invoice.period)},
                           {"metadata[lake_customer_id]", invoice.customer_id},
                           {"metadata[lake_period]", invoice.period},
                           {"metadata[lake_price_list_id]", invoice.plan_id}},
                          key,
                          "create the invoice");
        if (!draft) {
            return std::unexpected(draft.error());
        }
        const std::string stripe_invoice = draft->value("id", "");
        if (!stripe_invoice.starts_with("in_")) {
            return std::unexpected(std::string("Stripe answered an invoice without an id"));
        }
        // An invoice item per lake line with an amount, in cents; then, when rounding each line
        // to the cent does not add up to lake's total to the cent, one line with the difference.
        // Lake's invoice is the source of truth for amounts.
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
            auto item = post("/v1/invoiceitems",
                             {{"customer", account.provider_customer_id},
                              {"invoice", stripe_invoice},
                              {"currency", "usd"},
                              {"amount", std::to_string(cents)},
                              {"description", description}},
                             std::format("{}-line-{}", key, index),
                             "add an invoice line");
            if (!item) {
                return std::unexpected(item.error());
            }
            items += cents;
        }
        const std::int64_t total = stripe_cents(invoice.total_micros);
        if (total != items) {
            auto rounding = post("/v1/invoiceitems",
                                 {{"customer", account.provider_customer_id},
                                  {"invoice", stripe_invoice},
                                  {"currency", "usd"},
                                  {"amount", std::to_string(total - items)},
                                  {"description", "Rounding to the cent"}},
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

    std::expected<PaymentNotice, NoticeRejection>
    read_notice(std::string_view body,
                std::string_view signature,
                std::chrono::system_clock::time_point now) override {
        auto verified = verify_stripe_signature(body, signature, keys_.webhook_secret, now);
        if (!verified) {
            return std::unexpected(verified.error());
        }
        return parse_stripe_event(body);
    }

private:
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
            return std::unexpected(stripe_failure(what, response));
        }
        json body = json::parse(response.body, nullptr, false);
        if (!body.is_object()) {
            return std::unexpected(std::format("Stripe answered {} with no JSON object", what));
        }
        return body;
    }

    StripeKeys keys_;
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
            NoticeRejection{"invalid_signature", "lake has no webhook secret to check it with"});
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
            std::format("the signature's timestamp is {} s from lake's clock (at most {} s)",
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

std::expected<PaymentNotice, NoticeRejection> parse_stripe_event(std::string_view body) {
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
    if (const json* object = event.contains("data") && event["data"].is_object() &&
                                     event["data"].contains("object") &&
                                     event["data"]["object"].is_object()
                                 ? &event["data"]["object"]
                                 : nullptr) {
        notice.reference = object->value("id", "");
    }
    if (notice.event_type == "invoice.paid") {
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
stripe_payment_provider(StripeKeys keys, StripeHttp http, std::string base) {
    return std::make_shared<StripePaymentProvider>(
        std::move(keys), std::move(http), std::move(base));
}

} // namespace lake::api
