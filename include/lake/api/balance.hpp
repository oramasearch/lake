#pragma once

// The prepaid account balance (lake#1114, liquid-bridge decision 67, docs/api/billing.md#balance):
// lake.live is prepaid. Each account (a person's or an organization's) has a balance in US dollars:
// credits are paid top-ups (Stripe Checkout, or an auto top-up off a saved card), charges are its
// usage at the published price list, refreshed every few minutes by the store root's owner. The
// balance is credits minus charges. While it is low or negative the product nags (viewer, a banner,
// a warning on every API and MCP answer) and emails at each crossing; at the overdraft limit every
// capability that starts paid work answers 402 payment_required. Billing on only (LAKE_BILLING):
// with billing off none of this exists. The records live in lake's Postgres control plane as
// billing (decision 56); usage stays in each workspace Zetta.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/billing.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/resend.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// The instance's balance settings, from its environment (docs/serving/configuration.md#balance).
struct BalanceSettings {
    std::int64_t first_top_up_micros = 20'000'000;   // LAKE_BALANCE_FIRST_TOP_UP_USD
    std::int64_t minimum_top_up_micros = 20'000'000; // LAKE_BALANCE_MINIMUM_TOP_UP_USD
    std::int64_t overdraft_micros = 50'000'000;      // LAKE_BALANCE_OVERDRAFT_USD
    std::int64_t warning_micros = 10'000'000;        // LAKE_BALANCE_WARNING_USD
    std::chrono::seconds refresh_interval{300};      // LAKE_BALANCE_REFRESH_SECONDS
    int auto_top_ups_per_day = 5;                    // LAKE_AUTO_TOP_UPS_PER_DAY
    // Auto top-up's defaults when an owner turns it on without saying.
    std::int64_t auto_top_up_threshold_micros = 5'000'000;
    std::int64_t auto_top_up_amount_micros = 20'000'000;
};

// Reads the variables' values (empty: the default). Whole US dollars, refresh in seconds. An error
// names the variable: not a whole number, a top-up below $1, a minimum above the first, a refresh
// under 30 seconds, a daily limit outside 1-100.
std::expected<BalanceSettings, std::string>
balance_settings_from_environment(std::string_view first_top_up,
                                  std::string_view minimum_top_up,
                                  std::string_view overdraft,
                                  std::string_view warning,
                                  std::string_view refresh_seconds,
                                  std::string_view auto_top_ups_per_day);

// Sends one balance email through Resend under `idempotency_key`; Resend's email id or why not.
// The server's sends through send_resend_email with LAKE_RESEND_API_KEY; tests inject a fake.
using BalanceMailer =
    std::function<std::expected<std::string, std::string>(const Email&, std::string_view key)>;

// What the balance needs at run time, set on ApiContext::balance where billing is on: the
// settings, how its emails go (empty: they wait), the browser origin its links use, and the
// payment provider for auto top-up.
struct BalanceRuntime {
    BalanceSettings settings;
    BalanceMailer mailer;
    std::string origin;                   // https://lake.live; empty: links are site paths
    std::string email_unavailable_reason; // why the mailer is empty
};

// An account's balance as the product shows it.
struct BalanceView {
    std::int64_t balance_micros = 0;
    std::int64_t overdraft_micros = 0; // how far below $0 it may go
    // ok | low (under the warning amount) | negative (below $0) | blocked (at or past the
    // overdraft, or never topped up)
    std::string state = "ok";
    // Why blocked: first_top_up or overdraft; empty otherwise.
    std::string reason;
    // How much to add: the first top-up, or what brings the balance back over the warning amount,
    // at least the minimum top-up, in whole dollars.
    std::int64_t add_micros = 0;
    // Why the gate never stops this account (operator, administrator, paid_order); empty: it may.
    std::string exempt;
};

BalanceView balance_view(const control::AccountBalance& balance,
                         const BalanceSettings& settings,
                         std::string exempt = {});

// The console page that opens Add funds for an account (sign-in first when needed):
// /billing/add-funds?customer_id=<id>, under `origin` when it is set. Emails, the banner and every
// 402 answer use it.
std::string add_funds_link(std::string_view origin, std::string_view customer_id);

// {customer_id, currency, balance_micros, overdraft_micros, limit_micros, state, reason,
// add_micros, add_funds_url, first_top_up_micros, minimum_top_up_micros, exempt, refreshed_at,
// refresh_error, auto_top_up}: account_balance's answer and viewer.balance.
nlohmann::json balance_json(const control::AccountBalance& balance,
                            const BalanceView& view,
                            const BalanceRuntime& runtime);

// The capabilities that start or grow something that costs money (docs/api/billing.md#the-gate):
// apps, services, models and indexes (create, deploy, start, expose); runs, training, experiments;
// agent launches and schedules; image builds; model gateway calls; storage growth. Registry::add
// marks each (`Capability::starts_paid_work`); Registry::call refuses them past the overdraft.
struct PaidWork {
    std::string_view capability;
    std::string_view why;
};
std::span<const PaidWork> paid_work_capabilities();
bool starts_paid_work(std::string_view capability);

// The account a call charges: the workspace it acts on, else its `customer_id`, else its
// `organization_id`'s account, else the caller's personal account. Empty when none.
std::string balance_account_of(const nlohmann::json& input, ApiContext& context);

// Why the gate exempts the caller (operator credential, instance administrator); empty otherwise.
std::string balance_caller_exemption(const ApiContext& context);

// The gate (Registry::call, before a marked capability's handler): 402 payment_required with
// {customer_id, balance_micros, overdraft_micros, add_micros, add_funds_url, reason} in
// error.details when the account is blocked; nullopt to go on. Never refuses where billing is off
// (no ApiContext::balance), for an exempt caller or account, or a call that charges no account.
std::optional<ApiResult>
balance_refusal(const Capability& capability, const nlohmann::json& input, ApiContext& context);

// The warning every answer carries while the account the call acted on is low, negative or
// blocked (the one shared answer path, Registry::call): {code: "balance_low" | "balance_negative" |
// "balance_blocked", message, customer_id, balance_micros, add_micros, add_funds_url}; null
// otherwise.
nlohmann::json balance_warning(const nlohmann::json& input, ApiContext& context);

// A balance notice from the payment provider (a top-up paid, a card saved, an auto top-up that
// failed), applied once and audited centrally; the route's answer.
NoticeOutcome
apply_balance_notice(ApiContext& context, std::string_view provider, const PaymentNotice& notice);

// The emails before it bites, sent once per crossing and reset by a credit.
enum class BalanceCrossing {
    limit_warning,     // within the warning amount of the overdraft limit
    negative,          // passed $0
    blocked,           // at the overdraft limit: paid work is refused
    auto_top_up_failed // a declined or failed auto top-up turned it off
};
std::string_view to_string(BalanceCrossing crossing);
// The email for a crossing: the balance, how much to add and the Add funds link. Plain text with
// nothing a person typed in it (no names), from the Resend settings' sender.
Email balance_email(BalanceCrossing crossing,
                    const BalanceView& view,
                    std::string_view link,
                    const std::vector<std::string>& to);
// The email telling an account Orama gave it a credit (lake#1134): the amount, the balance after,
// the expiry (empty: none), that it is spent before paid money and is not cash, and a link to the
// Billing page. Plain text with nothing a person typed in it: the grant's reason is shown on the
// Billing page, never mailed.
Email balance_grant_email(std::int64_t amount_micros,
                          std::string_view expires_at,
                          const BalanceView& view,
                          bool paid_before,
                          std::string_view link,
                          const std::vector<std::string>& to);
// Where an account's balance emails go: its billing contact's email, else its owner's (the
// person's own, or an organization owner's). Empty when it has none.
std::string balance_email_recipient(control::Store& store, const std::string& customer_id);

// One refresh of every account's charges (or only `customer_id`'s), by the store root's owner:
// each month from the first top-up's or the first grant's to this one at the list in force, read
// through the usage capabilities without auditing each read; a month an invoice froze stays. A
// failed read holds the balance and records why. After a full read, the grants past their expiry
// end (their unused part stops counting, lake#1134). Then the crossings' emails and auto top-ups.
// `context` acts as the operator, with the registry and workspaces set.
struct BalanceRefreshReport {
    int accounts = 0;
    int refreshed = 0;
    int held = 0; // a usage read failed; the balance was kept as it was
    int emails = 0;
    int email_failures = 0;
    int auto_top_ups = 0;
    int auto_top_up_failures = 0;
    int grants_expired = 0; // grants from Orama past their expiry, ended after a full read
};
liquid::runtime::task<BalanceRefreshReport>
refresh_account_balances(ApiContext& context,
                         std::shared_ptr<PaymentProviders> providers,
                         const BalanceRuntime& runtime,
                         std::string customer_id = {});

// account_balance, start_top_up, set_auto_top_up, set_balance_overdraft,
// refresh_account_balance, grant_balance_credit and revoke_balance_credit, registered by
// register_billing_capabilities where billing is on.
void register_balance_capabilities(Registry& registry, std::shared_ptr<PaymentProviders> providers);

} // namespace lake::api
