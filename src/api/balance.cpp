// The prepaid account balance (lake#1114, liquid-bridge decision 67, docs/api/billing.md#balance):
// the balance's view and its 402 gate, the warning every answer carries, the payment provider's
// notices (a top-up paid, a card saved, an auto top-up that failed), the refresh of the charges by
// the store root's owner with its emails and auto top-ups, and the capabilities account_balance,
// start_top_up, set_auto_top_up, set_balance_overdraft, refresh_account_balance, and the grants
// from Orama (lake#1134) grant_balance_credit and revoke_balance_credit. Registered with billing
// only (LAKE_BILLING): with billing off nothing here runs.

#include "lake/api/balance.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/audit/audit.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

constexpr std::int64_t micros_per_dollar = 1'000'000;
constexpr std::int64_t largest_top_up_micros = 10'000 * micros_per_dollar;

// "$12.34", "-$52.10".
std::string usd(std::int64_t micros) {
    const bool negative = micros < 0;
    // Cents, rounded half away from zero.
    const std::int64_t magnitude = negative ? -micros : micros;
    const std::int64_t cents = (magnitude + 5'000) / 10'000;
    return std::format("{}${}.{:02}", negative ? "-" : "", cents / 100, cents % 100);
}

// Whole dollars at or above `micros`.
std::int64_t whole_dollars_up(std::int64_t micros) {
    if (micros <= 0) {
        return 0;
    }
    return (micros + micros_per_dollar - 1) / micros_per_dollar * micros_per_dollar;
}

std::int64_t cents_of(std::int64_t micros) {
    return (micros + 9'999) / 10'000;
}

std::string now_text() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(now));
}

std::string today_text() {
    return std::format("{:%F}",
                       std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
}

std::string month_text(std::chrono::sys_days day) {
    return std::format("{:%Y-%m}", day);
}

// The months from the one `first` (RFC 3339) names to this one, oldest first.
std::vector<std::string> months_since(const std::string& first) {
    std::vector<std::string> out;
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    int year = 0;
    unsigned month = 0;
    if (first.size() < 7 ||
        std::from_chars(first.data(), first.data() + 4, year).ec != std::errc{} ||
        std::from_chars(first.data() + 5, first.data() + 7, month).ec != std::errc{} || month < 1 ||
        month > 12) {
        out.push_back(month_text(std::chrono::sys_days{today.year() / today.month() / 1}));
        return out;
    }
    std::chrono::year_month at{std::chrono::year{year}, std::chrono::month{month}};
    const std::chrono::year_month end = today.year() / today.month();
    while (at <= end && out.size() < 240) {
        out.push_back(month_text(std::chrono::sys_days{at / 1}));
        at += std::chrono::months{1};
    }
    return out;
}

// When a month's charge is final: a refresh read it after the month ended (an hour's margin for
// usage flushed late). Before then it is read again on every refresh.
bool month_is_final(const control::BalanceCharge& charge) {
    int year = 0;
    unsigned month = 0;
    if (charge.period.size() != 7 ||
        std::from_chars(charge.period.data(), charge.period.data() + 4, year).ec != std::errc{} ||
        std::from_chars(charge.period.data() + 5, charge.period.data() + 7, month).ec !=
            std::errc{}) {
        return false;
    }
    const std::chrono::year_month next =
        std::chrono::year_month{std::chrono::year{year}, std::chrono::month{month}} +
        std::chrono::months{1};
    const std::string final_after =
        std::format("{:%FT%T}Z", std::chrono::sys_days{next / 1} + std::chrono::hours{1});
    return !charge.refreshed_at.empty() && charge.refreshed_at >= final_after;
}

// A whole-second UTC time, "YYYY-MM-DDTHH:MM:SSZ", or nullopt when `text` is not one.
std::optional<std::chrono::sys_seconds> utc_seconds(std::string_view text) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    const auto field = [&](std::size_t at, std::size_t width, int& out) {
        const char* first = text.data() + at;
        const auto parsed = std::from_chars(first, first + width, out);
        return parsed.ec == std::errc{} && parsed.ptr == first + width;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!field(0, 4, year) || !field(5, 2, month) || !field(8, 2, day) || !field(11, 2, hour) ||
        !field(14, 2, minute) || !field(17, 2, second) || month < 1 || day < 1 || hour > 23 ||
        minute > 59 || second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
           std::chrono::seconds{second};
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable", "no control-plane store is configured");
}

ApiResult from_store(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
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

std::expected<std::string, ApiError> administrator_only(const json&, ApiContext&) {
    return std::unexpected(
        ApiError{"unauthorized", "the operator or the instance administrator is required", {}});
}

bool instance_administrator(const ApiContext& context) {
    return context.operator_credential || context.acting_identity == "admin";
}

void audit(ApiContext& context, const std::string& customer_id, std::string kind, json details) {
    context.audit_customer_id = customer_id;
    context.audit_kind = std::move(kind);
    context.audit_object = "customer/" + customer_id + "/billing";
    context.audit_details = std::move(details);
}

// A central audit event outside a capability call (the provider's notices, the refresh pass).
bool record(ApiContext& context,
            const std::string& customer_id,
            std::string kind,
            std::string principal,
            std::string action,
            std::string outcome,
            json details) {
    if (context.store == nullptr) {
        return false;
    }
    const audit::EventDraft event{.customer_id = customer_id,
                                  .kind = std::move(kind),
                                  .principal = std::move(principal),
                                  .object = "customer/" + customer_id + "/billing",
                                  .action = std::move(action),
                                  .outcome = std::move(outcome),
                                  .request_id = context.request_id,
                                  .method = context.authentication_method,
                                  .device = {},
                                  .details = std::move(details)};
    const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                         .instance_kek = context.instance_kek};
    return audit::record(*context.store, event, options).has_value();
}

// Changes the account's balance row by `change` against the version read, again on a conflict;
// `change` answers false to leave it. The row after, or the store's error.
control::Result<control::AccountBalance>
change_balance(control::Store& store,
               const std::string& customer_id,
               const std::function<bool(control::AccountBalance&)>& change) {
    for (int attempt = 0;; ++attempt) {
        auto current = store.account_balance(customer_id);
        if (!current) {
            return current;
        }
        control::AccountBalance next = *current;
        if (!change(next)) {
            return current;
        }
        auto written = store.update_account_balance(next, current->version);
        if (written || written.error().code != "conflict" || attempt >= 4) {
            return written;
        }
    }
}

// The account's exemption from the gate and the nag: a paid cloud order covers it.
std::string account_exemption(const control::AccountBalance& balance) {
    return balance.paid_order ? "paid_order" : std::string();
}

std::string link_origin(const BalanceRuntime& runtime, const ApiContext& context) {
    if (!runtime.origin.empty()) {
        return runtime.origin;
    }
    if (!context.external_origin.empty()) {
        return context.external_origin;
    }
    if (!context.public_host.empty()) {
        return "https://" + context.public_host;
    }
    // A Lake without a configured origin (local development): the request's own.
    return context.request_origin;
}

json auto_top_up_json(const control::AccountBalance& balance) {
    return {{"enabled", balance.auto_top_up},
            {"threshold_micros", balance.auto_top_up_threshold_micros},
            {"amount_micros", balance.auto_top_up_amount_micros},
            {"card_saved", !balance.auto_top_up_payment_method.empty()},
            {"awaiting_card", balance.auto_top_up_pending == "card_setup"},
            {"in_flight",
             !balance.auto_top_up_pending.empty() && balance.auto_top_up_pending != "card_setup"},
            {"today", balance.auto_top_up_day == today_text() ? balance.auto_top_ups_today : 0},
            {"off_reason", balance.auto_top_up_off_reason}};
}

json balance_schema() {
    return {{"type", "object"},
            {"properties",
             {{"customer_id", {{"type", "string"}}},
              {"currency", {{"type", "string"}}},
              {"balance_micros", {{"type", "integer"}}},
              {"credits_micros", {{"type", "integer"}}},
              {"charges_micros", {{"type", "integer"}}},
              {"overdraft_micros", {{"type", "integer"}}},
              {"limit_micros", {{"type", "integer"}}},
              {"state", {{"type", "string"}, {"enum", {"ok", "low", "negative", "blocked"}}}},
              {"reason", {{"type", "string"}}},
              {"add_micros", {{"type", "integer"}}},
              {"add_funds_url", {{"type", "string"}}},
              {"first_top_up_micros", {{"type", "integer"}}},
              {"minimum_top_up_micros", {{"type", "integer"}}},
              {"first_top_up_at", {{"type", "string"}}},
              {"first_grant_at", {{"type", "string"}}},
              {"grants_unused_micros", {{"type", "integer"}}},
              {"exempt", {{"type", "string"}}},
              {"refreshed_at", {{"type", "string"}}},
              {"refresh_error", {{"type", "string"}}},
              {"auto_top_up", {{"type", "object"}}}}},
            {"required",
             json::array({"customer_id",
                          "currency",
                          "balance_micros",
                          "overdraft_micros",
                          "limit_micros",
                          "state",
                          "add_micros",
                          "add_funds_url"})}};
}

json object_schema(json properties, json required) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json amount_schema(std::string description) {
    return {{"type", "integer"},
            {"minimum", 0},
            {"maximum", largest_top_up_micros},
            {"description", std::move(description)}};
}

// The account's own customer row, or not_found at /customer_id.
std::expected<control::Customer, ApiResult> account_named(ApiContext& context,
                                                          const std::string& customer_id) {
    const auto customer = context.store->get_customer(customer_id);
    if (!customer) {
        return std::unexpected(
            ApiResult::failure("not_found", "no account " + customer_id, "/customer_id"));
    }
    return *customer;
}

// The payment provider in use now; a provider without card payment refuses its checkouts.
liquid::runtime::task<std::expected<std::shared_ptr<PaymentProvider>, ApiResult>>
card_provider(PaymentProviders& providers, ApiContext& context) {
    using Answer = std::expected<std::shared_ptr<PaymentProvider>, ApiResult>;
    auto current = co_await current_payment_provider(providers, context);
    if (!current) {
        co_return Answer(std::unexpect, ApiResult::failure("store_error", current.error().message));
    }
    if (!*current) {
        co_return Answer(std::unexpect, current->error());
    }
    co_return Answer(std::move(**current));
}

} // namespace

std::expected<BalanceSettings, std::string>
balance_settings_from_environment(std::string_view first_top_up,
                                  std::string_view minimum_top_up,
                                  std::string_view overdraft,
                                  std::string_view warning,
                                  std::string_view refresh_seconds,
                                  std::string_view auto_top_ups_per_day) {
    BalanceSettings settings;
    const auto whole = [](std::string_view text,
                          std::string_view name,
                          std::int64_t lowest,
                          std::int64_t highest,
                          std::int64_t& out) -> std::expected<void, std::string> {
        if (text.empty()) {
            return {};
        }
        std::int64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < lowest ||
            value > highest) {
            return std::unexpected(
                std::format("{} must be a whole number from {} to {}", name, lowest, highest));
        }
        out = value;
        return {};
    };
    std::int64_t first = 20, minimum = 20, limit = 50, near = 10, refresh = 300, daily = 5;
    for (const auto& check :
         {whole(first_top_up, "LAKE_BALANCE_FIRST_TOP_UP_USD", 1, 10'000, first),
          whole(minimum_top_up, "LAKE_BALANCE_MINIMUM_TOP_UP_USD", 1, 10'000, minimum),
          whole(overdraft, "LAKE_BALANCE_OVERDRAFT_USD", 0, 1'000'000, limit),
          whole(warning, "LAKE_BALANCE_WARNING_USD", 0, 1'000'000, near),
          whole(refresh_seconds, "LAKE_BALANCE_REFRESH_SECONDS", 30, 86'400, refresh),
          whole(auto_top_ups_per_day, "LAKE_AUTO_TOP_UPS_PER_DAY", 1, 100, daily)}) {
        if (!check) {
            return std::unexpected(check.error());
        }
    }
    settings.first_top_up_micros = first * micros_per_dollar;
    settings.minimum_top_up_micros = minimum * micros_per_dollar;
    settings.overdraft_micros = limit * micros_per_dollar;
    settings.warning_micros = near * micros_per_dollar;
    settings.refresh_interval = std::chrono::seconds{refresh};
    settings.auto_top_ups_per_day = static_cast<int>(daily);
    settings.auto_top_up_amount_micros =
        std::max(settings.auto_top_up_amount_micros, settings.minimum_top_up_micros);
    return settings;
}

BalanceView balance_view(const control::AccountBalance& balance,
                         const BalanceSettings& settings,
                         std::string exempt) {
    BalanceView view;
    view.balance_micros = balance.balance_micros();
    view.overdraft_micros = balance.overdraft_limit_micros.value_or(settings.overdraft_micros);
    view.exempt = std::move(exempt);
    const auto to_safety = [&] {
        return std::max(settings.minimum_top_up_micros,
                        whole_dollars_up(settings.warning_micros - view.balance_micros));
    };
    if (!view.exempt.empty()) {
        view.add_micros = settings.minimum_top_up_micros;
        return view;
    }
    if (balance.first_top_up_at.empty()) {
        // A new account starts at $0 and adds the first top-up before anything that costs money:
        // the overdraft is credit extended only to accounts that have paid once (lake#1114). A
        // grant from Orama is spent down to $0 and never past it (lake#1134).
        view.overdraft_micros = 0;
        view.add_micros = settings.first_top_up_micros;
        if (view.balance_micros <= 0) {
            view.state = "blocked";
            view.reason = "first_top_up";
        } else if (view.balance_micros < settings.warning_micros) {
            view.state = "low";
        }
        return view;
    }
    if (view.balance_micros <= -view.overdraft_micros) {
        view.state = "blocked";
        view.reason = "overdraft";
    } else if (view.balance_micros < 0) {
        view.state = "negative";
    } else if (view.balance_micros < settings.warning_micros) {
        view.state = "low";
    }
    view.add_micros = view.state == "ok" ? settings.minimum_top_up_micros : to_safety();
    return view;
}

std::string add_funds_link(std::string_view origin, std::string_view customer_id) {
    return std::format("{}/billing/add-funds?customer_id={}", origin, customer_id);
}

json balance_json(const control::AccountBalance& balance,
                  const BalanceView& view,
                  const BalanceRuntime& runtime) {
    return {{"customer_id", balance.customer_id},
            {"currency", "USD"},
            {"balance_micros", view.balance_micros},
            {"credits_micros", balance.credits_micros},
            {"charges_micros", balance.charges_micros},
            {"overdraft_micros", view.overdraft_micros},
            {"limit_micros", -view.overdraft_micros},
            {"state", view.state},
            {"reason", view.reason},
            {"add_micros", view.add_micros},
            {"add_funds_url", add_funds_link(runtime.origin, balance.customer_id)},
            {"first_top_up_micros", runtime.settings.first_top_up_micros},
            {"minimum_top_up_micros", runtime.settings.minimum_top_up_micros},
            {"first_top_up_at", balance.first_top_up_at},
            {"first_grant_at", balance.first_grant_at},
            {"grants_unused_micros", balance.grants_unused_micros},
            {"exempt", view.exempt},
            {"refreshed_at", balance.charges_refreshed_at},
            {"refresh_error", balance.refresh_error},
            {"auto_top_up", auto_top_up_json(balance)}};
}

std::span<const PaidWork> paid_work_capabilities() {
    // The one list (docs/api/billing.md#the-gate). PaidWorkContract.* fails when a capability whose
    // name says it creates, starts or grows something is neither here nor classified as free.
    static constexpr std::array<PaidWork, 51> list{{
        {"create_workspace", "starts a workspace engine, billed by its CPU, RAM and volume"},
        {"start_workspace", "starts a stopped workspace engine"},
        {"update_workspace", "starts a stopped workspace engine (`desired: running`)"},
        {"create_app", "creates an app that runs on billed compute"},
        {"deploy_app", "deploys an app"},
        {"start_app", "starts an app"},
        {"restart_app", "starts an app again"},
        {"scale_app", "adds an app's replicas"},
        {"rollback_app", "deploys an earlier release"},
        {"build_app", "builds an app's image on billed compute"},
        {"build_image", "builds an image on billed compute"},
        {"create_service", "creates a managed service and its volume"},
        {"create_service_and_bind_to_app", "creates a managed service"},
        {"connect_service", "creates a connection that runs a workload"},
        {"create_store", "creates a store and its billed volume"},
        {"set_workspace_storage", "grows a workspace volume"},
        {"train_model", "trains a model on billed compute"},
        {"retrain_model", "trains a model again"},
        {"register_model", "stores a model"},
        {"load_drive_model", "stores a model from a drive"},
        {"expose_model", "serves a model"},
        {"create_index", "builds a search index"},
        {"expose_index", "serves a search index"},
        {"start_run", "starts a run"},
        {"run_pipeline", "runs a pipeline"},
        {"deploy_pipeline", "deploys a pipeline"},
        {"run_solution", "runs a solution"},
        {"start_solution", "starts a solution"},
        {"run_methodology", "runs a methodology in its sandbox"},
        {"validate_methodology", "builds a methodology's image and runs it on a sample"},
        {"start_experiment", "starts an experiment's trials"},
        {"launch_agent", "launches an agent's job"},
        {"schedule_agent", "schedules an agent's runs"},
        {"agent_run", "runs an agent"},
        {"agent_turn", "starts an agent turn, billed in model tokens"},
        {"chat_with_agent", "starts an agent turn"},
        {"send_ask_message", "starts an Ask Orama turn"},
        {"create_mcp_connection_agent", "starts a connection's agent"},
        {"model_completion", "calls a model through the gateway, billed in tokens"},
        {"transcribe", "transcribes audio, billed by the minute"},
        {"create_transcript", "transcribes a recording"},
        {"upload_dataset", "stores a dataset"},
        {"generate_dataset", "generates and stores a dataset"},
        {"create_dataset_from_query", "stores a dataset"},
        {"load_drive_dataset", "stores a dataset from a drive"},
        {"begin_drive_upload", "starts storing a file"},
        {"upload_drive_files", "stores files"},
        {"upload_drive_entries", "stores files"},
        {"import_repository", "stores a repository"},
        {"import_github_repository", "stores a repository"},
        {"push", "grows a repository's storage"},
    }};
    return list;
}

bool starts_paid_work(std::string_view capability) {
    return std::ranges::any_of(paid_work_capabilities(),
                               [&](const PaidWork& work) { return work.capability == capability; });
}

std::string balance_caller_exemption(const ApiContext& context) {
    if (context.operator_credential) {
        return "operator";
    }
    if (context.acting_identity == "admin") {
        return "administrator";
    }
    return {};
}

std::string balance_account_of(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        return {};
    }
    const auto text = [&](const char* field) {
        return input.is_object() && input.contains(field) && input.at(field).is_string()
                   ? input.at(field).get<std::string>()
                   : std::string();
    };
    if (!context.audit_validated_workspace.second.empty()) {
        return context.audit_validated_workspace.second;
    }
    for (const std::string& workspace_id : {text("workspace_id"), context.path_workspace_id}) {
        if (!workspace_id.empty()) {
            if (const auto workspace = context.store->get_workspace(workspace_id)) {
                return workspace->customer_id;
            }
        }
    }
    if (const std::string customer_id = text("customer_id"); !customer_id.empty()) {
        if (const auto customer = context.store->get_customer(customer_id)) {
            return customer->id;
        }
    }
    if (const std::string repository_id = text("repository_id"); !repository_id.empty()) {
        if (const auto repository = context.store->get_repository(repository_id)) {
            return repository->customer_id;
        }
    }
    if (const std::string organization_id = text("organization_id"); !organization_id.empty()) {
        if (const auto customer = context.store->organization_customer(organization_id)) {
            return customer->id;
        }
    }
    if (!context.credential_customer_id.empty()) {
        return context.credential_customer_id;
    }
    if (!context.acting_user_id.empty()) {
        if (const auto customers = context.store->list_customers(20, context.acting_user_id)) {
            for (const control::Customer& customer : *customers) {
                if (customer.customer_type == "individual" &&
                    customer.user_id == context.acting_user_id) {
                    return customer.id;
                }
            }
        }
    }
    return {};
}

// A marked capability whose call, with this input, starts nothing that costs money (lake#1122): an
// account must always be able to stop or rename what it has, so only `update_workspace` with
// `desired: running` starts paid work; stopping or renaming a workspace is never refused.
bool call_starts_paid_work(const Capability& capability, const json& input) {
    if (capability.name == "update_workspace") {
        return input.is_object() && input.value("desired", std::string()) == "running";
    }
    return true;
}

std::optional<ApiResult>
balance_refusal(const Capability& capability, const json& input, ApiContext& context) {
    if (!call_starts_paid_work(capability, input)) {
        return std::nullopt;
    }
    if (!context.balance || context.store == nullptr ||
        !balance_caller_exemption(context).empty()) {
        return std::nullopt;
    }
    const std::string customer_id = balance_account_of(input, context);
    if (customer_id.empty()) {
        return std::nullopt;
    }
    const auto balance = context.store->account_balance(customer_id);
    if (!balance) {
        return ApiResult::failure("store_unavailable",
                                  "the account's balance could not be read; try again");
    }
    const BalanceView view =
        balance_view(*balance, context.balance->settings, account_exemption(*balance));
    if (view.state != "blocked") {
        return std::nullopt;
    }
    const std::string link = add_funds_link(link_origin(*context.balance, context), customer_id);
    const std::string message =
        view.reason == "first_top_up" && !balance->first_grant_at.empty()
            ? std::format("{} costs money, and this account's credit from Orama is used up. Add "
                          "{} to go on: {}",
                          capability.name,
                          usd(view.add_micros),
                          link)
        : view.reason == "first_top_up"
            ? std::format("{} costs money, and this account has not added funds yet. Add {} to "
                          "start: {}",
                          capability.name,
                          usd(view.add_micros),
                          link)
            : std::format("{} costs money, and this account's balance ({}) is past its {} "
                          "overdraft limit. Add {} to go on: {}. Reading, stopping and deleting "
                          "still work.",
                          capability.name,
                          usd(view.balance_micros),
                          usd(view.overdraft_micros),
                          usd(view.add_micros),
                          link);
    ApiResult refused = ApiResult::failure("payment_required", message);
    refused.error.details = {{"customer_id", customer_id},
                             {"balance_micros", view.balance_micros},
                             {"overdraft_micros", view.overdraft_micros},
                             {"add_micros", view.add_micros},
                             {"add_funds_url", link},
                             {"reason", view.reason}};
    return refused;
}

json balance_warning(const json& input, ApiContext& context) {
    if (!context.balance || context.store == nullptr ||
        !balance_caller_exemption(context).empty() || !context.authorized) {
        return nullptr;
    }
    const std::string customer_id = balance_account_of(input, context);
    if (customer_id.empty()) {
        return nullptr;
    }
    const auto balance = context.store->account_balance(customer_id);
    if (!balance) {
        return nullptr;
    }
    const BalanceView view =
        balance_view(*balance, context.balance->settings, account_exemption(*balance));
    if (view.state == "ok") {
        return nullptr;
    }
    const std::string link = add_funds_link(link_origin(*context.balance, context), customer_id);
    std::string message;
    if (view.reason == "first_top_up" && !balance->first_grant_at.empty()) {
        message = std::format("This account's credit from Orama is used up. Add {} to start "
                              "anything that costs money again: {}",
                              usd(view.add_micros),
                              link);
    } else if (view.reason == "first_top_up") {
        message = std::format("Add {} to this account's balance to start anything that costs "
                              "money: {}",
                              usd(view.add_micros),
                              link);
    } else if (view.state == "blocked") {
        message = std::format("This account's balance is {}, past its {} overdraft limit: "
                              "nothing that costs money starts until you add {}: {}",
                              usd(view.balance_micros),
                              usd(view.overdraft_micros),
                              usd(view.add_micros),
                              link);
    } else if (view.state == "negative") {
        message = std::format("This account's balance is {}. At -{} nothing that costs money "
                              "starts; add {}: {}",
                              usd(view.balance_micros),
                              usd(view.overdraft_micros),
                              usd(view.add_micros),
                              link);
    } else {
        message = std::format("This account's balance is low ({}). Add {}: {}",
                              usd(view.balance_micros),
                              usd(view.add_micros),
                              link);
    }
    return {{"code", "balance_" + view.state},
            {"message", std::move(message)},
            {"customer_id", customer_id},
            {"balance_micros", view.balance_micros},
            {"add_micros", view.add_micros},
            {"add_funds_url", link}};
}

NoticeOutcome
apply_balance_notice(ApiContext& context, std::string_view provider, const PaymentNotice& notice) {
    const auto answer = [](int status, std::string code, std::string message) {
        return NoticeOutcome{status, {{"error", {{"code", code}, {"message", message}}}}};
    };
    const PaymentNotice::Balance& about = *notice.balance;
    const std::string name(provider);
    if (context.store == nullptr) {
        return answer(503, "store_unavailable", "no control-plane store is configured");
    }
    if (about.customer_id.empty() || !context.store->get_customer(about.customer_id)) {
        // Tagged as this Lake's but naming no account it has (a deleted account, say).
        return NoticeOutcome{200, {{"received", true}, {"matched", false}}};
    }
    json details{{"provider", name},
                 {"event_id", notice.event_id},
                 {"event_type", notice.event_type},
                 {"reference", about.reference}};
    if (about.kind == "credit") {
        if (about.amount_cents <= 0) {
            return answer(400, "invalid_input", "the payment has no amount");
        }
        control::BalanceCredit credit;
        credit.customer_id = about.customer_id;
        credit.kind = about.credit_kind == "auto_top_up" ? "auto_top_up" : "top_up";
        credit.amount_micros = about.amount_cents * 10'000;
        credit.provider = name;
        credit.reference = about.reference;
        credit.event_id = notice.event_id;
        const auto applied = context.store->apply_balance_credit(credit);
        if (!applied) {
            return answer(applied.error().code == "invalid_input" ? 400 : 503,
                          applied.error().code,
                          applied.error().message);
        }
        if (applied->duplicate) {
            return NoticeOutcome{200, {{"received", true}, {"duplicate", true}}};
        }
        details["kind"] = credit.kind;
        details["amount_micros"] = credit.amount_micros;
        details["balance_micros"] = applied->balance.balance_micros();
        if (!record(context,
                    about.customer_id,
                    "billing.balance.credit",
                    "payment-provider",
                    "payment_notice",
                    "success",
                    std::move(details))) {
            return answer(503, "store_unavailable", "Lake could not record the notice");
        }
        return NoticeOutcome{200,
                             {{"received", true},
                              {"credited", true},
                              {"customer_id", about.customer_id},
                              {"amount_micros", credit.amount_micros},
                              {"balance_micros", applied->balance.balance_micros()}}};
    }
    bool changed = false;
    const auto stored =
        change_balance(*context.store, about.customer_id, [&](control::AccountBalance& balance) {
            if (about.kind == "card_saved") {
                // Only a card asked for by set_auto_top_up turns it on; a replayed event finds
                // nothing waiting and changes nothing.
                if (balance.auto_top_up_pending != "card_setup" || about.payment_method.empty()) {
                    return false;
                }
                balance.auto_top_up = true;
                balance.auto_top_up_payment_method = about.payment_method;
                balance.auto_top_up_pending.clear();
                balance.auto_top_up_off_reason.clear();
                balance.auto_top_up_failure_notified_at.clear();
            } else if (about.kind == "auto_top_up_failed") {
                if (!balance.auto_top_up && balance.auto_top_up_pending.empty()) {
                    return false;
                }
                balance.auto_top_up = false;
                balance.auto_top_up_pending.clear();
                balance.auto_top_up_started_at.clear();
                balance.auto_top_up_off_reason = control::utf8_prefix(
                    about.message.empty() ? "the card was declined" : about.message, 1000);
                balance.auto_top_up_failure_notified_at.clear();
            } else {
                return false;
            }
            changed = true;
            return true;
        });
    if (!stored) {
        return answer(503, stored.error().code, stored.error().message);
    }
    if (!changed) {
        return NoticeOutcome{200, {{"received", true}, {"duplicate", true}}};
    }
    if (about.kind == "auto_top_up_failed") {
        details["message"] = stored->auto_top_up_off_reason;
    }
    if (!record(context,
                about.customer_id,
                about.kind == "card_saved" ? "billing.balance.auto_top_up_on"
                                           : "billing.balance.auto_top_up_failed",
                "payment-provider",
                "payment_notice",
                about.kind == "card_saved" ? "success" : "failure",
                std::move(details))) {
        return answer(503, "store_unavailable", "Lake could not record the notice");
    }
    return NoticeOutcome{200,
                         {{"received", true},
                          {"customer_id", about.customer_id},
                          {"auto_top_up", stored->auto_top_up}}};
}

std::string_view to_string(BalanceCrossing crossing) {
    switch (crossing) {
    case BalanceCrossing::limit_warning:
        return "limit_warning";
    case BalanceCrossing::negative:
        return "negative";
    case BalanceCrossing::blocked:
        return "blocked";
    case BalanceCrossing::auto_top_up_failed:
        return "auto_top_up_failed";
    }
    return "unknown";
}

Email balance_email(BalanceCrossing crossing,
                    const BalanceView& view,
                    std::string_view link,
                    const std::vector<std::string>& to) {
    Email email;
    email.to = to;
    const std::string balance = usd(view.balance_micros);
    const std::string limit = usd(-view.overdraft_micros);
    const std::string add = usd(view.add_micros);
    std::string opening;
    switch (crossing) {
    case BalanceCrossing::limit_warning:
        email.subject = "Your Lake balance is close to its limit";
        opening = std::format("Your Lake account's balance is {}. At {} Lake stops starting "
                              "anything that costs money (apps, services, runs, training, agents "
                              "and model calls). What is already running keeps running.",
                              balance,
                              limit);
        break;
    case BalanceCrossing::negative:
        email.subject = "Your Lake balance is below $0";
        opening = std::format("Your Lake account's balance is {}: your usage has passed what you "
                              "added. Lake keeps working until the balance reaches {}, then stops "
                              "starting anything that costs money.",
                              balance,
                              limit);
        break;
    case BalanceCrossing::blocked:
        email.subject = "Lake has stopped starting paid work on your account";
        opening = std::format("Your Lake account's balance is {}, at its {} limit. Until you add "
                              "funds, Lake refuses anything that costs money. Reading, signing "
                              "in, stopping and deleting still work, and what is already running "
                              "keeps running.",
                              balance,
                              limit);
        break;
    case BalanceCrossing::auto_top_up_failed:
        email.subject = "Your Lake auto top-up was turned off";
        opening = std::format("Lake could not charge your saved card for an auto top-up, so auto "
                              "top-up is now off. Your balance is {}. Add funds yourself, and "
                              "turn auto top-up on again from the Billing page when your card "
                              "works.",
                              balance);
        break;
    }
    email.text = std::format("{}\n\nAdd {} or more here:\n{}\n\nThis email is sent once each time "
                             "your balance crosses this point, and not again until you add "
                             "funds.\n\nLake\n",
                             opening,
                             add,
                             link);
    return email;
}

namespace {

// "2026-12-31 23:59 UTC" from "2026-12-31T23:59:59Z".
std::string utc_minute(std::string_view at) {
    if (at.size() < 16) {
        return std::string(at);
    }
    return std::format("{} {} UTC", at.substr(0, 10), at.substr(11, 5));
}

} // namespace

Email balance_grant_email(std::int64_t amount_micros,
                          std::string_view expires_at,
                          const BalanceView& view,
                          bool paid_before,
                          std::string_view link,
                          const std::vector<std::string>& to) {
    Email email;
    email.to = to;
    email.subject =
        std::format("Orama added {} of credit to your Lake account", usd(amount_micros));
    const std::string expiry =
        expires_at.empty()
            ? std::string("It does not expire.")
            : std::format("Whatever is left of it on {} stops counting then; what it paid for "
                          "stays paid.",
                          utc_minute(expires_at));
    const std::string after =
        paid_before ? std::string()
                    : std::format("\n\nWhen the credit is used up, Lake stops starting anything "
                                  "that costs money until you add your first top-up of {}.",
                                  usd(view.add_micros));
    email.text = std::format("Orama added a credit of {} to your Lake account. Your balance is now "
                             "{}.\n\nThe credit is used before any money you add. {} It is a "
                             "credit, not cash: it is never refunded or paid out.{}\n\nSee your "
                             "balance here:\n{}\n\nLake\n",
                             usd(amount_micros),
                             usd(view.balance_micros),
                             expiry,
                             after,
                             link);
    return email;
}

std::string balance_email_recipient(control::Store& store, const std::string& customer_id) {
    if (const auto contact = store.billing_contact(customer_id); contact && *contact) {
        return (*contact)->email;
    }
    const auto customer = store.get_customer(customer_id);
    if (!customer) {
        return {};
    }
    const auto email_of = [&](const std::string& user_id) -> std::string {
        const auto user = store.get_user(user_id);
        return user && user->may_act() && plausible_email_address(user->email) ? user->email
                                                                               : std::string();
    };
    if (!customer->user_id.empty()) {
        return email_of(customer->user_id);
    }
    if (!customer->organization_id.empty()) {
        if (const auto members = store.list_memberships(customer->organization_id, "", 200)) {
            for (const control::Membership& member : *members) {
                if (member.role == "owner" && member.principal_type == "user") {
                    if (std::string email = email_of(member.principal_id); !email.empty()) {
                        return email;
                    }
                }
            }
        }
    }
    return {};
}

namespace {

// The charges of one account, month by month from its first top-up's: a month an invoice froze,
// or one already read after it ended, stays. Answers why the usage could not be read, or empty.
liquid::runtime::task<std::string> refresh_charges(ApiContext& context,
                                                   const control::AccountBalance& balance) {
    const auto kept = context.store->balance_charges(balance.customer_id);
    if (!kept) {
        co_return kept.error().message;
    }
    std::vector<control::BalanceCharge> fresh;
    for (const std::string& period : months_since(balance.charges_since())) {
        const auto found = std::ranges::find(*kept, period, &control::BalanceCharge::period);
        if (found != kept->end() && (!found->invoice_id.empty() || month_is_final(*found))) {
            continue;
        }
        auto bill = co_await account_month_bill(context, balance.customer_id, period, false);
        if (!bill) {
            co_return bill.error().message;
        }
        if (!*bill) {
            const ApiResult& failure = bill->error();
            if (failure.error.code == "conflict" &&
                failure.error.message.starts_with("no price list is published")) {
                // Nothing is billed for a month without a list.
                fresh.push_back({balance.customer_id, period, 0, {}, {}, {}});
                continue;
            }
            co_return failure.error.message;
        }
        if (!(*bill)->complete) {
            co_return "the usage of workspace " + (*bill)->unread + " could not be read";
        }
        fresh.push_back({balance.customer_id,
                         period,
                         static_cast<std::int64_t>(std::min<std::uint64_t>(
                             (*bill)->total_micros,
                             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
                         (*bill)->price_list_id,
                         {},
                         {}});
    }
    // Every month was read: only now is any written, so a failure holds the whole balance.
    for (const control::BalanceCharge& charge : fresh) {
        if (const auto stored = context.store->set_balance_charge(charge); !stored) {
            co_return stored.error().message;
        }
    }
    co_return std::string();
}

// Ends the account's grants past their expiry (and `revoke_id`, when given) against its charges
// now, and audits each that expired centrally (a revocation is its capability's own audit).
control::Result<std::vector<control::BalanceCredit>> end_grants(ApiContext& context,
                                                                const std::string& customer_id,
                                                                const std::string& revoke_id,
                                                                std::string principal,
                                                                std::string action) {
    auto ended = context.store->end_balance_grants(customer_id, now_text(), revoke_id);
    if (!ended) {
        return ended;
    }
    for (const control::BalanceCredit& grant : *ended) {
        if (grant.end_reason != "expired") {
            continue;
        }
        static_cast<void>(record(context,
                                 customer_id,
                                 "billing.balance.credit_expired",
                                 principal,
                                 action,
                                 "success",
                                 {{"credit_id", grant.id},
                                  {"amount_micros", grant.amount_micros},
                                  {"used_micros", grant.amount_micros - grant.forfeited_micros},
                                  {"forfeited_micros", grant.forfeited_micros},
                                  {"expires_at", grant.expires_at}}));
    }
    return ended;
}

// One credit as account_balance lists it; a grant with its reason, expiry, what stopped counting
// and, while it counts, what it has left (`unused_micros`).
json credit_json(const control::BalanceCredit& credit, std::int64_t unused_micros) {
    json row{{"id", credit.id},
             {"kind", credit.kind},
             {"label",
              credit.kind == "grant"         ? "Credit from Orama"
              : credit.kind == "auto_top_up" ? "Auto top-up"
                                             : "Top-up"},
             {"amount_micros", credit.amount_micros},
             {"provider", credit.provider},
             {"reference", credit.reference},
             {"created_at", credit.created_at}};
    if (credit.kind == "grant") {
        row["reason"] = credit.reason;
        row["expires_at"] = credit.expires_at;
        row["ended_at"] = credit.ended_at;
        row["end_reason"] = credit.end_reason;
        row["forfeited_micros"] = credit.forfeited_micros;
        row["counting_micros"] = credit.amount_micros - credit.forfeited_micros;
        row["unused_micros"] = credit.ended_at.empty() ? unused_micros : 0;
    }
    return row;
}

// What each of the account's grants has left, by id (grant_allocation over all of them).
std::map<std::string, std::int64_t> grants_left(control::Store& store,
                                                const control::AccountBalance& balance) {
    std::map<std::string, std::int64_t> out;
    if (balance.first_grant_at.empty()) {
        return out;
    }
    const auto credits = store.balance_credits(balance.customer_id, 0);
    if (!credits) {
        return out;
    }
    std::vector<control::BalanceCredit> grants;
    for (const control::BalanceCredit& credit : *credits) {
        if (credit.kind == "grant") {
            grants.push_back(credit);
        }
    }
    const std::vector<std::int64_t> used =
        control::grant_allocation(grants, balance.charges_micros);
    for (std::size_t index = 0; index < grants.size(); ++index) {
        out[grants[index].id] =
            std::max<std::int64_t>(grants[index].amount_micros - used[index], 0);
    }
    return out;
}

// The crossing's mark on the row.
std::string& notified(control::AccountBalance& balance, BalanceCrossing crossing) {
    switch (crossing) {
    case BalanceCrossing::limit_warning:
        return balance.limit_warning_notified_at;
    case BalanceCrossing::negative:
        return balance.negative_notified_at;
    case BalanceCrossing::blocked:
        return balance.blocked_notified_at;
    case BalanceCrossing::auto_top_up_failed:
        break;
    }
    return balance.auto_top_up_failure_notified_at;
}

// Sends the email of `crossing` once: marked first, so a refresh that overlaps never sends it
// twice, under a key per crossing and credit, and unmarked again when the send fails so the next
// refresh tries again. `also` are the milder crossings the same email covers.
void notify_once(ApiContext& context,
                 const BalanceRuntime& runtime,
                 const std::string& customer_id,
                 BalanceCrossing crossing,
                 std::vector<BalanceCrossing> also,
                 BalanceRefreshReport& report) {
    if (!runtime.mailer) {
        return;
    }
    const std::string to = balance_email_recipient(*context.store, customer_id);
    if (to.empty()) {
        return;
    }
    const std::string at = now_text();
    bool claimed = false;
    auto marked = change_balance(*context.store, customer_id, [&](control::AccountBalance& row) {
        if (!notified(row, crossing).empty()) {
            return false;
        }
        notified(row, crossing) = at;
        for (const BalanceCrossing milder : also) {
            if (notified(row, milder).empty()) {
                notified(row, milder) = at;
            }
        }
        claimed = true;
        return true;
    });
    if (!marked || !claimed) {
        return;
    }
    const BalanceView view = balance_view(*marked, runtime.settings, account_exemption(*marked));
    const Email email = balance_email(
        crossing, view, add_funds_link(link_origin(runtime, context), customer_id), {to});
    const std::string key =
        std::format("lake-balance-{}-{}-{}",
                    customer_id,
                    to_string(crossing),
                    crossing == BalanceCrossing::auto_top_up_failed ? marked->auto_top_up_sequence
                                                                    : marked->credit_count);
    const auto sent = runtime.mailer(email, key);
    if (sent) {
        ++report.emails;
        static_cast<void>(
            record(context,
                   customer_id,
                   "billing.balance.email",
                   "lake",
                   "balance_refresh",
                   "success",
                   {{"crossing", to_string(crossing)}, {"balance_micros", view.balance_micros}}));
        return;
    }
    ++report.email_failures;
    static_cast<void>(
        change_balance(*context.store, customer_id, [&](control::AccountBalance& row) {
            if (notified(row, crossing) != at) {
                return false;
            }
            notified(row, crossing).clear();
            for (const BalanceCrossing milder : also) {
                if (notified(row, milder) == at) {
                    notified(row, milder).clear();
                }
            }
            return true;
        }));
}

// Starts an auto top-up when the balance is under the account's threshold: at most one in flight,
// at most the daily limit, keyed per account and crossing so a refresh never charges twice. A
// failure turns auto top-up off; the refresh's email step tells the owner.
liquid::runtime::task<void> auto_top_up(ApiContext& context,
                                        const std::shared_ptr<PaymentProviders>& providers,
                                        const BalanceRuntime& runtime,
                                        const control::AccountBalance& balance,
                                        BalanceRefreshReport& report) {
    if (!balance.auto_top_up || balance.auto_top_up_payment_method.empty() || !providers) {
        co_return {};
    }
    if (!balance.auto_top_up_pending.empty()) {
        // A charge in flight is credited by its webhook; one that never answers is given up
        // after a day, past Stripe's idempotency window.
        if (balance.auto_top_up_started_at.empty() ||
            balance.auto_top_up_started_at >
                std::format("{:%FT%T}Z",
                            std::chrono::floor<std::chrono::seconds>(
                                std::chrono::system_clock::now() - std::chrono::hours{24}))) {
            co_return {};
        }
        static_cast<void>(
            change_balance(*context.store, balance.customer_id, [&](control::AccountBalance& row) {
                if (row.auto_top_up_pending != balance.auto_top_up_pending) {
                    return false;
                }
                row.auto_top_up_pending.clear();
                row.auto_top_up_started_at.clear();
                return true;
            }));
        co_return {};
    }
    if (balance.balance_micros() >= balance.auto_top_up_threshold_micros) {
        co_return {};
    }
    const std::string today = today_text();
    std::string key;
    auto claimed =
        change_balance(*context.store, balance.customer_id, [&](control::AccountBalance& row) {
            const int started = row.auto_top_up_day == today ? row.auto_top_ups_today : 0;
            if (!row.auto_top_up || !row.auto_top_up_pending.empty() ||
                started >= runtime.settings.auto_top_ups_per_day ||
                row.balance_micros() >= row.auto_top_up_threshold_micros) {
                return false;
            }
            ++row.auto_top_up_sequence;
            key = std::format("lake-auto-top-up-{}-{}", row.customer_id, row.auto_top_up_sequence);
            row.auto_top_up_pending = key;
            row.auto_top_up_started_at = now_text();
            row.auto_top_up_day = today;
            row.auto_top_ups_today = started + 1;
            return true;
        });
    if (!claimed || key.empty()) {
        co_return {};
    }
    std::string failure;
    std::string reference;
    auto provider = co_await card_provider(*providers, context);
    if (!provider) {
        failure = provider.error().message;
    } else if (!*provider) {
        failure = provider->error().error.message;
    } else {
        const auto customer = context.store->get_customer(balance.customer_id);
        if (!customer) {
            failure = customer.error().message;
        } else if (auto account = billing_payment_account(context, ***provider, *customer, 0);
                   !account) {
            failure = account.error().error.message;
        } else {
            auto charged =
                (**provider)
                    ->charge_saved_payment_method(*account,
                                                  {claimed->auto_top_up_payment_method,
                                                   cents_of(claimed->auto_top_up_amount_micros),
                                                   key});
            if (!charged) {
                failure = charged.error();
            } else {
                reference = charged->id;
            }
        }
    }
    if (failure.empty()) {
        ++report.auto_top_ups;
        static_cast<void>(record(context,
                                 balance.customer_id,
                                 "billing.balance.auto_top_up_started",
                                 "lake",
                                 "balance_refresh",
                                 "success",
                                 {{"amount_micros", claimed->auto_top_up_amount_micros},
                                  {"reference", reference},
                                  {"balance_micros", balance.balance_micros()}}));
        co_return {};
    }
    ++report.auto_top_up_failures;
    static_cast<void>(
        change_balance(*context.store, balance.customer_id, [&](control::AccountBalance& row) {
            if (row.auto_top_up_pending != key) {
                return false;
            }
            row.auto_top_up = false;
            row.auto_top_up_pending.clear();
            row.auto_top_up_started_at.clear();
            row.auto_top_up_off_reason = control::utf8_prefix(failure, 1000);
            row.auto_top_up_failure_notified_at.clear();
            return true;
        }));
    static_cast<void>(record(context,
                             balance.customer_id,
                             "billing.balance.auto_top_up_failed",
                             "lake",
                             "balance_refresh",
                             "failure",
                             {{"amount_micros", claimed->auto_top_up_amount_micros},
                              {"message", control::utf8_prefix(failure, 1000)}}));
    co_return {};
}

} // namespace

liquid::runtime::task<BalanceRefreshReport>
refresh_account_balances(ApiContext& context,
                         std::shared_ptr<PaymentProviders> providers,
                         const BalanceRuntime& runtime,
                         std::string customer_id) {
    BalanceRefreshReport report;
    if (context.store == nullptr) {
        co_return report;
    }
    std::vector<std::string> accounts;
    if (!customer_id.empty()) {
        accounts.push_back(customer_id);
    } else if (auto listed = context.store->balance_customers()) {
        accounts = std::move(*listed);
    }
    for (const std::string& account : accounts) {
        auto balance = context.store->account_balance(account);
        if (!balance) {
            continue;
        }
        ++report.accounts;
        if (!balance->charges_since().empty()) {
            auto read = co_await refresh_charges(context, *balance);
            const std::string problem = read ? *read : read.error().message;
            const std::string at = now_text();
            static_cast<void>(
                change_balance(*context.store, account, [&](control::AccountBalance& row) {
                    if (problem.empty()) {
                        row.charges_refreshed_at = at;
                        row.refresh_error.clear();
                    } else {
                        row.refresh_failed_at = at;
                        row.refresh_error = control::utf8_prefix(problem, 1000);
                    }
                    return true;
                }));
            ++(problem.empty() ? report.refreshed : report.held);
            // Only against charges just read in full: a held read would forfeit what the grant
            // in fact paid for (lake#1134). An expired grant counts until then.
            if (problem.empty()) {
                if (const auto ended =
                        end_grants(context, account, {}, "lake", "balance_refresh")) {
                    report.grants_expired += static_cast<int>(ended->size());
                }
            }
            balance = context.store->account_balance(account);
            if (!balance) {
                continue;
            }
        }
        const BalanceView view =
            balance_view(*balance, runtime.settings, account_exemption(*balance));
        if (view.exempt.empty() && !balance->charges_since().empty()) {
            // The most serious crossing not yet told, once; it covers the milder ones.
            if (view.state == "blocked") {
                notify_once(context,
                            runtime,
                            account,
                            BalanceCrossing::blocked,
                            {BalanceCrossing::negative, BalanceCrossing::limit_warning},
                            report);
            } else if (view.balance_micros <=
                       runtime.settings.warning_micros - view.overdraft_micros) {
                notify_once(context,
                            runtime,
                            account,
                            BalanceCrossing::limit_warning,
                            {BalanceCrossing::negative},
                            report);
            } else if (view.balance_micros < 0) {
                notify_once(context, runtime, account, BalanceCrossing::negative, {}, report);
            }
        }
        co_await auto_top_up(context, providers, runtime, *balance, report);
        if (auto after = context.store->account_balance(account);
            after && !after->auto_top_up_off_reason.empty() &&
            after->auto_top_up_failure_notified_at.empty()) {
            notify_once(context, runtime, account, BalanceCrossing::auto_top_up_failed, {}, report);
        }
    }
    co_return report;
}

void register_balance_capabilities(Registry& registry,
                                   std::shared_ptr<PaymentProviders> providers) {
    // The instance's balance runtime, or the defaults (a registry without a server: tests).
    const auto runtime_of = [](const ApiContext& context) -> std::shared_ptr<const BalanceRuntime> {
        static const auto defaults = std::make_shared<const BalanceRuntime>();
        return context.balance ? context.balance : defaults;
    };

    registry.add(Capability{
        .name = "account_balance",
        .title = "Account balance",
        .description =
            "The account's prepaid balance on this Lake (lake.live is prepaid): `balance_micros` "
            "(millionths of a US dollar; credits from top-ups minus charges for usage at the "
            "price list), the overdraft (`limit_micros`, how far below $0 it may go), and "
            "`state`: ok, low, negative or blocked (past the limit, or no first top-up yet: "
            "nothing that costs money starts). `add_micros` is how much to add and "
            "`add_funds_url` where. `credits` and `charges` are the history, newest first; "
            "charges are refreshed every few minutes, and a month's charge equals its bill. A "
            "credit of kind `grant` is a credit from Orama (label \"Credit from Orama\"), with "
            "its `reason`, `expires_at`, what it has left (`unused_micros`) and, once it expired "
            "or was revoked, the part that stopped counting (`forfeited_micros`); "
            "`grants_unused_micros` sums what the grants have left. Grants are spent before paid "
            "money.",
        .input_schema =
            object_schema({{"customer_id", {{"type", "string"}}}}, json::array({"customer_id"})),
        .output_schema =
            [] {
                json schema = balance_schema();
                schema["properties"]["credits"] = {{"type", "array"},
                                                   {"items", {{"type", "object"}}}};
                schema["properties"]["charges"] = {{"type", "array"},
                                                   {"items", {{"type", "object"}}}};
                schema["required"].push_back("credits");
                schema["required"].push_back("charges");
                return schema;
            }(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler =
            [runtime_of](const json& input, ApiContext& context) {
                if (!context.store) {
                    return no_store();
                }
                const std::string customer_id = input.at("customer_id");
                const auto customer = account_named(context, customer_id);
                if (!customer) {
                    return customer.error();
                }
                const auto balance = context.store->account_balance(customer_id);
                if (!balance) {
                    return from_store(balance.error());
                }
                const auto credits = context.store->balance_credits(customer_id, 50);
                const auto charges = context.store->balance_charges(customer_id);
                if (!credits || !charges) {
                    return from_store(!credits ? credits.error() : charges.error());
                }
                const auto runtime = runtime_of(context);
                BalanceRuntime shown = *runtime;
                shown.origin = link_origin(*runtime, context);
                json out = balance_json(
                    *balance,
                    balance_view(*balance, runtime->settings, account_exemption(*balance)),
                    shown);
                json credit_rows = json::array();
                const auto left = grants_left(*context.store, *balance);
                for (const control::BalanceCredit& credit : *credits) {
                    const auto found = left.find(credit.id);
                    credit_rows.push_back(
                        credit_json(credit, found == left.end() ? 0 : found->second));
                }
                json charge_rows = json::array();
                for (const control::BalanceCharge& charge : *charges) {
                    charge_rows.push_back({{"period", charge.period},
                                           {"amount_micros", charge.amount_micros},
                                           {"price_list_id", charge.price_list_id},
                                           {"invoice_id", charge.invoice_id},
                                           {"refreshed_at", charge.refreshed_at}});
                }
                out["credits"] = std::move(credit_rows);
                out["charges"] = std::move(charge_rows);
                audit(context,
                      customer_id,
                      "billing.balance.read",
                      {{"balance_micros", out["balance_micros"]}, {"state", out["state"]}});
                return ApiResult::success(std::move(out));
            }});

    registry.add(Capability{
        .name = "start_top_up",
        .title = "Add funds",
        .description =
            "The account's owners and admins. Starts a Stripe Checkout that adds `amount_micros` "
            "(millionths of a US dollar) to the account's prepaid balance, and answers its "
            "`checkout_url`; the balance is credited when Stripe says it is paid, once. The "
            "first top-up is the instance's first amount ($20 by default); later ones are any "
            "amount from the minimum ($20 by default). Left out, the amount is the first "
            "top-up, else the minimum. Refused (conflict) where card payment is not set up.",
        .input_schema = object_schema(
            {{"customer_id", {{"type", "string"}}},
             {"amount_micros", amount_schema("whole cents, in millionths of a US dollar")}},
            json::array({"customer_id"})),
        .output_schema = object_schema(
            {{"customer_id", {{"type", "string"}}},
             {"amount_micros", {{"type", "integer"}}},
             {"checkout_url", {{"type", "string"}}},
             {"session_id", {{"type", "string"}}}},
            json::array({"customer_id", "amount_micros", "checkout_url", "session_id"})),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [providers, runtime_of](const json& input, ApiContext& context)
            -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto customer = account_named(context, customer_id);
            if (!customer) {
                co_return customer.error();
            }
            const auto runtime = runtime_of(context);
            const auto balance = context.store->account_balance(customer_id);
            if (!balance) {
                co_return from_store(balance.error());
            }
            const bool first = balance->first_top_up_at.empty();
            const std::int64_t amount =
                input.value("amount_micros",
                            first ? runtime->settings.first_top_up_micros
                                  : runtime->settings.minimum_top_up_micros);
            if (amount % 10'000 != 0) {
                co_return ApiResult::failure(
                    "invalid_input", "the amount is whole cents", "/amount_micros");
            }
            if (first && amount != runtime->settings.first_top_up_micros) {
                co_return ApiResult::failure("invalid_input",
                                             "the first top-up is " +
                                                 usd(runtime->settings.first_top_up_micros),
                                             "/amount_micros");
            }
            if (!first && amount < runtime->settings.minimum_top_up_micros) {
                co_return ApiResult::failure("invalid_input",
                                             "a top-up is at least " +
                                                 usd(runtime->settings.minimum_top_up_micros),
                                             "/amount_micros");
            }
            auto provider = co_await card_provider(*providers, context);
            if (!provider) {
                co_return ApiResult::failure("store_error", provider.error().message);
            }
            if (!*provider) {
                co_return provider->error();
            }
            auto account = billing_payment_account(context, ***provider, *customer, 0);
            if (!account) {
                co_return account.error();
            }
            const std::string origin = link_origin(*runtime, context);
            if (!origin.starts_with("https://") && !origin.starts_with("http://")) {
                co_return ApiResult::failure(
                    "store_unavailable",
                    "this Lake has no public origin (LAKE_EXTERNAL_ORIGIN) to return to after "
                    "paying");
            }
            const std::string page = add_funds_link(origin, customer_id);
            auto session = (**provider)
                               ->create_balance_checkout(
                                   *account,
                                   {"top_up",
                                    cents_of(amount),
                                    page + "&top_up=done&session_id={CHECKOUT_SESSION_ID}",
                                    page + "&top_up=cancelled",
                                    control::new_uuid_v4()});
            if (!session) {
                co_return (**provider)->records_payments_by_hand()
                    ? ApiResult::failure("conflict",
                                         "card payment is not set up on this Lake, so "
                                         "funds cannot be added online; ask Orama")
                    : ApiResult::failure("store_unavailable",
                                         "the payment provider refused: " + session.error());
            }
            audit(context,
                  customer_id,
                  "billing.balance.top_up_started",
                  {{"amount_micros", amount}, {"session_id", session->id}, {"first", first}});
            co_return ApiResult::success({{"customer_id", customer_id},
                                          {"amount_micros", amount},
                                          {"checkout_url", session->url},
                                          {"session_id", session->id}});
        }});

    registry.add(Capability{
        .name = "set_auto_top_up",
        .title = "Set auto top-up",
        .description =
            "The account's owners and admins. Auto top-up is off by default. On, Lake adds "
            "`amount_micros` (at least the minimum top-up; $20 by default) from a saved card "
            "whenever the refreshed balance falls below `threshold_micros` ($5 by default): at "
            "most one at a time and a few a day. Turning it on without a saved card (or with "
            "`replace_card`) answers a Stripe Checkout `checkout_url` that saves one; it is on "
            "once Stripe says the card is saved. Lake never sees the card. A declined charge "
            "turns it off and emails the owner.",
        .input_schema = object_schema(
            {{"customer_id", {{"type", "string"}}},
             {"enabled", {{"type", "boolean"}}},
             {"threshold_micros", amount_schema("top up when the balance falls below this")},
             {"amount_micros", amount_schema("how much each auto top-up adds, whole cents")},
             {"replace_card", {{"type", "boolean"}}}},
            json::array({"customer_id", "enabled"})),
        .output_schema = object_schema({{"customer_id", {{"type", "string"}}},
                                        {"auto_top_up", {{"type", "object"}}},
                                        {"checkout_url", {{"type", "string"}}}},
                                       json::array({"customer_id", "auto_top_up"})),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [providers, runtime_of](const json& input, ApiContext& context)
            -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto customer = account_named(context, customer_id);
            if (!customer) {
                co_return customer.error();
            }
            const auto runtime = runtime_of(context);
            const bool enabled = input.at("enabled").get<bool>();
            const auto current = context.store->account_balance(customer_id);
            if (!current) {
                co_return from_store(current.error());
            }
            const std::int64_t threshold =
                input.value("threshold_micros",
                            current->version == 0 ? runtime->settings.auto_top_up_threshold_micros
                                                  : current->auto_top_up_threshold_micros);
            const std::int64_t amount = input.value(
                "amount_micros",
                std::max(current->version == 0 ? runtime->settings.auto_top_up_amount_micros
                                               : current->auto_top_up_amount_micros,
                         runtime->settings.minimum_top_up_micros));
            if (amount % 10'000 != 0 || amount < runtime->settings.minimum_top_up_micros) {
                co_return ApiResult::failure("invalid_input",
                                             "each auto top-up is whole cents and at least " +
                                                 usd(runtime->settings.minimum_top_up_micros),
                                             "/amount_micros");
            }
            const bool needs_card = enabled && (current->auto_top_up_payment_method.empty() ||
                                                input.value("replace_card", false));
            std::string checkout_url;
            if (needs_card) {
                auto provider = co_await card_provider(*providers, context);
                if (!provider) {
                    co_return ApiResult::failure("store_error", provider.error().message);
                }
                if (!*provider) {
                    co_return provider->error();
                }
                auto account = billing_payment_account(context, ***provider, *customer, 0);
                if (!account) {
                    co_return account.error();
                }
                const std::string origin = link_origin(*runtime, context);
                if (!origin.starts_with("https://") && !origin.starts_with("http://")) {
                    co_return ApiResult::failure(
                        "store_unavailable",
                        "this Lake has no public origin (LAKE_EXTERNAL_ORIGIN) to return to");
                }
                const std::string page =
                    std::format("{}/billing?customer_id={}", origin, customer_id);
                auto session = (**provider)
                                   ->create_balance_checkout(*account,
                                                             {"card_setup",
                                                              0,
                                                              page + "&auto_top_up=saved",
                                                              page + "&auto_top_up=cancelled",
                                                              control::new_uuid_v4()});
                if (!session) {
                    co_return (**provider)->records_payments_by_hand()
                        ? ApiResult::failure("conflict",
                                             "card payment is not set up on this Lake, "
                                             "so auto top-up cannot save a card")
                        : ApiResult::failure("store_unavailable",
                                             "the payment provider refused: " + session.error());
                }
                checkout_url = session->url;
            }
            const auto stored =
                change_balance(*context.store, customer_id, [&](control::AccountBalance& row) {
                    row.auto_top_up_threshold_micros = threshold;
                    row.auto_top_up_amount_micros = amount;
                    if (!enabled) {
                        row.auto_top_up = false;
                        if (row.auto_top_up_pending == "card_setup") {
                            row.auto_top_up_pending.clear();
                        }
                    } else if (needs_card) {
                        // On once Stripe says the card is saved (setup_intent.succeeded).
                        row.auto_top_up = false;
                        if (row.auto_top_up_pending.empty() ||
                            row.auto_top_up_pending == "card_setup") {
                            row.auto_top_up_pending = "card_setup";
                        }
                    } else {
                        row.auto_top_up = true;
                        row.auto_top_up_off_reason.clear();
                        row.auto_top_up_failure_notified_at.clear();
                    }
                    return true;
                });
            if (!stored) {
                co_return from_store(stored.error());
            }
            audit(context,
                  customer_id,
                  "billing.balance.auto_top_up_set",
                  {{"enabled", enabled},
                   {"threshold_micros", threshold},
                   {"amount_micros", amount},
                   {"card_requested", needs_card}});
            json out{{"customer_id", customer_id}, {"auto_top_up", auto_top_up_json(*stored)}};
            if (!checkout_url.empty()) {
                out["checkout_url"] = checkout_url;
            }
            co_return ApiResult::success(std::move(out));
        }});

    registry.add(Capability{
        .name = "set_balance_overdraft",
        .title = "Set an account's overdraft",
        .description =
            "Operator or instance administrator only. How far below $0 one account's prepaid "
            "balance may go before Lake stops starting paid work (`overdraft_micros`, millionths "
            "of a US dollar), in place of the instance's (LAKE_BALANCE_OVERDRAFT_USD, $50 by "
            "default); left out, the instance's applies again.",
        .input_schema = object_schema(
            {{"customer_id", {{"type", "string"}}},
             {"overdraft_micros",
              {{"type", "integer"}, {"minimum", 0}, {"maximum", 1'000'000'000'000LL}}}},
            json::array({"customer_id"})),
        .output_schema = balance_schema(),
        .kind = CapabilityKind::Action,
        .object = "billing balance",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [runtime_of](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return ApiResult::failure("unauthorized",
                                          "the operator or the instance administrator is required");
            }
            if (!context.store) {
                return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto customer = account_named(context, customer_id);
            if (!customer) {
                return customer.error();
            }
            const json limit =
                input.contains("overdraft_micros") ? input.at("overdraft_micros") : json(nullptr);
            const auto stored =
                change_balance(*context.store, customer_id, [&](control::AccountBalance& row) {
                    row.overdraft_limit_micros =
                        limit.is_null() ? std::nullopt
                                        : std::optional<std::int64_t>(limit.get<std::int64_t>());
                    return true;
                });
            if (!stored) {
                return from_store(stored.error());
            }
            const auto runtime = runtime_of(context);
            audit(context,
                  customer_id,
                  "billing.balance.overdraft_set",
                  {{"overdraft_micros", limit}});
            BalanceRuntime shown = *runtime;
            shown.origin = link_origin(*runtime, context);
            return ApiResult::success(
                balance_json(*stored,
                             balance_view(*stored, runtime->settings, account_exemption(*stored)),
                             shown));
        }});

    registry.add(Capability{
        .name = "refresh_account_balance",
        .title = "Refresh an account's balance",
        .description =
            "Operator or instance administrator only. Refreshes one account's charges now, as "
            "the store root's owner does every few minutes: each month's usage at the list in "
            "force, a failed read holding the balance; then the crossings' emails and auto "
            "top-up. Answers the balance and what the refresh did.",
        .input_schema =
            object_schema({{"customer_id", {{"type", "string"}}}}, json::array({"customer_id"})),
        .output_schema =
            object_schema({{"balance", balance_schema()}, {"refresh", {{"type", "object"}}}},
                          json::array({"balance", "refresh"})),
        .kind = CapabilityKind::Action,
        .object = "billing balance",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = [providers, runtime_of](const json& input, ApiContext& context)
            -> liquid::runtime::task<ApiResult> {
            if (!instance_administrator(context)) {
                co_return ApiResult::failure(
                    "unauthorized", "the operator or the instance administrator is required");
            }
            if (!context.store) {
                co_return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const auto customer = account_named(context, customer_id);
            if (!customer) {
                co_return customer.error();
            }
            const auto runtime = runtime_of(context);
            ApiContext inner = context;
            auto refreshed =
                co_await refresh_account_balances(inner, providers, *runtime, customer_id);
            const BalanceRefreshReport report = refreshed ? *refreshed : BalanceRefreshReport{};
            const auto balance = context.store->account_balance(customer_id);
            if (!balance) {
                co_return from_store(balance.error());
            }
            BalanceRuntime shown = *runtime;
            shown.origin = link_origin(*runtime, context);
            audit(context,
                  customer_id,
                  "billing.balance.refresh",
                  {{"refreshed", report.refreshed}, {"held", report.held}});
            co_return ApiResult::success(
                {{"balance",
                  balance_json(
                      *balance,
                      balance_view(*balance, runtime->settings, account_exemption(*balance)),
                      shown)},
                 {"refresh",
                  {{"refreshed", report.refreshed},
                   {"held", report.held},
                   {"refresh_error", balance->refresh_error},
                   {"emails", report.emails},
                   {"auto_top_ups", report.auto_top_ups},
                   {"auto_top_up_failures", report.auto_top_up_failures},
                   {"grants_expired", report.grants_expired}}}});
        }});
    registry.add(Capability{
        .name = "grant_balance_credit",
        .title = "Give an account a credit",
        .description =
            "Operator or instance administrator only. Gives one account a credit from Orama "
            "(a design partner, a prize, a make-good): `amount_micros` (millionths of a US "
            "dollar, whole cents, $0.01 to $10,000) added to its prepaid balance, recorded as a "
            "credit of kind `grant` beside its top-ups. Name the account by `customer_id`, or a "
            "person by `email` (their personal account). `reason` is required and shown to the "
            "account; `expires_at` (UTC, YYYY-MM-DDTHH:MM:SSZ, in the future, within 10 years) "
            "is when the unused part stops counting. A grant is spent before paid money and "
            "pays only for usage after it is given. It is not cash and does not unlock the "
            "overdraft: an account that has never paid stops at $0. Clears the low-balance "
            "emails' marks, emails the account once, and is audited "
            "(billing.balance.credit_granted). `reference` makes a retry safe: the same "
            "reference grants once.",
        .input_schema = object_schema(
            {{"customer_id", {{"type", "string"}}},
             {"email", {{"type", "string"}, {"maxLength", 320}}},
             {"amount_micros",
              {{"type", "integer"},
               {"minimum", 10'000},
               {"maximum", control::balance_grant_most_micros},
               {"description", "whole cents, in millionths of a US dollar"}}},
             {"reason", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1000}}},
             {"expires_at", {{"type", "string"}, {"description", "UTC, YYYY-MM-DDTHH:MM:SSZ"}}},
             {"reference",
              {{"type", "string"},
               {"minLength", 1},
               {"maxLength", 200},
               {"description", "an idempotency key: the same reference grants once"}}}},
            json::array({"amount_micros", "reason"})),
        .output_schema = object_schema({{"customer_id", {{"type", "string"}}},
                                        {"duplicate", {{"type", "boolean"}}},
                                        {"credit", {{"type", "object"}}},
                                        {"emailed", {{"type", "boolean"}}},
                                        {"email_error", {{"type", "string"}}},
                                        {"balance", balance_schema()}},
                                       json::array({"customer_id", "duplicate", "balance"})),
        .kind = CapabilityKind::Action,
        .object = "billing balance",
        .action = "admin",
        .resolve_object = administrator_only,
        .handler = [runtime_of](const json& input, ApiContext& context) {
            if (!instance_administrator(context)) {
                return ApiResult::failure("unauthorized",
                                          "the operator or the instance administrator is required");
            }
            if (!context.store) {
                return no_store();
            }
            const std::string named = input.value("customer_id", std::string());
            const std::string email = input.value("email", std::string());
            if (named.empty() == email.empty()) {
                return ApiResult::failure("invalid_input",
                                          "name the account by customer_id or a person by email, "
                                          "not both",
                                          named.empty() ? "/customer_id" : "/email");
            }
            std::string customer_id = named;
            if (!email.empty()) {
                const auto personal = context.store->personal_account_by_email(email);
                if (!personal) {
                    return personal.error().code == "not_found"
                               ? ApiResult::failure("not_found",
                                                    "no person with that email has a personal "
                                                    "account on this Lake",
                                                    "/email")
                               : from_store(personal.error());
                }
                customer_id = personal->id;
            } else if (const auto customer = account_named(context, customer_id); !customer) {
                return customer.error();
            }
            const std::int64_t amount = input.at("amount_micros").get<std::int64_t>();
            if (amount % 10'000 != 0) {
                return ApiResult::failure(
                    "invalid_input", "the amount is whole cents", "/amount_micros");
            }
            const std::string reason = input.at("reason").get<std::string>();
            if (reason.find_first_not_of(" \t\r\n") == std::string::npos) {
                return ApiResult::failure(
                    "invalid_input", "say why the credit is given", "/reason");
            }
            if (reason.size() > 1000) {
                return ApiResult::failure(
                    "invalid_input", "the reason is at most 1,000 bytes", "/reason");
            }
            const std::string reference = input.value("reference", std::string());
            if (input.contains("reference") &&
                (reference.empty() || reference.size() > 200 ||
                 !std::ranges::all_of(reference, [](char c) {
                     return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
                            c == '.' || c == ':';
                 }))) {
                return ApiResult::failure("invalid_input",
                                          "the reference is 1 to 200 letters, digits, '-', '_', "
                                          "'.' or ':'",
                                          "/reference");
            }
            const std::string expires_at = input.value("expires_at", std::string());
            if (!expires_at.empty()) {
                const auto at = utc_seconds(expires_at);
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
            control::BalanceCredit credit;
            credit.customer_id = customer_id;
            credit.kind = "grant";
            credit.amount_micros = amount;
            credit.provider = "orama";
            credit.reference = "grant-" + (reference.empty() ? control::new_uuid_v4() : reference);
            credit.reason = reason;
            credit.granted_by = control::utf8_prefix(
                context.operator_credential ? std::string("operator") : context.acting_identity,
                255);
            credit.expires_at = expires_at;
            const auto applied = context.store->apply_balance_credit(credit);
            if (!applied) {
                return from_store(applied.error());
            }
            const auto runtime = runtime_of(context);
            BalanceRuntime shown = *runtime;
            shown.origin = link_origin(*runtime, context);
            const BalanceView view = balance_view(
                applied->balance, runtime->settings, account_exemption(applied->balance));
            json out{{"customer_id", customer_id},
                     {"duplicate", applied->duplicate},
                     {"balance", balance_json(applied->balance, view, shown)}};
            if (applied->duplicate) {
                // A retry with the same reference: granted once, emailed once.
                audit(context,
                      customer_id,
                      "billing.balance.credit_granted",
                      {{"duplicate", true}, {"reference", credit.reference}});
                return ApiResult::success(std::move(out));
            }
            out["credit"] = credit_json(applied->credit, amount);
            // The email, once per grant (its id keys the send).
            bool emailed = false;
            std::string email_error;
            const std::string to = balance_email_recipient(*context.store, customer_id);
            if (!runtime->mailer) {
                email_error = runtime->email_unavailable_reason.empty()
                                  ? "email is not configured on this Lake"
                                  : runtime->email_unavailable_reason;
            } else if (to.empty()) {
                email_error = "the account has no email address to send to";
            } else {
                const Email message = balance_grant_email(
                    amount,
                    expires_at,
                    view,
                    !applied->balance.first_top_up_at.empty(),
                    std::format("{}/billing?customer_id={}", shown.origin, customer_id),
                    {to});
                const auto sent =
                    runtime->mailer(message, "lake-balance-grant-" + applied->credit.id);
                if (sent) {
                    emailed = true;
                    static_cast<void>(record(context,
                                             customer_id,
                                             "billing.balance.email",
                                             context.acting_identity.empty()
                                                 ? std::string("operator")
                                                 : context.acting_identity,
                                             "grant_balance_credit",
                                             "success",
                                             {{"crossing", "grant"},
                                              {"credit_id", applied->credit.id},
                                              {"balance_micros", view.balance_micros}}));
                } else {
                    email_error = control::utf8_prefix(sent.error(), 500);
                }
            }
            out["emailed"] = emailed;
            if (!email_error.empty()) {
                out["email_error"] = email_error;
            }
            audit(context,
                  customer_id,
                  "billing.balance.credit_granted",
                  {{"credit_id", applied->credit.id},
                   {"amount_micros", amount},
                   {"reason", credit.reason},
                   {"expires_at", expires_at},
                   {"granted_by", credit.granted_by},
                   {"by_email", !email.empty()},
                   {"reference", credit.reference},
                   {"emailed", emailed},
                   {"balance_micros", view.balance_micros}});
            return ApiResult::success(std::move(out));
        }});

    registry.add(Capability{
        .name = "revoke_balance_credit",
        .title = "Revoke a credit from Orama",
        .description =
            "Operator or instance administrator only. Revokes a grant (`credit_id`, from "
            "account_balance's credits): what it has not paid for stops counting. What it paid "
            "for stays paid; it never takes money back. The account's usage is read first, so "
            "the remainder is exact; refused (store_unavailable) when it cannot be read, and "
            "conflict when the grant already expired or was revoked. Audited "
            "(billing.balance.credit_revoked).",
        .input_schema = object_schema({{"credit_id", {{"type", "string"}}},
                                       {"reason", {{"type", "string"}, {"maxLength", 1000}}}},
                                      json::array({"credit_id"})),
        .output_schema =
            object_schema({{"customer_id", {{"type", "string"}}},
                           {"credit", {{"type", "object"}}},
                           {"revoked_micros", {{"type", "integer"}}},
                           {"balance", balance_schema()}},
                          json::array({"customer_id", "credit", "revoked_micros", "balance"})),
        .kind = CapabilityKind::Action,
        .object = "billing balance",
        .action = "admin",
        .resolve_object = administrator_only,
        .async_handler = [runtime_of](const json& input,
                                      ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!instance_administrator(context)) {
                co_return ApiResult::failure(
                    "unauthorized", "the operator or the instance administrator is required");
            }
            if (!context.store) {
                co_return no_store();
            }
            const std::string credit_id = input.at("credit_id");
            if (input.value("reason", std::string()).size() > 1000) {
                co_return ApiResult::failure(
                    "invalid_input", "the reason is at most 1,000 bytes", "/reason");
            }
            const auto credit = context.store->balance_credit(credit_id);
            if (!credit) {
                co_return credit.error().code == "not_found"
                    ? ApiResult::failure("not_found", "no credit " + credit_id, "/credit_id")
                    : from_store(credit.error());
            }
            if (credit->kind != "grant") {
                co_return ApiResult::failure("invalid_input",
                                             "only a credit from Orama can be revoked; a paid "
                                             "top-up is refunded through the payment provider",
                                             "/credit_id");
            }
            if (!credit->ended_at.empty()) {
                co_return ApiResult::failure("conflict",
                                             "the credit already " + credit->end_reason + " on " +
                                                 credit->ended_at);
            }
            // Read the usage first, so the remainder is what it is now.
            const auto before = context.store->account_balance(credit->customer_id);
            if (!before) {
                co_return from_store(before.error());
            }
            ApiContext inner = context;
            auto read = co_await refresh_charges(inner, *before);
            const std::string problem = read ? *read : read.error().message;
            if (!problem.empty()) {
                co_return ApiResult::failure("store_unavailable",
                                             "the account's usage could not be read, so what "
                                             "the credit paid for is not known; try again: " +
                                                 problem);
            }
            auto ended = end_grants(inner,
                                    credit->customer_id,
                                    credit_id,
                                    context.acting_identity.empty() ? std::string("operator")
                                                                    : context.acting_identity,
                                    "revoke_balance_credit");
            if (!ended) {
                co_return from_store(ended.error());
            }
            const auto revoked = std::ranges::find(*ended, credit_id, &control::BalanceCredit::id);
            if (revoked == ended->end()) {
                co_return ApiResult::failure("conflict", "the credit no longer counts");
            }
            const auto balance = context.store->account_balance(credit->customer_id);
            if (!balance) {
                co_return from_store(balance.error());
            }
            const std::string reason = input.value("reason", std::string());
            audit(context,
                  credit->customer_id,
                  "billing.balance.credit_revoked",
                  {{"credit_id", credit_id},
                   {"amount_micros", revoked->amount_micros},
                   {"used_micros", revoked->amount_micros - revoked->forfeited_micros},
                   {"revoked_micros", revoked->forfeited_micros},
                   {"reason", control::utf8_prefix(reason, 1000)},
                   {"balance_micros", balance->balance_micros()}});
            const auto runtime = runtime_of(context);
            BalanceRuntime shown = *runtime;
            shown.origin = link_origin(*runtime, context);
            co_return ApiResult::success(
                {{"customer_id", credit->customer_id},
                 {"credit", credit_json(*revoked, 0)},
                 {"revoked_micros", revoked->forfeited_micros},
                 {"balance",
                  balance_json(
                      *balance,
                      balance_view(*balance, runtime->settings, account_exemption(*balance)),
                      shown)}});
        }});
}

} // namespace lake::api
