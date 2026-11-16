// The apex order sheet (docs/api/orders.md): its configuration, the order form's checks,
// the status link, the emails and the abuse limit. Pure apart from OrderRateLimiter's clock-driven
// state.

#include "lake/api/orders.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <set>

#include <nlohmann/json.hpp>

#include "lake/api/brand.hpp"
#include "lake/api/tunnel_clients.hpp"
#include "lake/api/usage_recorder.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

// The default order sheet: off, with no offers, no providers and no trust claims. An instance that
// sells through the apex sets every offer, price, credit, provider and region in LAKE_ORDER_SHEET
// (docs/api/orders.md#configuration); lake ships none of its own.
constexpr std::string_view default_sheet = R"json({
  "enabled": false,
  "url": "",
  "currency": "USD",
  "shared": {
    "title": "Shared",
    "summary": "Your own workspaces on this Lake's shared regions. Pay only for what you use, at the published price list.",
    "features": [],
    "price_list_url": "",
    "promise": "",
    "show_promise": false
  },
  "offers": {},
  "providers": [],
  "trust": [],
  "limits": {"orders_per_client_per_hour": 5, "orders_per_hour": 60,
             "orders_per_email_per_day": 3, "orders_per_domain_per_hour": 20}
})json";

constexpr std::array offer_ids{std::string_view{"private_warehouse"},
                               std::string_view{"dedicated_cluster"},
                               std::string_view{"on_prem"}};

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

// One line of what a person typed: trimmed, with line breaks and other control characters made
// spaces, so it cannot break an email subject or a page.
std::string single_line(std::string_view text) {
    std::string out(trim(text));
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            c = ' ';
        }
    }
    return out;
}

// Free text: trimmed, line breaks kept as \n, other control characters dropped.
std::string multi_line(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : trim(text)) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || c == '\t' || (byte >= 0x20 && byte != 0x7f)) {
            out += c;
        }
    }
    return out;
}

// Length in characters (UTF-8 code points), as the schema's length() counts.
std::size_t characters(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    });
}

std::expected<std::string, std::string>
text_at(const json& object, std::string_view key, std::string_view field, bool required = true) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || found->is_null()) {
        if (required) {
            return std::unexpected(std::format("{} must be a non-empty string", field));
        }
        return std::string();
    }
    if (!found->is_string() || (required && found->get<std::string>().empty())) {
        return std::unexpected(std::format("{} must be a non-empty string", field));
    }
    return found->get<std::string>();
}

std::expected<std::vector<std::string>, std::string>
texts_at(const json& object, std::string_view key, std::string_view field) {
    std::vector<std::string> out;
    const auto found = object.find(std::string(key));
    if (found == object.end() || found->is_null()) {
        return out;
    }
    if (!found->is_array()) {
        return std::unexpected(std::format("{} must be an array of strings", field));
    }
    for (const json& item : *found) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            return std::unexpected(std::format("{} must be an array of strings", field));
        }
        out.push_back(item.get<std::string>());
    }
    return out;
}

bool boolean_at(const json& object, std::string_view key, bool fallback) {
    const auto found = object.find(std::string(key));
    return found != object.end() && found->is_boolean() ? found->get<bool>() : fallback;
}

std::expected<std::int64_t, std::string> whole_at(const json& object,
                                                  std::string_view key,
                                                  std::string_view field,
                                                  std::int64_t low,
                                                  std::int64_t high) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->is_number_integer()) {
        return std::unexpected(std::format("{} must be a whole number", field));
    }
    const std::int64_t value = found->get<std::int64_t>();
    if (value < low || value > high) {
        return std::unexpected(std::format("{} must be between {} and {}", field, low, high));
    }
    return value;
}

bool https_url(std::string_view url) {
    return url.starts_with("https://") && url.size() > 8 && url.size() <= 2048 &&
           std::ranges::none_of(url,
                                [](unsigned char c) { return c <= ' ' || c == '"' || c == 0x7f; });
}

bool claim_url(std::string_view url) {
    return https_url(url) || (url.starts_with("mailto:") && url.size() > 7 && url.size() <= 2048 &&
                              std::ranges::none_of(url, [](unsigned char c) {
                                  return c <= ' ' || c == '"' || c == 0x7f;
                              }));
}

std::string thousands(std::int64_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (digits.size() - index) % 3 == 0) {
            out += ',';
        }
        out += digits[index];
    }
    return out;
}

const OrderRegion* find_region(const std::vector<OrderRegion>& regions, std::string_view id) {
    const auto found = std::ranges::find(regions, id, &OrderRegion::id);
    return found == regions.end() ? nullptr : &*found;
}

} // namespace

const OrderOffer* OrderSheet::find_offer(std::string_view id) const {
    const auto found = std::ranges::find(offers, id, &OrderOffer::id);
    return found == offers.end() ? nullptr : &*found;
}

const OrderProvider* OrderSheet::find_provider(std::string_view id) const {
    const auto found = std::ranges::find(providers, id, &OrderProvider::id);
    return found == providers.end() ? nullptr : &*found;
}

std::string_view default_order_sheet_json() {
    return default_sheet;
}

std::expected<OrderSheet, std::string> order_sheet_from_environment(std::string_view overrides) {
    json merged = json::parse(default_sheet);
    if (!trim(overrides).empty()) {
        const json patch = json::parse(overrides, nullptr, false);
        if (patch.is_discarded()) {
            return std::unexpected(std::string("LAKE_ORDER_SHEET is not valid JSON"));
        }
        if (!patch.is_object()) {
            return std::unexpected(
                std::string("LAKE_ORDER_SHEET must be a JSON object merged over the default"));
        }
        merged.merge_patch(patch);
    }
    OrderSheet sheet;
    const std::string at = "LAKE_ORDER_SHEET";
    sheet.enabled = boolean_at(merged, "enabled", true);
    auto url = text_at(merged, "url", at + ".url", false);
    if (!url) {
        return std::unexpected(url.error());
    }
    sheet.url = *url;
    if (!sheet.url.empty() && !https_url(sheet.url)) {
        return std::unexpected(at + ".url must be an https URL such as https://lake.example.com/");
    }
    auto currency = text_at(merged, "currency", at + ".currency");
    if (!currency) {
        return std::unexpected(currency.error());
    }
    if (*currency != "USD") {
        return std::unexpected(at + ".currency must be USD: the price list and Stripe bill USD");
    }
    const json shared = merged.value("shared", json::object());
    if (!shared.is_object()) {
        return std::unexpected(at + ".shared must be an object");
    }
    for (auto [key, target] : {std::pair{"title", &sheet.shared_title},
                               std::pair{"summary", &sheet.shared_summary},
                               std::pair{"promise", &sheet.promise}}) {
        auto value = text_at(shared, key, at + ".shared." + key, std::string_view(key) != "promise");
        if (!value) {
            return std::unexpected(value.error());
        }
        *target = std::move(*value);
    }
    auto shared_features = texts_at(shared, "features", at + ".shared.features");
    if (!shared_features) {
        return std::unexpected(shared_features.error());
    }
    sheet.shared_features = std::move(*shared_features);
    auto price_list = text_at(shared, "price_list_url", at + ".shared.price_list_url", false);
    if (!price_list) {
        return std::unexpected(price_list.error());
    }
    sheet.price_list_url = *price_list;
    if (!sheet.price_list_url.empty() && !https_url(sheet.price_list_url)) {
        return std::unexpected(at + ".shared.price_list_url must be an https URL");
    }
    sheet.show_promise = boolean_at(shared, "show_promise", false);
    if (sheet.show_promise && sheet.promise.empty()) {
        return std::unexpected(at + ".shared.promise must be a non-empty string to be shown");
    }

    const json offers = merged.value("offers", json::object());
    if (!offers.is_object()) {
        return std::unexpected(at + ".offers must be an object keyed by offer");
    }
    for (const auto& [key, value] : offers.items()) {
        if (std::ranges::find(offer_ids, key) == offer_ids.end()) {
            return std::unexpected(std::format(
                "{}.offers.{} is not an offer: private_warehouse, dedicated_cluster or on_prem",
                at,
                key));
        }
        static_cast<void>(value);
    }
    for (const std::string_view id : offer_ids) {
        const std::string field = at + ".offers." + std::string(id);
        const auto found = offers.find(std::string(id));
        if (found == offers.end() || found->is_null()) {
            continue; // an offer this Lake does not sell
        }
        if (!found->is_object()) {
            return std::unexpected(field + " must be an object");
        }
        OrderOffer offer;
        offer.id = std::string(id);
        for (auto [key, target] :
             {std::pair{"title", &offer.title}, std::pair{"summary", &offer.summary}}) {
            auto text = text_at(*found, key, field + "." + key);
            if (!text) {
                return std::unexpected(text.error());
            }
            *target = std::move(*text);
        }
        auto features = texts_at(*found, "features", field + ".features");
        if (!features) {
            return std::unexpected(features.error());
        }
        offer.features = std::move(*features);
        auto dollars = whole_at(*found, "monthly_usd", field + ".monthly_usd", 1, 100'000'000);
        if (!dollars) {
            return std::unexpected(dollars.error());
        }
        offer.monthly_cents = *dollars * 100;
        auto credit = whole_at(*found, "credit_percent", field + ".credit_percent", 0, 100);
        if (!credit) {
            return std::unexpected(credit.error());
        }
        offer.credit_percent = static_cast<int>(*credit);
        offer.starting_from = boolean_at(*found, "starting_from", false);
        offer.checkout = boolean_at(*found, "checkout", false);
        if (offer.checkout && id == "on_prem") {
            return std::unexpected(field + ".checkout must be false: on-prem is ordered by form");
        }
        sheet.offers.push_back(std::move(offer));
    }

    const json providers = merged.value("providers", json::array());
    if (!providers.is_array()) {
        return std::unexpected(at + ".providers must be an array");
    }
    std::set<std::string, std::less<>> provider_ids;
    for (std::size_t index = 0; index < providers.size(); ++index) {
        const std::string field = std::format("{}.providers[{}]", at, index);
        const json& entry = providers[index];
        if (!entry.is_object()) {
            return std::unexpected(field + " must be an object");
        }
        OrderProvider provider;
        for (auto [key, target] :
             {std::pair{"id", &provider.id}, std::pair{"label", &provider.label}}) {
            auto text = text_at(entry, key, field + "." + key);
            if (!text) {
                return std::unexpected(text.error());
            }
            *target = std::move(*text);
        }
        if (!valid_id(provider.id) || provider.id == "other") {
            return std::unexpected(
                std::format("{}.id must be letters, digits, hyphens and underscores, not \"{}\"",
                            field,
                            provider.id));
        }
        if (!provider_ids.insert(provider.id).second) {
            return std::unexpected(
                std::format("{}.providers names the provider \"{}\" twice", at, provider.id));
        }
        provider.checkout = boolean_at(entry, "checkout", false);
        const json regions = entry.value("regions", json::array());
        if (!regions.is_array() || regions.empty()) {
            return std::unexpected(field + ".regions must be a non-empty array");
        }
        std::set<std::string, std::less<>> region_ids;
        for (std::size_t r = 0; r < regions.size(); ++r) {
            const std::string region_field = std::format("{}.regions[{}]", field, r);
            if (!regions[r].is_object()) {
                return std::unexpected(region_field + " must be an object");
            }
            OrderRegion region;
            for (auto [key, target] :
                 {std::pair{"id", &region.id}, std::pair{"label", &region.label}}) {
                auto text = text_at(regions[r], key, region_field + "." + key);
                if (!text) {
                    return std::unexpected(text.error());
                }
                *target = std::move(*text);
            }
            if (!valid_id(region.id) || region.id == "other") {
                return std::unexpected(std::format(
                    "{}.id must be letters, digits, hyphens and underscores, not \"{}\"",
                    region_field,
                    region.id));
            }
            region.checkout = boolean_at(regions[r], "checkout", provider.checkout);
            if (region.checkout && !provider.checkout) {
                return std::unexpected(std::format(
                    "{}.checkout cannot be true on a provider whose checkout is false", region_field));
            }
            if (regions[r].contains("checkout") && !regions[r]["checkout"].is_boolean()) {
                return std::unexpected(region_field + ".checkout must be true or false");
            }
            if (!region_ids.insert(region.id).second) {
                return std::unexpected(
                    std::format("{}.regions names the region \"{}\" twice", field, region.id));
            }
            provider.regions.push_back(std::move(region));
        }
        sheet.providers.push_back(std::move(provider));
    }
    if (sheet.providers.empty() && sheet.find_offer("dedicated_cluster") != nullptr) {
        return std::unexpected(at + ".providers must list at least one provider for a dedicated "
                                    "cluster");
    }

    const json trust = merged.value("trust", json::array());
    if (!trust.is_array()) {
        return std::unexpected(at + ".trust must be an array");
    }
    for (std::size_t index = 0; index < trust.size(); ++index) {
        const std::string field = std::format("{}.trust[{}]", at, index);
        if (!trust[index].is_object()) {
            return std::unexpected(field + " must be an object");
        }
        OrderTrustClaim claim;
        for (auto [key, target] : {std::pair{"text", &claim.text}, std::pair{"url", &claim.url}}) {
            auto text = text_at(trust[index], key, field + "." + key);
            if (!text) {
                return std::unexpected(text.error());
            }
            *target = std::move(*text);
        }
        // A claim is shown only with the link that backs it.
        if (!claim_url(claim.url)) {
            return std::unexpected(field + ".url must be an https or mailto URL that backs it");
        }
        sheet.trust.push_back(std::move(claim));
    }

    const json limits = merged.value("limits", json::object());
    if (!limits.is_object()) {
        return std::unexpected(at + ".limits must be an object");
    }
    auto per_client = whole_at(
        limits, "orders_per_client_per_hour", at + ".limits.orders_per_client_per_hour", 1, 10'000);
    if (!per_client) {
        return std::unexpected(per_client.error());
    }
    auto per_hour =
        whole_at(limits, "orders_per_hour", at + ".limits.orders_per_hour", 1, 1'000'000);
    if (!per_hour) {
        return std::unexpected(per_hour.error());
    }
    auto per_email = whole_at(
        limits, "orders_per_email_per_day", at + ".limits.orders_per_email_per_day", 1, 1'000);
    if (!per_email) {
        return std::unexpected(per_email.error());
    }
    auto per_domain = whole_at(limits,
                               "orders_per_domain_per_hour",
                               at + ".limits.orders_per_domain_per_hour",
                               1,
                               100'000);
    if (!per_domain) {
        return std::unexpected(per_domain.error());
    }
    sheet.orders_per_client_per_hour = static_cast<int>(*per_client);
    sheet.orders_per_hour = static_cast<int>(*per_hour);
    sheet.orders_per_email_per_day = static_cast<int>(*per_email);
    sheet.orders_per_domain_per_hour = static_cast<int>(*per_domain);
    return sheet;
}

std::int64_t included_credit_cents(const OrderOffer& offer, int quantity) {
    return offer.monthly_cents * offer.credit_percent / 100 * std::max(quantity, 1);
}

std::string format_usd(std::int64_t cents) {
    const bool negative = cents < 0;
    const std::int64_t magnitude = negative ? -cents : cents;
    std::string out = (negative ? "-$" : "$") + thousands(magnitude / 100);
    if (magnitude % 100 != 0) {
        out += std::format(".{:02}", magnitude % 100);
    }
    return out;
}

std::string offer_slug(std::string_view offer_id) {
    if (std::ranges::find(offer_ids, offer_id) == offer_ids.end()) {
        return {};
    }
    std::string slug(offer_id);
    std::ranges::replace(slug, '_', '-');
    return slug;
}

std::string offer_from_slug(std::string_view slug) {
    std::string id(slug);
    std::ranges::replace(id, '-', '_');
    return std::ranges::find(offer_ids, id) == offer_ids.end() ? std::string() : id;
}

std::expected<control::NewCloudOrder, OrderFormErrors>
validate_order_form(const OrderSheet& sheet,
                    const Regions& regions,
                    const OrderFormInput& input,
                    bool checkout_available) {
    OrderFormErrors errors;
    control::NewCloudOrder order;
    const OrderOffer* offer = sheet.find_offer(input.offer);
    if (offer == nullptr) {
        errors["offer"] = "Choose one of the offers on the order sheet.";
        return std::unexpected(std::move(errors));
    }
    order.offer = offer->id;
    order.path = input.path == "checkout" ? "checkout" : "order_form";
    if (input.path != "checkout" && input.path != "order_form") {
        errors["path"] = "Choose to pay now or to send the order form.";
    }

    order.contact_name = single_line(input.contact_name);
    if (order.contact_name.empty() || characters(order.contact_name) > 200) {
        errors["contact_name"] = "Enter your name (at most 200 characters).";
    }
    order.contact_email = single_line(input.contact_email);
    if (!plausible_email_address(order.contact_email)) {
        errors["contact_email"] = "Enter an email address we can reach you at.";
    }
    order.company = single_line(input.company);
    if (characters(order.company) > 200) {
        errors["company"] = "The company name can be at most 200 characters.";
    }
    order.details = multi_line(input.details);
    if (characters(order.details) > 4000) {
        errors["details"] = "Tell us in at most 4,000 characters.";
    }
    const std::string quantity_text = single_line(input.quantity);
    int quantity = 1;
    if (!quantity_text.empty()) {
        const auto [end, parsed] = std::from_chars(
            quantity_text.data(), quantity_text.data() + quantity_text.size(), quantity);
        if (parsed != std::errc{} || end != quantity_text.data() + quantity_text.size() ||
            quantity < 1 || quantity > 1000) {
            errors["quantity"] = "Enter a quantity from 1 to 1,000.";
            quantity = 1;
        }
    }
    order.quantity = quantity;

    // Where it runs. "other" (anything not listed) is always an order form, with the details.
    const std::string location = single_line(input.location);
    bool listed = false;
    bool provisionable = false;
    if (location == "other") {
        if (order.details.empty()) {
            errors["details"] = "Tell us the provider, hardware or region you need.";
        }
    } else if (offer->id == "private_warehouse") {
        // On shared hardware: one of the shared regions.
        if (const Region* region = regions.find(location); region != nullptr) {
            order.region = region->name;
            listed = true;
            provisionable = true;
        } else {
            errors["location"] = "Choose one of the shared regions, or tell us what you need.";
        }
    } else if (offer->id == "dedicated_cluster") {
        const std::size_t colon = location.find(':');
        const OrderProvider* provider =
            colon == std::string::npos ? nullptr : sheet.find_provider(location.substr(0, colon));
        const OrderRegion* region =
            provider == nullptr ? nullptr
                                : find_region(provider->regions, location.substr(colon + 1));
        if (region != nullptr) {
            order.cloud_provider = provider->id;
            order.region = region->id;
            listed = true;
            provisionable = provider->checkout && region->checkout;
        } else {
            errors["location"] = "Choose a cloud provider and region, or tell us what you need.";
        }
    } else if (!location.empty()) {
        // On-prem: where it runs is in the details.
        errors["location"] = "Tell us where it runs in the details.";
    } else if (order.details.empty()) {
        errors["details"] =
            "Tell us where it runs: providers or bare metal, accelerators, regions.";
    }
    static_cast<void>(listed);

    if (order.path == "checkout" && !errors.contains("location")) {
        if (!offer->checkout) {
            errors["path"] = "This offer is ordered with the order form.";
        } else if (!checkout_available) {
            errors["path"] = "Card payment is not available right now: send the order form.";
        } else if (!provisionable) {
            const OrderProvider* provider =
                sheet.find_provider(order.cloud_provider);
            errors["path"] = location == "other"
                                 ? "Anything not listed is ordered with the order form."
                             : provider != nullptr && !provider->checkout
                                 ? "This provider is ordered with the order form until Lake "
                                   "provisions it automatically."
                                 : "This region is ordered with the order form: we will confirm "
                                   "availability.";
        } else if (order.quantity != 1 && !errors.contains("quantity")) {
            // A paid order is one warehouse or one cluster: the operator provisions one and
            // writes one host (orama-operator#635). More than one is a larger order.
            errors["path"] = "Card payment orders one at a time: send the order form for more.";
        }
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    order.amount_cents = offer->monthly_cents * order.quantity;
    order.included_credit_cents = included_credit_cents(*offer, order.quantity);
    order.currency = "usd";
    return order;
}

std::string order_location_label(const OrderSheet& sheet,
                                 const Regions& regions,
                                 const control::CloudOrder& order) {
    if (order.cloud_provider.empty() && order.region.empty()) {
        return order.offer == "on_prem" ? "As described" : "Not listed: as described";
    }
    if (order.cloud_provider.empty()) {
        const Region* region = regions.find(order.region);
        return region != nullptr ? region->label + " (shared region " + region->name + ")"
                                 : order.region;
    }
    const OrderProvider* provider = sheet.find_provider(order.cloud_provider);
    const OrderRegion* region =
        provider == nullptr ? nullptr : find_region(provider->regions, order.region);
    return (provider != nullptr ? provider->label : order.cloud_provider) + ", " +
           (region != nullptr ? region->label + " (" + region->id + ")" : order.region);
}

std::string order_status_token(std::string_view key, std::string_view order_id) {
    return signed_link_signature(key, "lake-order-status\n" + std::string(order_id));
}

bool order_status_token_matches(std::string_view key,
                                std::string_view order_id,
                                std::string_view token) {
    return signed_link_matches(key, "lake-order-status\n" + std::string(order_id), token);
}

std::string order_confirmation_token(std::string_view key, std::string_view order_id) {
    return signed_link_signature(key, "lake-order-confirm\n" + std::string(order_id));
}

bool order_confirmation_token_matches(std::string_view key,
                                      std::string_view order_id,
                                      std::string_view token) {
    return signed_link_matches(key, "lake-order-confirm\n" + std::string(order_id), token);
}

std::string order_email_state(const control::CloudOrder& order, std::string_view recipient) {
    if (recipient == "customer" && !control::cloud_order_contact_confirmed(order)) {
        return "confirm";
    }
    return order.state;
}

std::vector<std::string> order_email_recipients_due(const control::CloudOrder& order) {
    std::vector<std::string> due;
    const std::string& state = order.state;
    if (!control::cloud_order_contact_confirmed(order)) {
        // lake#1000: the address has not been shown to be the sender's. It gets one email, the
        // confirmation, and support hears of the order only once it is confirmed.
        if (state == "submitted" && order.customer_emailed_state.empty()) {
            due.emplace_back("customer");
        }
        return due;
    }
    if ((state == "submitted" || state == "paid" || state == "failed") &&
        order.support_emailed_state != state) {
        due.emplace_back("support");
    }
    if ((state == "submitted" || state == "paid" || state == "ready" || state == "failed") &&
        order.customer_emailed_state != state) {
        due.emplace_back("customer");
    }
    return due;
}

namespace {

// An order's email in Lake's words; order_email() writes it in the instance brand's.
Email order_email_in_lake_words(const OrderSheet& sheet,
                                const Regions& regions,
                                const ResendSettings& settings,
                                const control::CloudOrder& order,
                                std::string_view recipient,
                                std::string_view status_url,
                                const std::string& customer_support) {
    const OrderOffer* offer = sheet.find_offer(order.offer);
    const std::string offer_title = offer != nullptr ? offer->title : order.offer;
    const std::string price = std::format(
        "{}{} USD / month{}",
        offer != nullptr && offer->starting_from ? "From " : "",
        format_usd(order.amount_cents),
        order.quantity > 1 ? std::format(" ({} x {})",
                                         order.quantity,
                                         format_usd(order.amount_cents / order.quantity))
                           : std::string());
    // What was ordered, in Lake's own words and the configuration's: both recipients get it.
    std::string ordered;
    ordered += "Order: " + order.id + "\n";
    ordered += "Offer: " + offer_title + "\n";
    ordered += "How: " +
               std::string(order.path == "checkout" ? "Stripe Checkout" : "Order form, no payment") +
               "\n";
    ordered += "Where: " + order_location_label(sheet, regions, order) + "\n";
    ordered += "Quantity: " + std::to_string(order.quantity) + "\n";
    ordered += "Price: " + price + "\n";
    ordered += "Included usage credit: " + format_usd(order.included_credit_cents) +
               " USD / month; usage beyond it is billed at the price list\n";
    // Everything else, the sender's free text included, goes to support only: the customer's copy
    // goes to whatever address was typed, so it carries nothing the sender wrote (lake#833).
    std::string fields = ordered;
    fields += "Name: " + order.contact_name + "\n";
    fields += "Email: " + order.contact_email + "\n";
    if (!order.contact_email_confirmed_at.empty()) {
        fields += "Email confirmed: " + order.contact_email_confirmed_at + "\n";
    }
    fields +=
        "Company: " + (order.company.empty() ? std::string("(none given)") : order.company) + "\n";
    fields += "State: " + order.state + "\n";
    if (!order.host.empty()) {
        fields += "Host: " + order.host + "\n";
    }
    if (!order.failure_reason.empty()) {
        fields += "Failure: " + order.failure_reason + "\n";
    }
    if (!order.stripe_subscription_id.empty()) {
        fields += "Stripe subscription: " + order.stripe_subscription_id + "\n";
    }
    if (!order.organization_id.empty()) {
        fields += "Organization: " + order.organization_id + "\n";
    }
    fields += "Created: " + order.created_at + "\n";
    fields +=
        "\nDetails:\n" + (order.details.empty() ? std::string("(none)") : order.details) + "\n";

    Email email;
    const std::string who = order.company.empty() ? order.contact_name : order.company;
    if (recipient == "support") {
        email.to = {settings.support_email};
        email.reply_to = order.contact_email;
        const std::string what = order.state == "submitted" ? "Order form"
                                 : order.state == "paid"    ? "Paid order"
                                                            : "Order failed";
        email.subject = std::format("{}: {}, {} ({})", what, offer_title, who, order.id);
        std::string lead;
        if (order.state == "submitted") {
            lead = "An order form was sent from the order sheet. It is not paid and nothing is "
                   "provisioned: reply to the customer to take it forward.\n\n";
        } else if (order.state == "paid") {
            lead = "An order was paid through Stripe Checkout. The Orama operator provisions it "
                   "from the control plane.\n\n";
        } else {
            lead = "Provisioning this order failed. The customer has been told; please take it "
                   "up with them.\n\n";
        }
        email.text = lead + fields;
        return email;
    }
    email.to = {order.contact_email};
    email.reply_to = customer_support;
    if (!control::cloud_order_contact_confirmed(order)) {
        // lake#1000: whoever typed this address may not own it, so this email says only that an
        // order form named it, and how to confirm it or ignore it. No offer, price or field.
        email.subject = "Confirm your email for Lake order " + order.id;
        email.text = "Someone sent an order form on the Lake order sheet with this email address. "
                     "If it was you, confirm it here and we will pass your order to our team:\n\n" +
                     std::string(status_url) +
                     "\n\nThe link works for 7 days. If it was not you, ignore this email: nothing "
                     "more is sent to this address about it.\n\nOrder: " +
                     order.id + "\n";
        return email;
    }
    std::string lead;
    if (order.state == "submitted") {
        email.subject = "We received your Lake order (" + order.id + ")";
        lead = "Thank you. We received your order form and will be in touch shortly. Nothing has "
               "been charged and nothing is set up until we have spoken.\n\n";
    } else if (order.state == "paid") {
        email.subject = "Your Lake order is paid (" + order.id + ")";
        lead = "Thank you. Your payment is in and we are setting up your " + offer_title +
               ". We will email you again when it is ready.\n\n";
    } else if (order.state == "ready") {
        email.subject = "Your Lake " + offer_title + " is ready";
        lead = "Your " + offer_title + " is ready" +
               (order.host.empty() ? std::string(".") : " at https://" + order.host + "/.") +
               "\n\n";
    } else {
        email.subject = "There is a problem with your Lake order (" + order.id + ")";
        lead = "We could not finish setting up your " + offer_title +
               ". Our support team has been told and will be in touch.\n\n";
    }
    std::string text = lead;
    if (!status_url.empty()) {
        text += "Your order: " + std::string(status_url) + "\n\n";
    }
    text += ordered;
    text += "\nQuestions: reply to this email or write to " + customer_support + ".\n";
    email.text = std::move(text);
    return email;
}

} // namespace

Email order_email(const OrderSheet& sheet,
                  const Regions& regions,
                  const ResendSettings& settings,
                  const control::CloudOrder& order,
                  std::string_view recipient,
                  std::string_view status_url) {
    // The customer is told to write to the brand's support address (lake#1135); orders still go
    // to LAKE_SUPPORT_EMAIL.
    const Brand& brand = instance_brand();
    const std::string customer_support =
        brand.custom && !brand.support_email.empty() ? brand.support_email : settings.support_email;
    Email email = order_email_in_lake_words(
        sheet, regions, settings, order, recipient, status_url, customer_support);
    email.subject = brand.apply(email.subject);
    email.text = brand.apply(email.text);
    return email;
}

std::vector<PriceListRow> price_list_rows(const std::vector<control::BillingPrice>& prices) {
    std::vector<PriceListRow> rows;
    for (const control::BillingPrice& price : prices) {
        const UsageDimension* dimension = find_usage_dimension(price.dimension);
        if (dimension != nullptr && !dimension->priced) {
            continue;
        }
        PriceListRow row;
        row.title = dimension != nullptr ? std::string(dimension->title) : price.dimension;
        if (!price.class_name.empty()) {
            std::string name = price.class_name;
            for (char& c : name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            row.title += " (" + name + ")";
        }
        // Millionths of a dollar, shown to the last digit that is not zero.
        std::string amount =
            std::format("{}.{:06}", price.unit_micros / 1'000'000, price.unit_micros % 1'000'000);
        while (amount.back() == '0') {
            amount.pop_back();
        }
        if (amount.back() == '.') {
            amount += "00";
        } else if (amount.size() - amount.find('.') == 2) {
            amount += '0';
        }
        const std::string_view unit = dimension != nullptr ? dimension->unit : std::string_view();
        const std::uint64_t per = std::max<std::uint64_t>(price.per, 1);
        std::string per_text;
        if (unit == "mcpu_seconds" && per == 60'000) {
            per_text = "vCPU-minute";
        } else if (unit == "mib_seconds" && per == 61'440) {
            per_text = "GiB-minute";
        } else if (unit == "gpu_seconds" && per == 60) {
            per_text = "GPU-minute";
        } else if ((unit == "seconds" || unit == "instance_seconds") && per == 60) {
            per_text = "minute";
        } else if (unit == "bytes" && per == 1'073'741'824) {
            per_text = "GiB";
        } else if (unit == "tokens" && per == 1'000'000) {
            per_text = "million tokens";
        } else if (per == 1) {
            per_text = std::string(unit.empty() ? "unit" : unit);
        } else {
            per_text = thousands(static_cast<std::int64_t>(per)) + " " +
                       std::string(unit.empty() ? "units" : unit);
        }
        row.rate = "$" + amount + " per " + per_text;
        rows.push_back(std::move(row));
    }
    return rows;
}

int order_email_retry_seconds(int attempts) {
    const int exponent = std::clamp(attempts - 1, 0, 6);
    return std::min(60 << exponent, 3600);
}

bool OrderRateLimiter::allow(std::string_view client,
                             std::string_view path,
                             std::chrono::steady_clock::time_point now) {
    const auto window_start = now - std::chrono::hours(1);
    std::lock_guard lock(mutex_);
    for (auto& [kind, times] : all_) {
        while (!times.empty() && times.front() <= window_start) {
            times.pop_front();
        }
    }
    for (auto it = clients_.begin(); it != clients_.end();) {
        while (!it->second.empty() && it->second.front() <= window_start) {
            it->second.pop_front();
        }
        it = it->second.empty() ? clients_.erase(it) : std::next(it);
    }
    auto& on_path = all_[std::string(path)];
    if (static_cast<int>(on_path.size()) >= per_hour_) {
        return false;
    }
    auto found = clients_.find(client);
    if (found != clients_.end() && static_cast<int>(found->second.size()) >= per_client_) {
        return false;
    }
    if (found == clients_.end()) {
        if (clients_.size() >= 10'000) {
            // Forget the address whose newest order is oldest.
            const auto oldest = std::ranges::min_element(
                clients_, {}, [](const auto& entry) { return entry.second.back(); });
            clients_.erase(oldest);
        }
        found =
            clients_
                .emplace(std::string(client), std::deque<std::chrono::steady_clock::time_point>{})
                .first;
    }
    found->second.push_back(now);
    on_path.push_back(now);
    return true;
}

} // namespace lake::api
