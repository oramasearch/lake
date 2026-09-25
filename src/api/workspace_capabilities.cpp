#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <string_view>

#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

// The Zetta capabilities lake's pages may call on a customer's workspace (docs/api/workspaces.md).
// Reads need `read` on the customer, actions `write`. Anything else is refused, so a lake caller
// can never reach Zetta's administrative surface (sql, users, cloud provisioning) through lake.
constexpr std::array<std::string_view, 18> workspace_reads = {"server_info",
                                                              "list_projects",
                                                              "get_project",
                                                              "get_graph",
                                                              "list_connections",
                                                              "list_datasets",
                                                              "get_dataset",
                                                              "list_runs",
                                                              "get_run",
                                                              "get_run_report",
                                                              "list_models",
                                                              "get_model",
                                                              "model_health",
                                                              "list_indexes",
                                                              "get_index",
                                                              "search",
                                                              "list_jobs",
                                                              "get_job"};
constexpr std::array<std::string_view, 14> workspace_actions = {"create_project",
                                                                "capture_intent",
                                                                "compile_intent",
                                                                "approve_plan",
                                                                "gate",
                                                                "run",
                                                                "create_connection",
                                                                "create_index",
                                                                "expose_model",
                                                                "unexpose_model",
                                                                "expose_index",
                                                                "unexpose_index",
                                                                "score",
                                                                "explain"};

template <std::size_t Size>
bool listed(const std::array<std::string_view, Size>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
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

json customer_input_schema(json properties, json required) {
    properties["customer_id"] = {{"type", "string"}};
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

ApiResult unavailable() {
    return ApiResult::failure("store_unavailable", "workspaces are not configured on this lake");
}

void set_audit(ApiContext& context, std::string kind, const json& input, std::string capability) {
    context.audit_kind = std::move(kind);
    context.audit_details = {
        {"customer_id", input.at("customer_id")},
        {"workspace", control::workspace_slug(input.at("customer_id").get<std::string>())}};
    if (!capability.empty()) {
        context.audit_details["capability"] = std::move(capability);
    }
}

void register_workspace_call(Registry& registry, bool read) {
    registry.add(Capability{
        .name = read ? "workspace_query" : "workspace_act",
        .title = read ? "Query a workspace" : "Act in a workspace",
        .description =
            read ? "Call one of the allowed read capabilities on the customer's Zetta workspace."
                 : "Call one of the allowed actions on the customer's Zetta workspace.",
        .input_schema = customer_input_schema(
            {{"capability", {{"type", "string"}}}, {"input", {{"type", "object"}}}},
            json::array({"capability"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = read ? "read" : "write",
        .resolve_object = customer_object,
        .async_handler = [read](const json& input,
                                ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string capability = input.at("capability");
            if (read ? !listed(workspace_reads, capability)
                     : !listed(workspace_actions, capability)) {
                co_return ApiResult::failure("invalid_input",
                                             capability + " is not a workspace " +
                                                 (read ? "read" : "action"),
                                             "/capability");
            }
            set_audit(context, read ? "workspace.query" : "workspace.act", input, capability);
            auto called = co_await context.workspaces->call(
                input.at("customer_id"), capability, read, input.value("input", json::object()));
            if (!called) {
                co_return ApiResult::failure("store_unavailable", called.error().message);
            }
            if (!*called) {
                co_return ApiResult::failure(called->error().code, called->error().message);
            }
            json value = std::move(**called);
            co_return ApiResult::success(value.is_object() ? std::move(value)
                                                           : json{{"value", std::move(value)}});
        }});
}

} // namespace

void register_workspace_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "workspace_status",
        .title = "Workspace status",
        .description = "Whether the customer's Zetta workspace was requested, its operator phase, "
                       "and whether lake can call it.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            auto state = co_await context.workspaces->state(input.at("customer_id"));
            if (!state) {
                co_return ApiResult::failure("store_unavailable", state.error().message);
            }
            if (!*state) {
                co_return ApiResult::failure(state->error().code, state->error().message);
            }
            co_return ApiResult::success((*state)->to_json());
        }});
    registry.add(Capability{
        .name = "start_workspace",
        .title = "Start a workspace",
        .description = "Ask the operator for the customer's Zetta workspace. Repeating it is "
                       "harmless.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            set_audit(context, "workspace.start", input, "");
            auto state = co_await context.workspaces->request(input.at("customer_id"));
            if (!state) {
                co_return ApiResult::failure("store_unavailable", state.error().message);
            }
            if (!*state) {
                co_return ApiResult::failure(state->error().code, state->error().message);
            }
            co_return ApiResult::success((*state)->to_json());
        }});
    register_workspace_call(registry, true);
    register_workspace_call(registry, false);
}

} // namespace lake::api
