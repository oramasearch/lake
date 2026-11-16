// SPDX-License-Identifier: MIT

// Coupon codes for ads (lake#1156, docs/api/billing.md#coupon-codes): the operator creates a code
// worth an amount of cloud credit (create_coupon, list_coupons, disable_coupon), and a person
// redeems it for an account they administer (redeem_coupon), in Billing or carried through sign-up
// by a `?code=` link. A redemption is an ordinary grant from Orama, given through
// give_balance_grant (lake#1134) with the reference `coupon:<coupon id>:<customer id>`, so the
// spending order, expiry, no-overdraft rule, email and idempotency are the grant's. Billing, so the
// records are in the control plane and registered only where billing is on.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include "lake/api/balance.hpp"
#include "lake/control/identity_crypto.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

// Failed redemptions allowed per client address and per person in one window, so codes cannot be
// guessed (a generated code has about 78 bits). The window is an hour.
constexpr int coupon_failures_allowed = 10;
constexpr int coupon_failure_window_seconds = 3600;
// Generated codes: 16 characters from an alphabet without look-alikes (no 0/O, 1/I/L, U).
constexpr std::string_view coupon_alphabet = "ABCDEFGHJKMNPQRSTVWXYZ23456789";
constexpr std::size_t generated_code_length = 16;

ApiResult no_store() {
    return ApiResult::failure("store_unavailable", "no control-plane store is configured");
}

ApiResult from_store(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::string, ApiError> administrator_only(const json&, ApiContext&) {
    return std::unexpected(
        ApiError{"unauthorized", "the operator or the instance administrator is required", {}});
}

bool instance_administrator(const ApiContext& context) {
    return context.operator_credential || context.acting_identity == "admin";
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

json object_schema(json properties, json required) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

std::string now_seconds_text() {
    return std::format("{:%FT%T}Z",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

// An unguessable code, or empty when the system's random source failed.
std::string generated_code() {
    std::string code;
    while (code.size() < generated_code_length) {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            return {};
        }
        for (const unsigned char byte : bytes) {
            // Rejection sampling keeps every character equally likely.
            if (byte >= 240 || code.size() >= generated_code_length) {
                continue;
            }
            code.push_back(coupon_alphabet[byte % coupon_alphabet.size()]);
        }
    }
    return code;
}

// What a code's state is now: active, disabled, expired or used_up.
std::string coupon_state(const control::Coupon& coupon) {
    if (!coupon.disabled_at.empty()) {
        return "disabled";
    }
    if (!coupon.expires_at.empty() && coupon.expires_at <= now_seconds_text()) {
        return "expired";
    }
    if (coupon.max_redemptions && coupon.redemptions >= *coupon.max_redemptions) {
        return "used_up";
    }
    return "active";
}

json coupon_json(const control::Coupon& coupon) {
    return {
        {"id", coupon.id},
        {"code", coupon.code},
        {"amount_micros", coupon.amount_micros},
        {"credit_days", coupon.credit_days},
        {"expires_at", coupon.expires_at},
        {"max_redemptions", coupon.max_redemptions ? json(*coupon.max_redemptions) : json(nullptr)},
        {"new_accounts_only", coupon.new_accounts_only},
        {"campaign", coupon.campaign},
        {"redemptions", coupon.redemptions},
        {"state", coupon_state(coupon)},
        {"created_by", coupon.created_by},
        {"created_at", coupon.created_at},
        {"disabled_at", coupon.disabled_at},
        {"disabled_by", coupon.disabled_by}};
}

json coupon_schema() {
    return {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"code", {{"type", "string"}}},
          {"amount_micros", {{"type", "integer"}}},
          {"credit_days", {{"type", "integer"}}},
          {"expires_at", {{"type", "string"}}},
          {"max_redemptions", {{"type", json::array({"integer", "null"})}}},
          {"new_accounts_only", {{"type", "boolean"}}},
          {"campaign", {{"type", "string"}}},
          {"redemptions", {{"type", "integer"}}},
          {"state", {{"type", "string"}, {"enum", {"active", "disabled", "expired", "used_up"}}}},
          {"created_by", {{"type", "string"}}},
          {"created_at", {{"type", "string"}}},
          {"disabled_at", {{"type", "string"}}},
          {"disabled_by", {{"type", "string"}}}}},
        {"required", json::array({"id", "code", "amount_micros", "state"})}};
}

// Where an ad links: the apex picker (`https://<apex>/?code=`), which carries the code to the
// region the person chooses, else this Lake's own front page.
std::string coupon_link(const ApiContext& context, const std::string& code) {
    std::string origin;
    if (context.regions != nullptr && !context.regions->apex_host.empty()) {
        origin = "https://" + context.regions->apex_host;
    } else if (!context.external_origin.empty()) {
        origin = context.external_origin;
    } else if (!context.public_host.empty()) {
        origin = "https://" + context.public_host;
    } else {
        origin = context.request_origin;
    }
    return origin + "/?code=" + code;
}

// Each region is its own Lake (docs/api/regions.md): this Lake's region, and the others, where the
// same code must be created too for the link to work there.
json region_note(const ApiContext& context) {
    json others = json::array();
    if (context.regions == nullptr || context.regions->name.empty()) {
        return {{"region", nullptr}, {"other_regions", others}};
    }
    for (const Region& region : context.regions->list) {
        if (region.name != context.regions->name) {
            others.push_back(
                {{"name", region.name}, {"label", region.label}, {"host", region.host}});
        }
    }
    return {{"region", {{"name", context.regions->name}, {"label", context.regions->label}}},
            {"other_regions", others}};
}

void audit_coupon_admin(ApiContext& context, std::string kind, json details) {
    context.audit_kind = std::move(kind);
    context.audit_object = "billing/coupons";
    context.audit_details = std::move(details);
}

void audit_redemption(ApiContext& context,
                      const std::string& customer_id,
                      std::string kind,
                      json details) {
    context.audit_customer_id = customer_id;
    context.audit_kind = std::move(kind);
    context.audit_object = "customer/" + customer_id + "/billing";
    context.audit_details = std::move(details);
}

// What a refused redemption tells the person, by the reason the store or the checks gave.
std::string refusal_message(const std::string& reason) {
    if (reason == "disabled") {
        return "This code is no longer offered.";
    }
    if (reason == "expired") {
        return "This code has expired.";
    }
    if (reason == "used_up") {
        return "This code has been used as many times as it can be.";
    }
    if (reason == "already_redeemed") {
        return "This account has already redeemed this code.";
    }
    if (reason == "email_redeemed") {
        return "You have already redeemed this code with another account.";
    }
    if (reason == "not_new_account") {
        return "This code is for new accounts only, and this account was created before it.";
    }
    if (reason == "account_used") {
        return "This code is for new accounts only, and this account has already been used.";
    }
    return "This code cannot be redeemed.";
}

ApiResult refused(std::string code, std::string message, std::string reason, std::string path) {
    ApiResult result = ApiResult::failure(std::move(code), std::move(message), std::move(path));
    result.error.details = {{"reason", std::move(reason)}};
    return result;
}

} // namespace

void register_coupon_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_coupon",
        .title = "Create a coupon code",
        .description =
            "Operator or instance administrator only. Creates a coupon code for ads: whoever "
            "redeems it (redeem_coupon) gets a credit from Orama of `amount_micros` (millionths of "
            "a US dollar, whole cents, $0.01 to $10,000) that lasts `credit_days` after the "
            "redemption (default 90). `code` is typed (4 to 32 letters and digits; blanks and "
            "hyphens are dropped, case does not matter) or, when left out, generated: 16 "
            "characters, unguessable. `expires_at` (UTC, YYYY-MM-DDTHH:MM:SSZ) is when the code "
            "stops working, `max_redemptions` the most redemptions in total, "
            "`new_accounts_only` (default true) refuses accounts older than the code or already "
            "used, and `campaign` is an attribution label (lower case). Each region is a separate "
            "Lake: a code works only in the region it is created in, so create it with the same "
            "code in every region the ad reaches (`other_regions`). Answers the coupon and the "
            "link for the ad. Audited (billing.coupon.created).",
        .input_schema = object_schema(
            {{"code", {{"type", "string"}, {"minLength", 4}, {"maxLength", 64}}},
             {"amount_micros",
              {{"type", "integer"},
               {"minimum", 10'000},
               {"maximum", control::balance_grant_most_micros},
               {"description", "whole cents, in millionths of a US dollar"}}},
             {"credit_days",
              {{"type", "integer"},
               {"minimum", 1},
               {"maximum", 3650},
               {"description", "how long the credit lasts after redemption (default 90)"}}},
             {"expires_at",
              {{"type", "string"}, {"description", "when the code stops working, UTC"}}},
             {"max_redemptions", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10'000'000}}},
             {"new_accounts_only", {{"type", "boolean"}}},
             {"campaign", {{"type", "string"}, {"maxLength", 64}}}},
            json::array({"amount_micros"})),
        .output_schema = object_schema({{"coupon", coupon_schema()},
                                        {"link", {{"type", "string"}}},
                                        {"region", {{"type", json::array({"object", "null"})}}},
                                        {"other_regions", {{"type", "array"}}}},
                                       json::array({"coupon", "link"})),
        .kind = CapabilityKind::Action,
        .object = "billing coupons",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return ApiResult::failure("unauthorized",
                                          "the operator or the instance administrator is required");
            }
            if (!context.store) {
                return no_store();
            }
            control::Coupon coupon;
            const bool typed = input.contains("code");
            if (typed) {
                const auto code =
                    control::coupon_code_normalized(input.at("code").get<std::string>());
                if (!code) {
                    return ApiResult::failure("invalid_input",
                                              "a code is 4 to 32 letters and digits (blanks and "
                                              "hyphens are dropped)",
                                              "/code");
                }
                coupon.code = *code;
            }
            coupon.amount_micros = input.at("amount_micros").get<std::int64_t>();
            if (coupon.amount_micros % 10'000 != 0) {
                return ApiResult::failure(
                    "invalid_input", "the amount is whole cents", "/amount_micros");
            }
            coupon.credit_days = input.value("credit_days", 90);
            coupon.expires_at = input.value("expires_at", std::string());
            if (!coupon.expires_at.empty()) {
                const auto at = balance_utc_seconds(coupon.expires_at);
                const auto now =
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                if (!at) {
                    return ApiResult::failure("invalid_input",
                                              "expires_at is a UTC time, YYYY-MM-DDTHH:MM:SSZ",
                                              "/expires_at");
                }
                if (*at <= now || *at > now + std::chrono::days{3660}) {
                    return ApiResult::failure("invalid_input",
                                              "expires_at is in the future and within 10 years",
                                              "/expires_at");
                }
            }
            if (input.contains("max_redemptions")) {
                coupon.max_redemptions = input.at("max_redemptions").get<int>();
            }
            coupon.new_accounts_only = input.value("new_accounts_only", true);
            coupon.campaign = input.value("campaign", std::string());
            coupon.created_by =
                context.operator_credential ? std::string("operator") : context.acting_identity;
            if (!coupon.campaign.empty()) {
                control::Coupon probe = coupon;
                probe.code = "PROBE";
                if (const auto problem = control::coupon_problem(probe)) {
                    return ApiResult::failure("invalid_input", *problem, "/campaign");
                }
            }
            control::Result<control::Coupon> created =
                std::unexpected(control::StoreError{"store_error", "no code was generated"});
            // A generated code is retried on the rare clash; a typed one is the person's choice.
            for (int attempt = 0; attempt < (typed ? 1 : 3); ++attempt) {
                if (!typed) {
                    coupon.code = generated_code();
                    if (coupon.code.empty()) {
                        return ApiResult::failure("crypto_error",
                                                  "the random source failed; try again");
                    }
                }
                created = context.store->create_coupon(coupon);
                if (created || created.error().code != "conflict") {
                    break;
                }
            }
            if (!created) {
                if (created.error().code == "conflict") {
                    return ApiResult::failure(
                        "conflict", "the code " + coupon.code + " is taken", "/code");
                }
                return from_store(created.error());
            }
            json out = region_note(context);
            out["coupon"] = coupon_json(*created);
            out["link"] = coupon_link(context, created->code);
            audit_coupon_admin(context,
                               "billing.coupon.created",
                               {{"coupon_id", created->id},
                                {"code", created->code},
                                {"generated", !typed},
                                {"amount_micros", created->amount_micros},
                                {"credit_days", created->credit_days},
                                {"expires_at", created->expires_at},
                                {"max_redemptions", out["coupon"]["max_redemptions"]},
                                {"new_accounts_only", created->new_accounts_only},
                                {"campaign", created->campaign}});
            return ApiResult::success(std::move(out));
        }});

    registry.add(Capability{
        .name = "list_coupons",
        .title = "Coupon codes and their campaigns",
        .description =
            "Operator or instance administrator only. Every coupon code on this Lake, newest "
            "first (`campaign`: one campaign's only), each with its state (active, disabled, "
            "expired, used_up), its redemptions and its ads report: `redeemed` (redemptions that "
            "became a credit), `topped_up` (redeeming accounts that went on to a first paid "
            "top-up), `spend_micros` (what those accounts were charged for usage after the "
            "credit was given) and `paid_micros` (what they paid in top-ups since). `campaigns` "
            "sums the same per campaign. Counts only: no workspace data.",
        .input_schema =
            object_schema({{"campaign", {{"type", "string"}, {"maxLength", 64}}}}, json::array()),
        .output_schema =
            object_schema({{"coupons", {{"type", "array"}, {"items", coupon_schema()}}},
                           {"campaigns", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
                          json::array({"coupons", "campaigns"})),
        .kind = CapabilityKind::Read,
        .object = "billing coupons",
        .action = "read",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return ApiResult::failure("unauthorized",
                                          "the operator or the instance administrator is required");
            }
            if (!context.store) {
                return no_store();
            }
            const auto reports = context.store->coupon_reports(input.value("campaign", ""));
            if (!reports) {
                return from_store(reports.error());
            }
            json coupons = json::array();
            struct Totals {
                int coupons = 0, redeemed = 0, topped_up = 0;
                std::int64_t spend = 0, paid = 0;
            };
            std::map<std::string, Totals> campaigns;
            for (const control::CouponReport& report : *reports) {
                json row = coupon_json(report.coupon);
                row["redeemed"] = report.redeemed;
                row["topped_up"] = report.topped_up;
                row["spend_micros"] = report.spend_micros;
                row["paid_micros"] = report.paid_micros;
                coupons.push_back(std::move(row));
                Totals& totals = campaigns[report.coupon.campaign];
                ++totals.coupons;
                totals.redeemed += report.redeemed;
                totals.topped_up += report.topped_up;
                totals.spend += report.spend_micros;
                totals.paid += report.paid_micros;
            }
            json by_campaign = json::array();
            for (const auto& [name, totals] : campaigns) {
                by_campaign.push_back({{"campaign", name},
                                       {"coupons", totals.coupons},
                                       {"redeemed", totals.redeemed},
                                       {"topped_up", totals.topped_up},
                                       {"spend_micros", totals.spend},
                                       {"paid_micros", totals.paid}});
            }
            audit_coupon_admin(
                context,
                "billing.coupon.listed",
                {{"campaign", input.value("campaign", "")}, {"coupons", coupons.size()}});
            return ApiResult::success(
                {{"coupons", std::move(coupons)}, {"campaigns", std::move(by_campaign)}});
        }});

    registry.add(Capability{
        .name = "disable_coupon",
        .title = "Disable a coupon code",
        .description =
            "Operator or instance administrator only. Stops a coupon code at once (`coupon_id` or "
            "`code`): later redemptions are refused. Credit already given stays. Calling it again "
            "changes nothing. Audited (billing.coupon.disabled).",
        .input_schema = object_schema({{"coupon_id", {{"type", "string"}}},
                                       {"code", {{"type", "string"}, {"maxLength", 64}}}},
                                      json::array()),
        .output_schema = object_schema({{"coupon", coupon_schema()}}, json::array({"coupon"})),
        .kind = CapabilityKind::Action,
        .object = "billing coupons",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return ApiResult::failure("unauthorized",
                                          "the operator or the instance administrator is required");
            }
            if (!context.store) {
                return no_store();
            }
            const std::string id = input.value("coupon_id", std::string());
            const std::string typed = input.value("code", std::string());
            if (id.empty() == typed.empty()) {
                return ApiResult::failure("invalid_input",
                                          "name the coupon by coupon_id or code, not both",
                                          id.empty() ? "/coupon_id" : "/code");
            }
            std::string coupon_id = id;
            if (!typed.empty()) {
                const auto code = control::coupon_code_normalized(typed);
                const auto found = code ? context.store->coupon_by_code(*code)
                                        : control::Result<control::Coupon>(std::unexpected(
                                              control::StoreError{"not_found", "no such code"}));
                if (!found) {
                    return found.error().code == "not_found"
                               ? ApiResult::failure("not_found", "no such code", "/code")
                               : from_store(found.error());
                }
                coupon_id = found->id;
            }
            const auto disabled = context.store->disable_coupon(
                coupon_id,
                context.operator_credential ? std::string("operator") : context.acting_identity);
            if (!disabled) {
                return disabled.error().code == "not_found"
                           ? ApiResult::failure("not_found", "no coupon " + coupon_id, "/coupon_id")
                           : from_store(disabled.error());
            }
            audit_coupon_admin(context,
                               "billing.coupon.disabled",
                               {{"coupon_id", disabled->id},
                                {"code", disabled->code},
                                {"campaign", disabled->campaign},
                                {"redemptions", disabled->redemptions}});
            return ApiResult::success({{"coupon", coupon_json(*disabled)}});
        }});

    registry.add(Capability{
        .name = "redeem_coupon",
        .title = "Redeem a coupon code",
        .description =
            "Redeems a coupon code for an account you administer (`customer_id`), as the person "
            "signed in: the account gets a credit from Orama (label \"Credit from Orama\") of the "
            "code's amount, lasting the code's `credit_days`. Like every credit from Orama it is "
            "spent before paid money, is not cash and does not unlock the overdraft. One "
            "redemption per account and per verified email (one person cannot redeem a code "
            "twice with two accounts); a new-accounts-only code refuses accounts older than the "
            "code or already used. Codes are case-insensitive. Refusals answer `conflict` or "
            "`not_found` with `details.reason` (disabled, expired, used_up, already_redeemed, "
            "email_redeemed, not_new_account, account_used, unknown); failed attempts are limited "
            "per client and per person (`rate_limited`). Audited (billing.coupon.redeemed, "
            "billing.coupon.refused).",
        .input_schema = object_schema({{"customer_id", {{"type", "string"}}},
                                       {"code", {{"type", "string"}, {"maxLength", 64}}}},
                                      json::array({"customer_id", "code"})),
        .output_schema = object_schema(
            {{"customer_id", {{"type", "string"}}},
             {"code", {{"type", "string"}}},
             {"amount_micros", {{"type", "integer"}}},
             {"expires_at", {{"type", "string"}}},
             {"message", {{"type", "string"}}},
             {"credit", {{"type", "object"}}},
             {"emailed", {{"type", "boolean"}}},
             {"email_error", {{"type", "string"}}},
             {"balance", {{"type", "object"}}}},
            json::array(
                {"customer_id", "code", "amount_micros", "expires_at", "message", "balance"})),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            // A person redeems a code, never the operator credential or an agent acting for one.
            if (context.operator_credential || context.acting_user_id.empty() ||
                !acting_agent(context).empty()) {
                return ApiResult::failure("unauthorized",
                                          "a coupon code is redeemed by a person signed in to "
                                          "their own account");
            }
            const std::string typed = input.at("code");
            // Never the code itself in the audit of a refusal: a short digest says which.
            const std::string typed_digest =
                control::identity_crypto::sha256_hex(typed).substr(0, 12);
            std::vector<std::string> keys{control::identity_crypto::sha256_hex(
                "lake-coupon-attempt:person:" + context.acting_user_id)};
            if (!context.client_address.empty()) {
                keys.push_back(control::identity_crypto::sha256_hex("lake-coupon-attempt:client:" +
                                                                    context.client_address));
            }
            const auto exhausted = context.store->coupon_attempts_exhausted(
                keys, coupon_failures_allowed, coupon_failure_window_seconds);
            if (!exhausted) {
                return from_store(exhausted.error());
            }
            if (*exhausted) {
                audit_redemption(context,
                                 customer_id,
                                 "billing.coupon.refused",
                                 {{"reason", "rate_limited"}, {"code_digest", typed_digest}});
                return refused("rate_limited",
                               "Too many codes that did not work. Try again in an hour.",
                               "rate_limited",
                               "");
            }
            // Every refusal below counts against the client and the person.
            const auto refuse =
                [&](std::string code, std::string reason, std::string coupon_id) -> ApiResult {
                static_cast<void>(
                    context.store->coupon_attempt_failed(keys, coupon_failure_window_seconds));
                audit_redemption(
                    context,
                    customer_id,
                    "billing.coupon.refused",
                    {{"reason", reason}, {"code_digest", typed_digest}, {"coupon_id", coupon_id}});
                const std::string message = reason == "unknown"
                                                ? "That code does not work on this Lake."
                                                : refusal_message(reason);
                return refused(std::move(code), message, std::move(reason), "/code");
            };
            const auto code = control::coupon_code_normalized(typed);
            if (!code) {
                return refuse("not_found", "unknown", "");
            }
            const auto person = context.store->get_user(context.acting_user_id);
            if (!person) {
                return from_store(person.error());
            }
            const std::string email_digest = control::coupon_email_digest(person->email);
            if (email_digest.empty()) {
                return refused("conflict",
                               "Your account needs a verified email address to redeem a code.",
                               "no_verified_email",
                               "");
            }
            const auto coupon = context.store->coupon_by_code(*code);
            if (!coupon) {
                return coupon.error().code == "not_found" ? refuse("not_found", "unknown", "")
                                                          : from_store(coupon.error());
            }
            control::CouponRedemption wanted;
            wanted.coupon_id = coupon->id;
            wanted.customer_id = customer_id;
            wanted.email_sha256 = email_digest;
            wanted.redeemed_by =
                context.acting_identity.empty() ? context.acting_user_id : context.acting_identity;
            const auto claim = context.store->claim_coupon(wanted);
            if (!claim) {
                if (claim.error().code == "conflict") {
                    // The store's reasons; a uniqueness race between two claims (the database's
                    // own message) is a redemption that already exists.
                    static constexpr std::array<std::string_view, 7> known{"disabled",
                                                                           "expired",
                                                                           "used_up",
                                                                           "already_redeemed",
                                                                           "email_redeemed",
                                                                           "not_new_account",
                                                                           "account_used"};
                    const std::string& reason = claim.error().message;
                    return refuse("conflict",
                                  std::ranges::find(known, reason) != known.end()
                                      ? reason
                                      : std::string("already_redeemed"),
                                  coupon->id);
                }
                return from_store(claim.error());
            }
            // The grant: an ordinary credit from Orama. Its reference makes a retry (or a
            // redemption resumed after an interruption) grant once.
            BalanceGrantRequest grant;
            grant.customer_id = customer_id;
            grant.amount_micros = claim->coupon.amount_micros;
            grant.reason = "Coupon code " + claim->coupon.code;
            grant.expires_at = std::format(
                "{:%FT%T}Z",
                std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) +
                    std::chrono::days{claim->coupon.credit_days});
            grant.reference = "coupon:" + claim->coupon.id + ":" + customer_id;
            grant.granted_by = "coupon:" + claim->coupon.id;
            grant.action = "redeem_coupon";
            const auto given = give_balance_grant(context, grant);
            if (!given) {
                return given.error();
            }
            if (!given->credit.id.empty()) {
                if (const auto finished = context.store->finish_coupon_redemption(
                        claim->redemption.id, given->credit.id);
                    !finished) {
                    return from_store(finished.error());
                }
            }
            const std::string expires =
                given->credit.expires_at.empty() ? grant.expires_at : given->credit.expires_at;
            const std::string message =
                "You have " + balance_usd(claim->coupon.amount_micros) + " of credit from Orama.";
            json out{{"customer_id", customer_id},
                     {"code", claim->coupon.code},
                     {"amount_micros", claim->coupon.amount_micros},
                     {"expires_at", expires},
                     {"message", message},
                     {"credit", balance_credit_json(given->credit, claim->coupon.amount_micros)},
                     {"emailed", given->emailed},
                     {"balance", given->balance}};
            if (!given->email_error.empty()) {
                out["email_error"] = given->email_error;
            }
            audit_redemption(context,
                             customer_id,
                             "billing.coupon.redeemed",
                             {{"coupon_id", claim->coupon.id},
                              {"code", claim->coupon.code},
                              {"campaign", claim->coupon.campaign},
                              {"redemption_id", claim->redemption.id},
                              {"credit_id", given->credit.id},
                              {"amount_micros", claim->coupon.amount_micros},
                              {"expires_at", expires},
                              {"resumed", claim->resumed},
                              {"emailed", given->emailed},
                              {"balance_micros", given->balance_micros}});
            return ApiResult::success(std::move(out));
        }});
}

} // namespace lake::api
