// SPDX-License-Identifier: MIT

// A workspace's settings (lake#1230, docs/api/workspaces.md#settings). Today one: `agent_model`,
// the model every agent turn of the workspace calls through Lake's model gateway, chosen among the
// served chat models; unset means default_agent_model (gpt-6.1-sol). The settings are workspace
// data, so they are one sealed record in the workspace's Zetta (`lake_workspace_settings`), never
// a row of lake's Postgres (the multitenancy rule).

#include "lake/api/workspace_settings.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <utility>

#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/model_gateway.hpp"
#include "lake/control/store.hpp"
#include "lake/control/workspace_settings.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_object = "customer/" + customer_id;
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

ApiResult failed(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

json settings_schema(json properties) {
    properties["customer_id"] = {{"type", "string"}};
    properties["workspace_id"] = {{"type", "string"}};
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", json::array({"customer_id", "workspace_id"})},
            {"additionalProperties", false}};
}

json settings_output_schema() {
    const json price{{"type", {"integer", "null"}}};
    return {{"type", "object"},
            {"properties",
             {{"workspace_id", {{"type", "string"}}},
              {"agent_model", {{"type", "string"}}},
              {"agent_model_chosen", {{"type", {"string", "null"}}}},
              {"default_agent_model", {{"type", "string"}}},
              {"updated_at", {{"type", {"string", "null"}}}},
              {"set_by", {{"type", {"string", "null"}}}},
              {"currency", {{"type", {"string", "null"}}}},
              {"agent_models",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"model", {{"type", "string"}}},
                    {"default", {{"type", "boolean"}}},
                    {"input_micros_per_million", price},
                    {"output_micros_per_million", price},
                    {"long_input_micros_per_million", price},
                    {"long_output_micros_per_million", price}}}}}}}}}};
}

// What a million tokens of `name` cost on the list, in millionths of the list's currency; null
// when the list has no rate for it.
json per_million(const std::optional<PriceList>& list, const std::string& name) {
    if (!list) {
        return nullptr;
    }
    for (const control::BillingPrice& price : list->prices) {
        if (price.dimension == "model_tokens" && price.class_name == name && price.per > 0) {
            return price_of(1'000'000, price.unit_micros, price.per);
        }
    }
    return nullptr;
}

// The price list in force now, where billing is on and a list is published.
std::optional<PriceList> list_in_force(ApiContext& context) {
    if (!billing_registered(context.registry) || context.store == nullptr) {
        return std::nullopt;
    }
    const auto stored = context.store->billing_plans();
    if (!stored) {
        return std::nullopt;
    }
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    return price_list_for(price_lists(*stored),
                          std::format("{:04}-{:02}", int(today.year()), unsigned(today.month())));
}

json settings_json(ApiContext& context,
                   const std::string& workspace_id,
                   const std::optional<control::WorkspaceSettings>& settings) {
    const std::string chosen = settings ? settings->agent_model : std::string();
    const auto list = list_in_force(context);
    json models = json::array();
    for (const std::string& model : gateway_chat_models(context.model_gateway_models)) {
        json entry{
            {"model", model},
            {"default", model == default_agent_model},
            {"input_micros_per_million", per_million(list, model_token_class(model, "input"))},
            {"output_micros_per_million", per_million(list, model_token_class(model, "output"))},
            {"long_input_micros_per_million", nullptr},
            {"long_output_micros_per_million", nullptr}};
        if (has_long_context_rate(model)) {
            entry["long_input_micros_per_million"] = per_million(
                list, model_token_class(model, "input", long_context_prompt_tokens + 1));
            entry["long_output_micros_per_million"] = per_million(
                list, model_token_class(model, "output", long_context_prompt_tokens + 1));
        }
        models.push_back(std::move(entry));
    }
    return {
        {"workspace_id", workspace_id},
        {"agent_model", chosen.empty() ? std::string(default_agent_model) : chosen},
        {"agent_model_chosen", chosen.empty() ? json(nullptr) : json(chosen)},
        {"default_agent_model", default_agent_model},
        {"updated_at",
         settings && !settings->updated_at.empty() ? json(settings->updated_at) : json(nullptr)},
        {"set_by", settings && !settings->set_by.empty() ? json(settings->set_by) : json(nullptr)},
        {"currency", list ? json(list->currency) : json(nullptr)},
        {"agent_models", std::move(models)}};
}

liquid::runtime::task<ApiResult> get_settings(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    context.workspace_id = workspace_id;
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto settings =
        control::settled(co_await control::get_workspace_settings(*records, customer_id));
    if (!settings) {
        co_return failed(settings.error());
    }
    co_return ApiResult::success(settings_json(context, workspace_id, *settings));
}

liquid::runtime::task<ApiResult> set_settings(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    context.workspace_id = workspace_id;
    // Absent: unchanged. Null or "": back to the default.
    const bool changing = input.contains("agent_model");
    std::string model;
    if (const auto found = input.find("agent_model"); found != input.end() && found->is_string()) {
        model = found->get<std::string>();
    }
    if (changing && !model.empty()) {
        const auto served = gateway_chat_models(context.model_gateway_models);
        if (std::ranges::find(served, model) == served.end()) {
            co_return ApiResult::failure(
                "invalid_input",
                "agent_model: choose one of the chat models this Lake serves "
                "(get_workspace_settings lists them)",
                "/agent_model");
        }
        if (const auto priced = validate_model_prices(model, context, "/agent_model"); !priced) {
            co_return ApiResult::failure(
                priced.error().code, priced.error().message, priced.error().path);
        }
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto current =
        control::settled(co_await control::get_workspace_settings(*records, customer_id));
    if (!current) {
        co_return failed(current.error());
    }
    control::WorkspaceSettings next = current->value_or(control::WorkspaceSettings{});
    const std::string before = next.agent_model;
    next.customer_id = customer_id;
    if (changing) {
        next.agent_model = model;
    }
    model = next.agent_model;
    next.set_by = context.acting_user_id;
    next.set_by_identity = context.acting_identity;
    auto written =
        control::settled(co_await control::put_workspace_settings(*records, std::move(next)));
    if (!written) {
        co_return failed(written.error());
    }
    context.audit_workspace_id = workspace_id;
    context.audit_object = "workspace/" + workspace_id + "/settings";
    context.audit_details = {
        {"agent_model", model.empty() ? json(nullptr) : json(model)},
        {"previous_agent_model", before.empty() ? json(nullptr) : json(before)}};
    co_return ApiResult::success(settings_json(context, workspace_id, *written));
}

} // namespace

liquid::runtime::task<control::ZettaControlResult<std::string>> workspace_agent_model(
    const ApiContext& context, std::string customer_id, std::string workspace_id) {
    using Result = control::ZettaControlResult<std::string>;
    if (!context.workspaces) {
        co_return Result(std::string(default_agent_model));
    }
    auto records = workspace_records(context, customer_id, std::move(workspace_id));
    if (!records) {
        co_return Result(std::unexpected(records.error()));
    }
    auto settings =
        control::settled(co_await control::get_workspace_settings(*records, customer_id));
    if (!settings) {
        co_return Result(std::unexpected(settings.error()));
    }
    if (!*settings || (*settings)->agent_model.empty()) {
        co_return Result(std::string(default_agent_model));
    }
    co_return Result((*settings)->agent_model);
}

void register_workspace_settings_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "get_workspace_settings",
        .title = "Get a workspace's settings",
        .description =
            "Read a workspace's settings: `agent_model`, the model its agents call through Lake "
            "(the one chosen, or gpt-6.1-sol when none is), and every chat model this Lake serves "
            "with its price per million input and output tokens from the price list in force "
            "(null where billing is off or the list has no rate). Prompts over 272,000 input "
            "tokens are billed at the long-context prices.",
        .input_schema = settings_schema(json::object()),
        .output_schema = settings_output_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = get_settings});
    registry.add(Capability{
        .name = "set_workspace_settings",
        .title = "Change a workspace's settings",
        .description =
            "Choose the model a workspace's agents call through Lake (`agent_model`): one of the "
            "chat models get_workspace_settings lists, priced where billing is on. Every agent "
            "turn of the workspace then calls it, whatever model the agent asks for, and its "
            "usage is billed under it. Null or an empty string goes back to the default, "
            "gpt-6.1-sol; left out, it is unchanged. Needs permission to change the workspace's "
            "account.",
        .input_schema =
            settings_schema({{"agent_model", {{"type", {"string", "null"}}, {"maxLength", 128}}}}),
        .output_schema = settings_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = set_settings});
}

} // namespace lake::api
