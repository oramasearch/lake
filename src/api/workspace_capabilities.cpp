#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <string_view>

#include "lake/control/policy.hpp"
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
constexpr std::array<std::string_view, 18> workspace_actions = {"create_project",
                                                                "capture_intent",
                                                                "compile_intent",
                                                                "approve_plan",
                                                                "gate",
                                                                "run",
                                                                "create_connection",
                                                                "delete_connection",
                                                                "create_dataset_from_query",
                                                                "upload_dataset",
                                                                "delete_project",
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

ApiResult store_failure(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

void service_audit(ApiContext& context, std::string kind, const control::ManagedService& service) {
    context.audit_kind = std::move(kind);
    context.audit_details = {{"customer_id", service.customer_id},
                             {"service_id", service.id},
                             {"service_kind", service.kind},
                             {"service_name", service.name}};
}

bool valid_service_name(const std::string& name) {
    if (name.empty() || name.size() > 31 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    });
}

void register_service_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_services",
        .title = "List services",
        .description = "The customer's managed services (Zetta and Postgres clusters) with the "
                       "operator's phase and in-cluster endpoint. Never includes credentials.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // Like every list_ capability it authorizes itself: read on the customer.
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const auto object = customer_object(input, context);
            if (!object) {
                co_return ApiResult::failure(
                    object.error().code, object.error().message, object.error().path);
            }
            if (context.acting_identity != "admin") {
                const auto decision = control::authorize(*context.store,
                                                         context.acting_identity,
                                                         *object,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    co_return store_failure(decision.error());
                }
                if (!decision->allowed) {
                    co_return ApiResult::failure("unauthorized",
                                                 "principal is not allowed to read the customer");
                }
            }
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto listed = context.workspaces->services(input.at("customer_id"));
            if (!listed) {
                co_return store_failure(listed.error());
            }
            json services = json::array();
            for (const control::ManagedService& service : *listed) {
                json value = control::service_json(service);
                // The operator reports Ready once it has written the Secret; the kubelet copies
                // it into lake's mount up to a minute later. Until then lake cannot connect.
                value["connected"] = context.workspaces->connection(service).has_value();
                services.push_back(std::move(value));
            }
            co_return ApiResult::success(
                {{"services", std::move(services)},
                 {"limit", context.workspaces->config().max_per_customer}});
        }});
    registry.add(Capability{
        .name = "create_service",
        .title = "Create a service",
        .description = "Ask the operator for a new service from the catalog (Postgres, Zetta, "
                       "Kafka, ClickHouse, TiDB, Cassandra, Valkey as a preview). "
                       "`instances` is 1-3 (default 1), `storage_gi` 1-100 (default 10).",
        .input_schema = customer_input_schema(
            {{"kind",
              {{"type", "string"},
               {"enum",
                [] {
                    json keys = json::array();
                    for (const control::ServiceKind& kind : control::service_kinds) {
                        keys.push_back(std::string(kind.key));
                    }
                    return keys;
                }()}}},
             {"name", {{"type", "string"}}},
             {"instances", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}},
             {"storage_gi", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
            json::array({"kind", "name"})),
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
            control::NewManagedService service;
            service.customer_id = input.at("customer_id");
            service.kind = input.at("kind");
            service.name = input.at("name");
            service.instances = input.value("instances", 1);
            service.storage_gi = input.value("storage_gi", 10);
            service.created_by_taap_id = context.acting_identity;
            if (control::find_service_kind(service.kind) == nullptr) {
                co_return ApiResult::failure(
                    "invalid_input", "kind is not in the service catalog", "/kind");
            }
            if (!valid_service_name(service.name)) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "name must start with a letter and use lowercase letters, digits and '-', at "
                    "most 31 characters",
                    "/name");
            }
            if (service.instances < 1 || service.instances > 3 || service.storage_gi < 1 ||
                service.storage_gi > 100) {
                co_return ApiResult::failure("invalid_input",
                                             "instances must be 1-3 and storage_gi 1-100");
            }
            const auto created = context.workspaces->create_service(std::move(service));
            if (!created) {
                co_return store_failure(created.error());
            }
            service_audit(context, "service.create", *created);
            co_return ApiResult::success(control::service_json(*created));
        }});
    registry.add(Capability{
        .name = "delete_service",
        .title = "Delete a service",
        .description = "Delete one of the customer's services. The operator removes the cluster "
                       "and its data, which cannot be recovered. `confirm` must be the service's "
                       "name.",
        .input_schema = customer_input_schema(
            {{"service_id", {{"type", "string"}}}, {"confirm", {{"type", "string"}}}},
            json::array({"service_id", "confirm"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found =
                context.workspaces->service(input.at("customer_id"), input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            if (input.at("confirm") != found->name) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must be the service name " + found->name, "/confirm");
            }
            service_audit(context, "service.delete", *found);
            const auto deleted =
                context.workspaces->delete_service(input.at("customer_id"), input.at("service_id"));
            if (!deleted) {
                co_return store_failure(deleted.error());
            }
            co_return ApiResult::success(control::service_json(*deleted));
        }});
    registry.add(Capability{
        .name = "service_connection",
        .title = "Service connection",
        .description = "The connection details of a Ready service, including its password: "
                       "what the operator delivered for its kind. Audited. A Zetta service's "
                       "admin token is never returned: lake calls it on the customer's behalf "
                       "through workspace_query and workspace_act.",
        .input_schema = customer_input_schema({{"service_id", {{"type", "string"}}}},
                                              json::array({"service_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto found =
                context.workspaces->service(input.at("customer_id"), input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            service_audit(context, "service.connection", *found);
            if (found->kind == "zetta") {
                co_return ApiResult::failure("invalid_input",
                                             "a Zetta service's admin token is never handed out; "
                                             "lake calls it for you",
                                             "/service_id");
            }
            const auto connection = context.workspaces->connection(*found);
            if (found->phase != "Ready" || !connection) {
                co_return ApiResult::failure(
                    "workspace_not_ready",
                    found->phase == "Ready"
                        ? "the service is ready and its connection is still being delivered to "
                          "lake; try again in a minute"
                        : "the service is not ready yet");
            }
            // Everything the operator delivered for this kind (docs/control-plane/services.md,
            // The catalog), for example Kafka's bootstrap and SASL settings.
            json value = *connection;
            value.erase("kind");
            value.erase("token");
            co_return ApiResult::success(std::move(value));
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
            auto state = co_await context.workspaces->request(input.at("customer_id"),
                                                              context.acting_identity);
            if (!state) {
                co_return ApiResult::failure("store_unavailable", state.error().message);
            }
            if (!*state) {
                co_return ApiResult::failure(state->error().code, state->error().message);
            }
            co_return ApiResult::success((*state)->to_json());
        }});
    registry.add(Capability{
        .name = "delete_workspace",
        .title = "Delete a workspace",
        .description =
            "Delete the customer's Zetta workspace. The operator removes its cluster, "
            "projects, data and models, which cannot be recovered. `confirm` must be the "
            "workspace id.",
        .input_schema =
            customer_input_schema({{"confirm", {{"type", "string"}}}}, json::array({"confirm"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            auto current = co_await context.workspaces->state(input.at("customer_id"));
            if (!current) {
                co_return ApiResult::failure("store_unavailable", current.error().message);
            }
            if (!*current) {
                co_return ApiResult::failure(current->error().code, current->error().message);
            }
            const std::string slug = (*current)->slug;
            if (input.at("confirm") != slug) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must be the workspace id " + slug, "/confirm");
            }
            set_audit(context, "workspace.delete", input, "");
            auto state = co_await context.workspaces->withdraw(input.at("customer_id"));
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
    register_service_capabilities(registry);
}

} // namespace lake::api
