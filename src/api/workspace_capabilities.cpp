#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <map>
#include <string>
#include <string_view>

#include <optional>

#include "lake/api/oauth.hpp"
#include "lake/api/pipeline.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

// The Zetta capabilities lake's pages may call on a customer's workspace (docs/api/workspaces.md).
// Reads need `read` on the customer, actions `write`. Anything else is refused, so a lake caller
// can never reach Zetta's administrative surface (sql, users, cloud provisioning) through lake.
constexpr std::array<std::string_view, 32> workspace_reads = {"server_info",
                                                              "score",
                                                              "explain",
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
                                                              "get_job",
                                                              "get_trace",
                                                              "list_traces",
                                                              "list_source_tables",
                                                              "list_dataset_generators",
                                                              "list_agents",
                                                              "get_agent",
                                                              "agent_training_status",
                                                              // The workspace's own pub/sub
                                                              // (docs/api/agent-bus.md): only
                                                              // topics created exposed.
                                                              "list_topics",
                                                              "receive_events",
                                                              // What the engine can run
                                                              // (zetta#1061) and how to
                                                              // featurize for it (zetta#1062).
                                                              "list_methodologies",
                                                              "analyze_dataset",
                                                              "plan_features"};
constexpr std::array<std::string_view, 34> workspace_actions = {"create_project",
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
                                                                "explain",
                                                                "save_graph",
                                                                "deploy",
                                                                "import_n8n",
                                                                "compile_n8n",
                                                                "compile_python",
                                                                "promote_dataset",
                                                                "generate_dataset",
                                                                "create_agent",
                                                                "update_agent",
                                                                "delete_agent",
                                                                "set_training_schedule",
                                                                "agent_run",
                                                                "create_topic",
                                                                "drop_topic",
                                                                "publish_event",
                                                                // A read in Zetta, called with GET,
                                                                // but it loads a model and spends
                                                                // compute: `write` in lake.
                                                                "chat"};

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
            if (read ? !workspace_read_allowed(capability)
                     : !workspace_action_allowed(capability)) {
                co_return ApiResult::failure("invalid_input",
                                             capability + " is not a workspace " +
                                                 (read ? "read" : "action"),
                                             "/capability");
            }
            set_audit(context, read ? "workspace.query" : "workspace.act", input, capability);
            auto called = co_await context.workspaces->call(input.at("customer_id"),
                                                            capability,
                                                            workspace_uses_get(capability),
                                                            input.value("input", json::object()));
            if (!called) {
                co_return ApiResult::failure("store_unavailable", called.error().message);
            }
            if (!*called) {
                // Point at the field the workspace names (lake#256): "required property 'x' not
                // found in object" is about /input/x.
                const std::string message = called->error().message;
                std::string path;
                if (const auto at = message.find("property '"); at != std::string::npos) {
                    const auto end = message.find('\'', at + 10);
                    if (end != std::string::npos) {
                        path = "/input/" + message.substr(at + 10, end - at - 10);
                    }
                }
                co_return ApiResult::failure(called->error().code, message, path);
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
            if ((service.kind == "sqlite" || service.kind == "duckdb") && service.instances != 1) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "a " + std::string(service.kind == "sqlite" ? "SQLite" : "DuckDB") +
                        " service has exactly one instance: one process "
                        "owns its database file",
                    "/instances");
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
            // Zetta was refused above; for every other kind the token, where there is one (SQLite),
            // is the credential the customer asked for.
            json value = *connection;
            value.erase("kind");
            co_return ApiResult::success(std::move(value));
        }});
}

void store_audit(ApiContext& context, std::string kind, const control::ManagedStore& store) {
    context.audit_kind = std::move(kind);
    context.audit_details = {{"customer_id", store.customer_id},
                             {"store_id", store.id},
                             {"store_type", store.type},
                             {"store_provider", store.provider},
                             {"store_name", store.name}};
}

// A read on the customer, checked in the handler as list_services does.
std::optional<ApiResult> refuse_unless_reader(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity != "admin") {
        const auto decision = control::authorize(
            *context.store, context.acting_identity, *object, "read", context.authorization_cache);
        if (!decision) {
            return store_failure(decision.error());
        }
        if (!decision->allowed) {
            return ApiResult::failure("unauthorized",
                                      "principal is not allowed to read the customer");
        }
    }
    return std::nullopt;
}

json store_types_enum() {
    json types = json::array();
    for (const control::StoreProvider& entry : control::store_providers) {
        if (std::find(types.begin(), types.end(), std::string(entry.type)) == types.end()) {
            types.push_back(std::string(entry.type));
        }
    }
    return types;
}

json store_providers_enum() {
    json providers = json::array();
    for (const control::StoreProvider& entry : control::store_providers) {
        if (std::find(providers.begin(), providers.end(), std::string(entry.provider)) ==
            providers.end()) {
            providers.push_back(std::string(entry.provider));
        }
    }
    return providers;
}

// Storage (docs/control-plane/storage.md, lake#196): the provider table and the customer's stores.
void register_store_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_store_providers",
        .title = "List store providers",
        .description = "Every type × provider pair a store can be (object, filesystem, "
                       "network_filesystem, volume on cluster, r2, s3, b2, azure, gcs, lake, "
                       "zetta, nfs), whether this lake offers it, and why not when it does not.",
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
            if (auto refused = refuse_unless_reader(input, context)) {
                co_return std::move(*refused);
            }
            json providers = json::array();
            for (const control::StoreProvider& entry : control::store_providers) {
                json value{{"type", std::string(entry.type)},
                           {"provider", std::string(entry.provider)},
                           {"label", std::string(entry.label)},
                           {"available", entry.available}};
                if (!entry.available) {
                    value["unavailable_reason"] = std::string(entry.unavailable_reason);
                }
                providers.push_back(std::move(value));
            }
            co_return ApiResult::success({{"providers", std::move(providers)}});
        }});
    registry.add(Capability{
        .name = "list_stores",
        .title = "List stores",
        .description = "The customer's stores (buckets, filesystems, network filesystems and "
                       "volumes) with the operator's phase, message and endpoint. Never includes "
                       "credentials.",
        .input_schema = customer_input_schema(json::object(), json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto refused = refuse_unless_reader(input, context)) {
                co_return std::move(*refused);
            }
            if (!context.workspaces) {
                co_return unavailable();
            }
            const auto listed = context.workspaces->stores(input.at("customer_id"));
            if (!listed) {
                co_return store_failure(listed.error());
            }
            json stores = json::array();
            for (const control::ManagedStore& store : *listed) {
                json value = control::store_json(store);
                // The operator writes the connection before Ready; the kubelet copies it into
                // lake's mount up to a minute later.
                value["connected"] = context.workspaces->store_connection(store).has_value();
                stores.push_back(std::move(value));
            }
            co_return ApiResult::success({{"stores", std::move(stores)}});
        }});
    registry.add(Capability{
        .name = "create_store",
        .title = "Create a store",
        .description = "Ask the operator for a new store: a `type` and `provider` pair that "
                       "list_store_providers reports available (today volume on cluster), a "
                       "`name`, and optionally `size_gi` 1-100 (default 10) and `region`. Private "
                       "by default.",
        .input_schema = customer_input_schema(
            {{"type", {{"type", "string"}, {"enum", store_types_enum()}}},
             {"provider", {{"type", "string"}, {"enum", store_providers_enum()}}},
             {"name", {{"type", "string"}}},
             {"size_gi", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
             {"region", {{"type", "string"}}}},
            json::array({"type", "provider", "name"})),
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
            control::NewManagedStore store;
            store.customer_id = input.at("customer_id");
            store.type = input.at("type");
            store.provider = input.at("provider");
            store.name = input.at("name");
            store.size_gi = input.value("size_gi", 10);
            store.region = input.value("region", std::string());
            store.created_by_taap_id = context.acting_identity;
            if (!valid_service_name(store.name)) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "name must start with a letter and use lowercase letters, digits and '-', at "
                    "most 31 characters",
                    "/name");
            }
            if (store.size_gi < 1 || store.size_gi > 100) {
                co_return ApiResult::failure("invalid_input", "size_gi must be 1-100", "/size_gi");
            }
            const auto created = context.workspaces->create_store(std::move(store));
            if (!created) {
                co_return store_failure(created.error());
            }
            store_audit(context, "store.create", *created);
            co_return ApiResult::success(control::store_json(*created));
        }});
    registry.add(Capability{
        .name = "delete_store",
        .title = "Delete a store",
        .description = "Delete one of the customer's stores. The operator removes it and its "
                       "data, which cannot be recovered. `confirm` must be the store's name.",
        .input_schema = customer_input_schema(
            {{"store_id", {{"type", "string"}}}, {"confirm", {{"type", "string"}}}},
            json::array({"store_id", "confirm"})),
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
                context.workspaces->store(input.at("customer_id"), input.at("store_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            if (input.at("confirm") != found->name) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must be the store name " + found->name, "/confirm");
            }
            store_audit(context, "store.delete", *found);
            const auto deleted =
                context.workspaces->delete_store(input.at("customer_id"), input.at("store_id"));
            if (!deleted) {
                co_return store_failure(deleted.error());
            }
            co_return ApiResult::success(control::store_json(*deleted));
        }});
    registry.add(Capability{
        .name = "store_connection",
        .title = "Store connection",
        .description = "How to reach a Ready store: what the operator delivered for its type and "
                       "provider (for a cluster volume, the claim, namespace, size and storage "
                       "class). Audited.",
        .input_schema =
            customer_input_schema({{"store_id", {{"type", "string"}}}}, json::array({"store_id"})),
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
                context.workspaces->store(input.at("customer_id"), input.at("store_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            store_audit(context, "store.connection", *found);
            const auto connection = context.workspaces->store_connection(*found);
            if (found->desired != "present" || found->phase != "Ready" || !connection) {
                co_return ApiResult::failure(
                    "workspace_not_ready",
                    found->desired == "present" && found->phase == "Ready"
                        ? "the store is ready and its connection is still being delivered to "
                          "lake; try again in a minute"
                        : "the store is not ready");
            }
            co_return ApiResult::success(*connection);
        }});
}

// One Zetta call in the customer's workspace, as an ApiResult.
liquid::runtime::task<ApiResult>
zetta(ApiContext& context, std::string customer, std::string capability, bool read, json input) {
    auto called =
        co_await context.workspaces->call(customer, std::move(capability), read, std::move(input));
    if (!called) {
        co_return ApiResult::failure("store_unavailable", called.error().message);
    }
    if (!*called) {
        co_return ApiResult::failure((*called).error().code, (*called).error().message);
    }
    co_return ApiResult::success(**called);
}

ApiResult take(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

} // namespace

// One validated pipeline edit (docs/design/zetta-parity.md): read the graph, apply the edit with
// lake::api::pipeline, and save the whole graph back. The page's forms, REST and MCP all edit
// through this, so no client has to build a save_graph body or can save a broken graph.
void register_pipeline_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "edit_pipeline",
        .title = "Edit a pipeline",
        .description =
            "Apply one edit to a pipeline in the customer's workspace and save it: `op` is "
            "add_step (kind, name), rename_step (step, name), kind_step (step, kind), remove_step "
            "(step), add_edge (from, to, kind data_flow|control), remove_edge (from, to), "
            "set_attribute (step, name, value; a null value removes it) or rename (name). Or pass "
            "`edits`, a list of such edits, to apply them in order as one save (all or nothing), "
            "as a Project Agent proposal is applied. The result must keep unique step ids, known "
            "kinds, edges between existing steps and no cycle. A saved pipeline is proposed again "
            "and must be re-approved to run.",
        .input_schema = customer_input_schema({{"project", {{"type", "string"}}},
                                               {"graph", {{"type", "string"}}},
                                               {"op",
                                                {{"type", "string"},
                                                 {"enum",
                                                  json::array({"add_step",
                                                               "rename_step",
                                                               "kind_step",
                                                               "remove_step",
                                                               "add_edge",
                                                               "remove_edge",
                                                               "set_attribute",
                                                               "rename"})}}},
                                               {"step", {{"type", "string"}}},
                                               {"kind", {{"type", "string"}}},
                                               {"name", {{"type", "string"}}},
                                               {"from", {{"type", "string"}}},
                                               {"to", {{"type", "string"}}},
                                               {"value", json::object()},
                                               {"edits",
                                                {{"type", "array"},
                                                 {"items", {{"type", "object"}}}}}},
                                              json::array({"project", "graph"})),
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
            const std::string customer = input.at("customer_id");
            const std::string project = input.at("project");
            const std::string graph = input.at("graph");
            set_audit(context, "pipeline.edit", input, "save_graph");
            auto read = co_await context.workspaces->call(
                customer, "get_graph", true, {{"project", project}, {"graph", graph}});
            if (!read) {
                co_return ApiResult::failure("store_unavailable", read.error().message);
            }
            if (!*read) {
                co_return ApiResult::failure((*read).error().code, (*read).error().message);
            }
            auto current = pipeline::from_json(**read);
            if (!current) {
                co_return ApiResult::failure("store_error", current.error());
            }
            const std::size_t before = current->steps.size();
            const json edits = input.value("edits", json());
            if (edits.is_null() && !input.contains("op")) {
                co_return ApiResult::failure("invalid_input", "give an op or a list of edits", "/op");
            }
            auto edited = edits.is_null()
                              ? pipeline::apply(*current, pipeline::edit_from_json(input))
                              : pipeline::apply_all(*current, edits);
            if (!edited) {
                co_return ApiResult::failure("invalid_input", edited.error());
            }
            json save{{"project", project},
                      {"graph", current->graph_id.empty() ? graph : current->graph_id},
                      {"nodes", pipeline::steps_json(*edited)},
                      {"edges", pipeline::edges_json(*edited)}};
            if (edited->name != current->name) {
                save["new_name"] = edited->name;
            }
            auto saved = co_await context.workspaces->call(customer, "save_graph", false, save);
            if (!saved) {
                co_return ApiResult::failure("store_unavailable", saved.error().message);
            }
            if (!*saved) {
                co_return ApiResult::failure((*saved).error().code, (*saved).error().message);
            }
            json value = **saved;
            value["name"] = edited->name;
            if (edited->steps.size() > before) {
                value["step"] = edited->steps.back().id;
            }
            co_return ApiResult::success(std::move(value));
        }});
}

// Search one published index of the customer's workspace (#187). The key an outside caller holds
// is a lake API token scoped to search_index alone: it can query published indexes and nothing
// else, and never sees the workspace's admin token. An unpublished index is refused.
void register_search_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "search_index",
        .title = "Search a published index",
        .description = "Search one published search index in the customer's workspace. `query` is "
                       "the text, `field` optionally limits it to one indexed field, `top_k` caps "
                       "the hits (default 10). Answers hits as {id, score}. An index that is not "
                       "published is refused; publish it with workspace_act expose_index.",
        .input_schema = customer_input_schema(
            {{"index", {{"type", "string"}}},
             {"query", {{"type", "string"}}},
             {"field", {{"type", "string"}}},
             {"top_k", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
            json::array({"index", "query"})),
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
            const std::string customer = input.at("customer_id");
            const std::string index = input.at("index");
            set_audit(context, "index.search", input, "search");
            auto described = co_await context.workspaces->call(
                customer, "get_index", true, {{"index_id", index}});
            if (!described) {
                co_return ApiResult::failure("store_unavailable", described.error().message);
            }
            if (!*described) {
                co_return ApiResult::failure((*described).error().code,
                                             (*described).error().message);
            }
            if (!(**described).value("exposed", false)) {
                co_return ApiResult::failure(
                    "invalid_input", "index " + index + " is not published", "/index");
            }
            json search{{"target", index},
                        {"query", input.at("query")},
                        {"top_k", input.value("top_k", 10)}};
            if (const std::string field = input.value("field", std::string()); !field.empty()) {
                search["field"] = field;
            }
            auto found = co_await context.workspaces->call(customer, "search", true, search);
            if (!found) {
                co_return ApiResult::failure("store_unavailable", found.error().message);
            }
            if (!*found) {
                co_return ApiResult::failure((*found).error().code, (*found).error().message);
            }
            co_return ApiResult::success(**found);
        }});
}

// Predict with one published model (lake#250): the public route a customer calls from outside lake,
// with a lake key. The model must be published (expose_model); rows are keyed by the model's own
// feature columns and answered ranked by risk with what drives it (Zetta's `score`).
void register_predict_capability(Registry& registry) {
    registry.add(Capability{
        .name = "predict",
        .title = "Predict with a published model",
        .description =
            "Score rows through one published model of the customer's workspace: `model` is the "
            "model's id (Ship → Models), `rows` the records to score, each keyed by the model's "
            "feature columns. Answers every row ranked by risk with its score and the features "
            "that drive it. A model that is not published is refused; publish it with "
            "workspace_act expose_model.",
        .input_schema = customer_input_schema(
            {{"model", {{"type", "string"}}},
             {"rows", {{"type", "array"}, {"items", {{"type", "object"}}}, {"minItems", 1}}},
             {"top_k", {{"type", "integer"}, {"minimum", 0}}}},
            json::array({"model", "rows"})),
        .output_schema = {{"type", "object"}},
        // Rows go in a JSON body, so it is POST; it changes nothing and is authorized as a read.
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces) {
                co_return unavailable();
            }
            const std::string customer = input.at("customer_id");
            const std::string model = input.at("model");
            set_audit(context, "model.predict", input, "score");
            auto described = co_await context.workspaces->call(
                customer, "get_model", true, {{"model_id", model}});
            if (!described) {
                co_return ApiResult::failure("store_unavailable", described.error().message);
            }
            if (!*described) {
                co_return ApiResult::failure(
                    "not_found", "no model " + model + " (use its id from Ship → Models)", "/model");
            }
            if (!(**described).value("exposed", false)) {
                co_return ApiResult::failure(
                    "invalid_input", "model " + model + " is not published", "/model");
            }
            json score{{"model", model}, {"rows", input.at("rows")}};
            if (input.contains("top_k")) {
                score["top_k"] = input["top_k"];
            }
            auto scored = co_await context.workspaces->call(customer, "score", true, score);
            if (!scored) {
                co_return ApiResult::failure("store_unavailable", scored.error().message);
            }
            if (!*scored) {
                co_return ApiResult::failure((*scored).error().code, (*scored).error().message);
            }
            co_return ApiResult::success(**scored);
        }});
}

// The Zetta connector for a service kind, from the service's delivered connection, or nothing when
// Zetta cannot read that kind yet. Zetta's connectors today: postgres and snowflake.
std::optional<json> zetta_connector(const control::ManagedService& service,
                                    const json& connection) {
    if (service.kind == "postgres") {
        json connector{
            {"type", "postgres"},
            {"host", connection.value("host", std::string())},
            {"port",
             connection.contains("port")
                 ? (connection.at("port").is_string() ? connection.at("port").get<std::string>()
                                                      : connection.at("port").dump())
                 : std::string("5432")},
            {"user", connection.value("user", std::string())},
            {"database", connection.value("database", std::string())},
            {"token", connection.value("password", std::string())},
            // CloudNativePG serves TLS in the cluster with its own CA.
            {"sslmode", "require"}};
        return connector;
    }
    return std::nullopt;
}

// Train → Data → Connect one of your services (lake#188): make a Zetta connection to one of the
// customer's own Ready services from the connection the operator delivered, so nobody types or
// sees its password. Saving under an existing name replaces it, which is how Refresh works.
void register_connect_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "connect_service",
        .title = "Connect a service to the workspace",
        .description = "Create (or refresh) a Zetta connection in the customer's workspace to one "
                       "of the customer's own Ready services, filled from the service's delivered "
                       "connection. Postgres today; other kinds answer invalid_input until Zetta "
                       "can read them. `name` defaults to the service's name.",
        .input_schema = customer_input_schema(
            {{"service_id", {{"type", "string"}}}, {"name", {{"type", "string"}}}},
            json::array({"service_id"})),
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
            const std::string customer = input.at("customer_id");
            const auto found = context.workspaces->service(customer, input.at("service_id"));
            if (!found) {
                co_return store_failure(found.error());
            }
            service_audit(context, "service.connect", *found);
            const auto connection = context.workspaces->connection(*found);
            if (found->phase != "Ready" || !connection) {
                co_return ApiResult::failure("workspace_not_ready", "the service is not ready yet");
            }
            const auto connector = zetta_connector(*found, *connection);
            if (!connector) {
                co_return ApiResult::failure("invalid_input",
                                             "the workspace cannot read a " + found->kind +
                                                 " service yet",
                                             "/service_id");
            }
            const std::string name = input.value("name", found->name);
            auto created = co_await context.workspaces->call(
                customer, "create_connection", false, {{"name", name}, {"connector", *connector}});
            if (!created) {
                co_return ApiResult::failure("store_unavailable", created.error().message);
            }
            if (!*created) {
                co_return ApiResult::failure((*created).error().code, (*created).error().message);
            }
            json value = **created;
            value["service_id"] = found->id;
            value["service_name"] = found->name;
            co_return ApiResult::success(std::move(value));
        }});
}

// What the pipeline page's forms chain together, as capabilities, so REST and MCP clients get the
// same flows (lake#189): read a pipeline with its step status and runs, gate then run it, and
// create one empty, from n8n or from Python. The catalog of service kinds is readable too.
void register_parity_capabilities(Registry& registry) {
    const json project_graph{{"project", {{"type", "string"}}}, {"graph", {{"type", "string"}}}};
    registry.add(Capability{
        .name = "get_pipeline",
        .title = "Get a pipeline",
        .description =
            "One pipeline in the customer's workspace: its steps and edges, the approval "
            "state, each step's status in the latest run, and the pipeline's runs, "
            "newest first.",
        .input_schema = customer_input_schema(project_graph, json::array({"project", "graph"})),
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
            const std::string customer = input.at("customer_id");
            ApiResult graph = take(
                co_await zetta(context,
                               customer,
                               "get_graph",
                               true,
                               {{"project", input.at("project")}, {"graph", input.at("graph")}}));
            if (!graph.ok) {
                co_return graph;
            }
            json value = graph.value;
            const std::string name = value.value("name", std::string());
            const std::string id = value.value("graph_id", std::string());
            json runs = json::array();
            const ApiResult listed =
                take(co_await zetta(context, customer, "list_runs", true, json::object()));
            if (listed.ok) {
                for (const json& run : listed.value.value("runs", json::array())) {
                    const std::string of = run.value("graph", std::string());
                    if (of == name || of == id) {
                        runs.push_back(run);
                    }
                }
            }
            std::sort(runs.begin(), runs.end(), [](const json& a, const json& b) {
                return a.value("created", std::string()) > b.value("created", std::string());
            });
            json status = json::object();
            if (!runs.empty()) {
                const ApiResult latest =
                    take(co_await zetta(context,
                                        customer,
                                        "get_run",
                                        true,
                                        {{"run_id", runs.front().value("id", std::string())}}));
                if (latest.ok) {
                    for (const json& step : latest.value.value("lineage", json::object())
                                                .value("steps", json::array())) {
                        if (step.is_string()) {
                            status[step.get<std::string>()] =
                                latest.value.value("status", std::string());
                        }
                    }
                }
            }
            value["step_status"] = std::move(status);
            value["runs"] = std::move(runs);
            co_return ApiResult::success(std::move(value));
        }});
    registry.add(Capability{
        .name = "run_pipeline",
        .title = "Check and run a pipeline",
        .description =
            "Run Zetta's correctness checks on an approved pipeline and, when they pass, "
            "run it. Optional `dataset_path` (database.table), `holdout_fraction` and "
            "`model_name`. Answers {gate, run}; a failing gate answers {gate} and runs "
            "nothing.",
        .input_schema = customer_input_schema(
            {{"project", {{"type", "string"}}},
             {"graph", {{"type", "string"}}},
             {"dataset_path", {{"type", "string"}}},
             {"holdout_fraction", {{"type", "number"}, {"minimum", 0}, {"maximum", 0.9}}},
             {"model_name", {{"type", "string"}}}},
            json::array({"project", "graph"})),
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
            const std::string customer = input.at("customer_id");
            set_audit(context, "pipeline.run", input, "run");
            const json target{{"project", input.at("project")}, {"graph", input.at("graph")}};
            const ApiResult gate = take(co_await zetta(context, customer, "gate", false, target));
            if (!gate.ok) {
                co_return gate;
            }
            if (gate.value.value("block_deploy", false)) {
                co_return ApiResult::success({{"gate", gate.value}});
            }
            json run = target;
            for (const char* key : {"dataset_path", "holdout_fraction", "model_name"}) {
                if (input.contains(key)) {
                    run[key] = input.at(key);
                }
            }
            const ApiResult ran = take(co_await zetta(context, customer, "run", false, run));
            if (!ran.ok) {
                co_return ran;
            }
            co_return ApiResult::success({{"gate", gate.value}, {"run", ran.value}});
        }});
    registry.add(Capability{
        .name = "create_pipeline",
        .title = "Create a pipeline",
        .description = "Create a pipeline in a project: `from` empty (one connect step to build "
                       "on), n8n (`content` is an n8n workflow export) or python (`content` is a "
                       "simulated-Python pipeline Zetta lowers without running it). The new "
                       "pipeline is proposed and must be approved before it runs.",
        .input_schema = customer_input_schema(
            {{"project", {{"type", "string"}}},
             {"name", {{"type", "string"}}},
             {"from", {{"type", "string"}, {"enum", json::array({"empty", "n8n", "python"})}}},
             {"content", {{"type", "string"}}}},
            json::array({"project", "from"})),
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
            const std::string customer = input.at("customer_id");
            const std::string project = input.at("project");
            const std::string from = input.at("from");
            const std::string name = input.value("name", std::string());
            set_audit(context,
                      "pipeline.create",
                      input,
                      from == "empty" ? "save_graph" : "compile_" + from);
            if (from == "empty") {
                if (name.empty()) {
                    co_return ApiResult::failure(
                        "invalid_input", "an empty pipeline needs a name", "/name");
                }
                co_return take(co_await zetta(
                    context,
                    customer,
                    "save_graph",
                    false,
                    {{"project", project},
                     {"graph", name},
                     {"nodes",
                      json::array(
                          {{{"id", "connect-1"}, {"kind", "connect"}, {"name", "Source"}}})},
                     {"edges", json::array()}}));
            }
            const std::string content = input.value("content", std::string());
            if (content.empty()) {
                co_return ApiResult::failure("invalid_input", "content is required", "/content");
            }
            if (from == "n8n") {
                const ApiResult imported =
                    take(co_await zetta(context,
                                        customer,
                                        "import_n8n",
                                        false,
                                        {{"project", project}, {"content", content}}));
                if (!imported.ok) {
                    co_return imported;
                }
                json compile{{"project", project},
                             {"input_id", imported.value.value("input_id", std::string())}};
                if (!name.empty()) {
                    compile["name"] = name;
                }
                co_return take(co_await zetta(context, customer, "compile_n8n", false, compile));
            }
            json compile{{"project", project}, {"source", content}};
            if (!name.empty()) {
                compile["name"] = name;
            }
            co_return take(co_await zetta(context, customer, "compile_python", false, compile));
        }});
    registry.add(Capability{
        .name = "list_service_kinds",
        .title = "List service kinds",
        .description = "Every kind of service a customer can create: key, label and how long a new "
                       "one usually takes to become ready.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = [](const json&,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            return "principal/" + context.acting_identity;
        },
        // The catalog is the same for everyone signed in; nothing to filter per caller.
        .authorizes_candidates = true,
        .handler =
            [](const json&, ApiContext&) {
                json kinds = json::array();
                for (const control::ServiceKind& kind : control::service_kinds) {
                    kinds.push_back({{"kind", std::string(kind.key)},
                                     {"label", std::string(kind.label)},
                                     {"ready_seconds", kind.ready_seconds}});
                }
                return ApiResult::success({{"kinds", std::move(kinds)}});
            }});
}

// Empty when `statement` is one read-only query: a SELECT or WITH, with at most a trailing
// semicolon. Otherwise why not. A coarse guard in front of Zetta's own SQL, which also runs DDL and
// administration; the workspace is the customer's own cluster, so reading its data is fine.
std::string read_only_statement_problem(std::string_view statement) {
    std::string text(statement);
    while (!text.empty() &&
           (std::isspace(static_cast<unsigned char>(text.back())) != 0 || text.back() == ';')) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::string head;
    for (std::size_t index = start;
         index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) != 0;
         ++index) {
        head += static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
    }
    if (head != "select" && head != "with") {
        return "only a SELECT or WITH query can run here";
    }
    bool quoted = false;
    for (const char character : text) {
        if (character == '\'') {
            quoted = !quoted;
        } else if (character == ';' && !quoted) {
            return "run one statement at a time";
        }
    }
    return {};
}

// Train → Data → Query (Zetta parity phase 2, lake#181): read-only SQL over the workspace's data.
void register_sql_capability(Registry& registry) {
    registry.add(Capability{
        .name = "workspace_sql",
        .title = "Query the workspace",
        .description = "Run one read-only SQL query over the customer's workspace data and answer "
                       "{columns, rows}. The workspace speaks a SQL subset: SELECT <columns or *> "
                       "FROM <database>.<table> [WHERE <column> <op> <value>] [GROUP BY <column>] "
                       "[ORDER BY <column> [DESC], ...] [LIMIT n] [OFFSET n]; one WHERE "
                       "comparison, aggregates (COUNT, SUM, AVG, MIN, MAX) only with GROUP BY, no "
                       "joins, expressions or subqueries. list_datasets (through workspace_query) "
                       "names the tables. Anything but a read is refused; the workspace's "
                       "administrative SQL is not reachable.",
        .input_schema = customer_input_schema(
            {{"statement", {{"type", "string"}}}, {"database", {{"type", "string"}}}},
            json::array({"statement"})),
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
            const std::string statement = input.at("statement");
            if (const std::string problem = read_only_statement_problem(statement);
                !problem.empty()) {
                co_return ApiResult::failure("invalid_input", problem, "/statement");
            }
            set_audit(context, "workspace.sql", input, "sql");
            json query{{"statement", statement}};
            if (const std::string database = input.value("database", std::string());
                !database.empty()) {
                query["database"] = database;
            }
            co_return take(co_await zetta(context, input.at("customer_id"), "sql", true, query));
        }});
}

namespace {

// The workspace's answer to one Zetta call, or the failure as an ApiResult. Not std::expected:
// a liquid task already wraps its value in one, and nesting them makes every co_return ambiguous.
struct ZettaAnswer {
    json value;
    std::optional<ApiResult> failure;

    explicit operator bool() const {
        return !failure;
    }
    const json* operator->() const {
        return &value;
    }
    json& operator*() {
        return value;
    }
    ApiResult error() const {
        return *failure;
    }
};

liquid::runtime::task<ZettaAnswer>
zetta_call(ApiContext& context, std::string customer_id, std::string capability, json input) {
    auto called = co_await context.workspaces->call(
        customer_id, capability, workspace_uses_get(capability), std::move(input));
    if (!called) {
        co_return ZettaAnswer{json(),
                              ApiResult::failure("store_unavailable", called.error().message)};
    }
    if (!*called) {
        co_return ZettaAnswer{json(),
                              ApiResult::failure(called->error().code, called->error().message)};
    }
    co_return ZettaAnswer{std::move(**called), std::nullopt};
}

ZettaAnswer answered(std::expected<ZettaAnswer, liquid::error> completed) {
    if (!completed) {
        return ZettaAnswer{json(), ApiResult::failure("store_error", completed.error().message)};
    }
    return std::move(*completed);
}

json launch_json(const control::AgentLaunch& launch) {
    return {{"id", launch.id},
            {"customer_id", launch.customer_id},
            {"agent_id", launch.agent_id},
            {"agent_name", launch.agent_name},
            {"mode", launch.mode},
            {"trigger", launch.trigger},
            {"input", launch.input},
            {"slug", launch.slug},
            {"desired", launch.desired},
            {"phase", launch.phase},
            {"message", launch.message},
            {"exit_code", launch.exit_code ? json(*launch.exit_code) : json(nullptr)},
            {"result", launch.result},
            {"created_at", launch.created_at},
            {"created_by_taap_id", launch.created_by_taap_id}};
}

// A background turn as callers see it (lake#269). A finished Project Agent proposal names the
// pipeline page where it is reviewed and applied.
json task_json(const control::AgentTask& task) {
    const std::string status =
        task.phase.empty() ? std::string("pending") : [&] {
            std::string lower = task.phase;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return lower;
        }();
    json out{{"task_id", task.id},
             {"agent", task.agent},
             {"kind", task.kind},
             {"status", status},
             {"message", task.message},
             {"created_at", task.created_at},
             {"finished_at", task.finished_at},
             {"answer", task.result}};
    if (!task.conversation_id.empty()) {
        out["conversation_id"] = task.conversation_id;
    }
    const json proposal = task.result.is_object()
                              ? task.result.value("result", json::object()).value("proposal", json())
                              : json();
    if (task.agent == "agent-project" && proposal.is_object()) {
        out["review_url"] = "/design/pipeline?customer_id=" + task.customer_id + "&project=" +
                            oauth_url_encode(proposal.value("project", std::string())) +
                            "&graph=" + oauth_url_encode(proposal.value("graph", std::string())) +
                            "&task=" + task.id;
    }
    return out;
}

// A finished Discover scan's Opportunities become rows, once (lake#269): what Auto-Suggest
// suggested (new) and what it declined (not_yet), in its order.
void settle_discover(ApiContext& context, const control::AgentTask& task) {
    if (task.kind != "discover" || task.phase != "Succeeded" || !context.store ||
        !task.result.is_object()) {
        return;
    }
    const json profile = task.result.value("result", json::object())
                             .value("project_profile", json::object());
    std::vector<control::Opportunity> rows;
    int rank = 0;
    for (const json& item : profile.value("suggested", json::array())) {
        rows.push_back({.customer_id = task.customer_id,
                        .task_id = task.id,
                        .rank = ++rank,
                        .intent = item.value("intent", std::string()),
                        .solution = item.value("solution", std::string()),
                        .methodology = item.value("methodology", std::string()),
                        .why = item.value("why_this_data_supports_it", std::string()),
                        .payload = item,
                        .status = "new"});
    }
    for (const json& item : profile.value("declined", json::array())) {
        rows.push_back({.customer_id = task.customer_id,
                        .task_id = task.id,
                        .rank = ++rank,
                        .intent = item.value("intent", std::string()),
                        .why = item.value("reason", std::string()),
                        .payload = item,
                        .status = "not_yet"});
    }
    context.store->record_opportunities(rows);
}

json opportunity_json(const control::Opportunity& row) {
    return {{"id", row.id},
            {"task_id", row.task_id},
            {"rank", row.rank},
            {"intent", row.intent},
            {"solution", row.solution},
            {"methodology", row.methodology},
            {"why", row.why},
            {"status", row.status},
            {"project", row.project},
            {"created_at", row.created_at},
            {"detail", row.payload}};
}

json schedule_json(const control::AgentCronSchedule& schedule) {
    return {{"id", schedule.id},
            {"customer_id", schedule.customer_id},
            {"agent_id", schedule.agent_id},
            {"agent_name", schedule.agent_name},
            {"cron", schedule.cron},
            {"time_zone", schedule.time_zone},
            {"description", schedule.description},
            {"input", schedule.input},
            {"slug", schedule.slug},
            {"desired", schedule.desired},
            {"phase", schedule.phase},
            {"message", schedule.message},
            {"last_schedule_at", schedule.last_schedule_at},
            {"created_at", schedule.created_at},
            {"created_by_taap_id", schedule.created_by_taap_id}};
}

// Read on the customer, for the list_ capabilities, which authorize themselves.
std::optional<ApiResult> authorize_customer_read(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read the customer");
    }
    return std::nullopt;
}

void agent_audit(ApiContext& context,
                 std::string kind,
                 const std::string& customer_id,
                 json details) {
    context.audit_kind = std::move(kind);
    context.audit_details = std::move(details);
    context.audit_details["customer_id"] = customer_id;
}

// Another capability, called as the same person with its own authorization and audit (a copy of
// the context, so the outer call's audit record is left as it is).
liquid::runtime::task<ApiResult> call_capability(ApiContext& context, std::string name, json input) {
    if (context.registry == nullptr) {
        co_return ApiResult::failure("unavailable", "no registry to call " + name);
    }
    ApiContext inner = context;
    inner.audit_kind.clear();
    inner.audit_details = json();
    auto done = co_await context.registry->call_async(name, input, inner);
    co_return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

ApiResult called(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

std::string base64(std::string_view bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (rest == 2) {
            n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        }
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += rest == 2 ? alphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// A name for something made from free text: lower case, words joined by '-', at most `limit`.
std::string slug_of(std::string_view text, std::size_t limit) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
        if (out.size() >= limit) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

// The files an AgentCreator proposal carries, with instructions.md written from the agent when it
// is missing.
json proposal_files(const json& proposal) {
    const json agent = proposal.value("agent", json::object());
    json files = json::array();
    bool instructions = false;
    for (const json& file : proposal.value("files", json::array())) {
        instructions = instructions || file.value("path", std::string()) == "instructions.md";
        files.push_back({{"path", file.value("path", std::string())},
                         {"contents", file.value("contents", std::string())}});
    }
    if (!instructions) {
        std::string text = "# " + agent.value("name", std::string()) + "\n\n" +
                           agent.value("purpose", std::string()) + "\n";
        if (!agent.value("manifesto", std::string()).empty()) {
            text += "\n## How you work\n\n" + agent.value("manifesto", std::string()) + "\n";
        }
        if (!agent.value("goal", std::string()).empty()) {
            text += "\n## Your goal\n\n" + agent.value("goal", std::string()) + "\n";
        }
        files.push_back({{"path", "instructions.md"}, {"contents", text}});
    }
    return files;
}

} // namespace

// The agent capabilities that were page-only (UX review 2026-09-27): creating an agent from an
// approved AgentCreator proposal, turning chosen Opportunities into projects, and reading an Ask
// Orama conversation back. The pages call these, so REST, MCP and the site do the same thing.
namespace {

// A design field as text: the model writes null (or a number) as readily as a string.
std::string design_text_field(const json& design, const char* key) {
    const auto found = design.find(key);
    return found != design.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// The one table a Project Agent design trains on: `dataset`, else the first of
// `training_sources` or `datasets` (the agent's build output names it either way).
std::string design_dataset(const json& design) {
    if (const std::string dataset = design_text_field(design, "dataset"); !dataset.empty()) {
        return dataset;
    }
    for (const char* key : {"training_sources", "datasets", "sources"}) {
        const auto found = design.find(key);
        const json list = found != design.end() ? *found : json();
        if (list.is_array() && !list.empty() && list.front().is_string()) {
            return list.front().get<std::string>();
        }
    }
    return {};
}

// A design in words for Zetta's compiler: what it predicts, from which data, with which model.
std::string design_text(const std::string& brief, const json& design) {
    std::string text = brief;
    const std::string label = design_text_field(design, "label");
    if (!label.empty()) {
        text += " Predict " + label;
        if (const std::string dataset = design_dataset(design); !dataset.empty()) {
            text += " from " + dataset;
        }
        std::string features;
        const auto features_found = design.find("features");
        for (const json& feature : features_found != design.end() && features_found->is_array()
                                       ? *features_found
                                       : json::array()) {
            if (feature.is_string()) {
                features += (features.empty() ? "" : ", ") + feature.get<std::string>();
            }
        }
        if (!features.empty()) {
            text += " using " + features;
        }
        text += ".";
    }
    if (const std::string model = design_text_field(design, "model_kind"); !model.empty()) {
        text += " Train a " + model + " model";
        if (design.contains("holdout_fraction") && design["holdout_fraction"].is_number()) {
            text += " with a " +
                    std::to_string(static_cast<int>(design["holdout_fraction"].get<double>() * 100)) +
                    "% hold-out";
        }
        text += ".";
    }
    return text;
}

} // namespace

liquid::runtime::task<json> workspace_methodologies(ApiContext& context,
                                                    std::string customer_id) {
    const ApiResult listed = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", customer_id}, {"capability", "list_methodologies"}, {"input", json::object()}}));
    if (!listed.ok || !listed.value.contains("methodologies") ||
        !listed.value["methodologies"].is_array()) {
        co_return json(nullptr);
    }
    co_return listed.value["methodologies"];
}

json find_methodology(const json& methodologies,
                      std::string_view methodology,
                      std::string_view solution) {
    // An id written anywhere in the text ("M-3", "M-3 Segmentation"), else the catalogue
    // solution ("S-5") the registry lists the methodology as serving.
    const auto token = [](std::string_view text, char prefix) {
        for (std::size_t at = 0; at + 2 < text.size(); ++at) {
            if (text[at] != prefix || text[at + 1] != '-') continue;
            std::size_t end = at + 2;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end > at + 2) return std::string(text.substr(at, end - at));
        }
        return std::string();
    };
    if (!methodologies.is_array()) return nullptr;
    if (const std::string id = token(methodology, 'M'); !id.empty()) {
        for (const json& entry : methodologies) {
            if (entry.value("id", std::string()) == id) return entry;
        }
    }
    if (const std::string id = token(solution, 'S'); !id.empty()) {
        for (const json& entry : methodologies) {
            const json solutions = entry.value("solutions", json::array());
            if (solutions.is_array() &&
                std::ranges::find(solutions, json(id)) != solutions.end()) {
                return entry;
            }
        }
    }
    return nullptr;
}

std::string methodology_waits(const json& entry) {
    std::string text = entry.value("name", std::string("This methodology")) + " (" +
                       entry.value("id", std::string()) + ") does not run in this workspace's "
                       "engine yet";
    if (const std::string issue = entry.value("issue", std::string()); !issue.empty()) {
        text += " (" + issue + ")";
    }
    return text + ".";
}

liquid::runtime::task<ApiResult> point_pipeline(ApiContext& context,
                                                std::string customer_id,
                                                std::string project,
                                                std::string graph,
                                                std::string dataset,
                                                json model) {
    const ApiResult read = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", customer_id},
         {"capability", "get_graph"},
         {"input", {{"project", project}, {"graph", graph}}}}));
    if (!read.ok) {
        co_return read;
    }
    const std::size_t dot = dataset.find('.');
    const std::string source = dot == std::string::npos ? std::string("datasets") : dataset.substr(0, dot);
    const std::string table = dot == std::string::npos ? dataset : dataset.substr(dot + 1);
    json edits = json::array();
    bool pointed = false, modelled = false;
    for (const json& node : read.value.value("nodes", json::array())) {
        const std::string id = node.value("id", std::string());
        const std::string kind = node.value("kind", std::string());
        if (kind == "connect" && !dataset.empty()) {
            if (!pointed) {
                edits.push_back({{"op", "set_attribute"}, {"step", id}, {"name", "source"},
                                 {"value", source}});
                edits.push_back({{"op", "set_attribute"}, {"step", id}, {"name", "table"},
                                 {"value", table}});
                edits.push_back({{"op", "rename_step"}, {"step", id}, {"name", "Load " + dataset}});
                pointed = true;
            } else {
                edits.push_back({{"op", "remove_step"}, {"step", id}});
            }
        } else if (kind == "model" && !modelled && model.is_object()) {
            for (const auto& [name, value] : model.items()) {
                if (!value.is_null()) {
                    edits.push_back({{"op", "set_attribute"}, {"step", id}, {"name", name},
                                     {"value", value}});
                }
            }
            modelled = true;
        }
    }
    if (edits.empty()) {
        co_return ApiResult::success({{"graph", graph}});
    }
    const ApiResult edited = called(co_await call_capability(
        context,
        "edit_pipeline",
        {{"customer_id", customer_id}, {"project", project}, {"graph", graph}, {"edits", edits}}));
    if (!edited.ok) {
        co_return edited;
    }
    co_return ApiResult::success({{"graph", graph}});
}

liquid::runtime::task<ApiResult> compile_pipeline(ApiContext& context,
                                                  std::string customer_id,
                                                  std::string project,
                                                  std::string intent,
                                                  std::string dataset,
                                                  json model,
                                                  std::string methodology) {
    const auto act = [&](std::string capability, json input) {
        return call_capability(context,
                               "workspace_act",
                               {{"customer_id", customer_id},
                                {"capability", std::move(capability)},
                                {"input", std::move(input)}});
    };
    ApiResult result = called(co_await act("capture_intent", {{"project", project}, {"text", intent}}));
    if (result.ok) {
        json input{{"project", project}};
        if (const std::string input_id = result.value.value("input_id", std::string());
            !input_id.empty()) {
            input["input_id"] = input_id;
        }
        // The methodology's own pipeline (zetta#1061); an engine that cannot run it refuses
        // with not_supported, naming the issue, rather than compiling a skeleton.
        if (!methodology.empty()) {
            input["methodology"] = methodology;
        }
        result = called(co_await act("compile_intent", std::move(input)));
    }
    if (!result.ok) {
        co_return result;
    }
    const std::string graph = result.value.value("name", result.value.value("graph", std::string()));
    if (graph.empty() || (dataset.empty() && !model.is_object())) {
        co_return ApiResult::success({{"graph", graph}});
    }
    co_return called(co_await point_pipeline(context, customer_id, project, graph, dataset, model));
}

liquid::runtime::task<ApiResult> settle_build(ApiContext& context, control::AgentTask task) {
    if (!task.kind.starts_with("build:") || task.phase != "Succeeded" || !task.result.is_object()) {
        co_return ApiResult::success(json::object());
    }
    const std::string project = task.kind.substr(6);
    const json answer = task.result.value("result", json());
    const json design = answer.is_object() && answer.contains("pipeline") &&
                                answer["pipeline"].is_object()
                            ? answer["pipeline"]
                            : json::object();
    const std::string dataset = design_dataset(design);
    json model = json::object();
    if (const std::string label = design_text_field(design, "label"); !label.empty()) {
        model["target"] = label;
    }
    if (const std::string algo = design_text_field(design, "model_kind"); !algo.empty()) {
        model["algo"] = algo;
    }
    // The methodology the design names, else the one of the Opportunity the build came from
    // (lake#289). An engine with the registry (zetta#1061) builds that methodology's own
    // pipeline; one it cannot run yet is refused here, never replaced by a skeleton.
    std::string methodology;
    auto listed = co_await workspace_methodologies(context, task.customer_id);
    if (const json registry = listed ? *listed : json(nullptr); registry.is_array()) {
        const json context_body = task.body.is_object() ? task.body.value("context", json::object())
                                                        : json::object();
        const json opportunity = context_body.is_object()
                                     ? context_body.value("opportunity", json::object())
                                     : json::object();
        json entry = find_methodology(registry, design_text_field(design, "methodology"),
                                      design_text_field(design, "solution"));
        if (entry.is_null() && opportunity.is_object()) {
            entry = find_methodology(registry, design_text_field(opportunity, "methodology"),
                                     design_text_field(opportunity, "solution"));
        }
        if (entry.is_object()) {
            if (!entry.value("runnable", false)) {
                co_return ApiResult::failure("not_supported", methodology_waits(entry));
            }
            methodology = entry.value("id", std::string());
        }
    }
    const ApiResult read = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", task.customer_id},
         {"capability", "get_project"},
         {"input", {{"name", project}}}}));
    if (!read.ok) {
        co_return read;
    }
    const json graphs = read.value.value("graphs", json::array());
    if (graphs.empty()) {
        co_return called(co_await compile_pipeline(context,
                                                   task.customer_id,
                                                   project,
                                                   design_text(design_text_field(task.body, "message"), design),
                                                   dataset,
                                                   model,
                                                   methodology));
    }
    // The project's own pipeline (Zetta compiles one from its intent when it is created):
    // the first one not yet approved. Applied once: a connect step that already reads the
    // design's table means it was.
    const auto open = std::ranges::find_if(graphs, [](const json& graph) {
        return design_text_field(graph, "approval") != "approved";
    });
    if (open == graphs.end() || dataset.empty()) {
        co_return ApiResult::success(json::object());
    }
    const std::string graph = design_text_field(*open, "name");
    // A pipeline of another methodology's shape (the churn skeleton a segmentation project got
    // before zetta#1061) is not repointed: the design's methodology gets its own pipeline.
    if (!methodology.empty() && design_text_field(*open, "methodology") != methodology) {
        co_return called(co_await compile_pipeline(context,
                                                   task.customer_id,
                                                   project,
                                                   design_text(design_text_field(task.body, "message"), design),
                                                   dataset,
                                                   model,
                                                   methodology));
    }
    const ApiResult current = called(co_await call_capability(
        context,
        "workspace_query",
        {{"customer_id", task.customer_id},
         {"capability", "get_graph"},
         {"input", {{"project", project}, {"graph", graph}}}}));
    const std::string table = dataset.substr(dataset.find('.') == std::string::npos ? 0 : dataset.find('.') + 1);
    for (const json& node : current.ok ? current.value.value("nodes", json::array()) : json::array()) {
        if (design_text_field(node, "kind") == "connect" && node.contains("attributes") &&
            node["attributes"].is_object() && design_text_field(node["attributes"], "table") == table) {
            co_return ApiResult::success({{"graph", graph}});
        }
    }
    co_return called(co_await point_pipeline(context, task.customer_id, project, graph, dataset, model));
}

void register_agent_flow_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_agent_from_proposal",
        .title = "Create an agent from an AgentCreator proposal",
        .description =
            "Create the agent an AgentCreator proposal describes (agent_turn with agent-creator "
            "answers `proposal`): a native agent, or a Python agent whose code goes into a new "
            "private lake repository (storage lake) or into the workspace (storage zetta). A "
            "Python agent with a trigger is scheduled, and its first check run starts. Refused when "
            "the proposal lists defects.",
        .input_schema = customer_input_schema(
            {{"proposal", {{"type", "object"}}}, {"defects", {{"type", "array"}}}},
            json::array({"proposal"})),
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
            const std::string customer_id = input.at("customer_id");
            const json proposal = input.at("proposal");
            if (!input.value("defects", json::array()).empty()) {
                co_return ApiResult::failure("invalid_input", "This proposal still has defects.");
            }
            const json agent = proposal.value("agent", json::object());
            if (agent.value("name", std::string()).empty()) {
                co_return ApiResult::failure(
                    "invalid_input", "the proposal names no agent", "/proposal/agent/name");
            }
            agent_audit(context,
                        "agent.factory.approve",
                        customer_id,
                        {{"name", agent.value("name", std::string())}});
            const control::WorkspaceConfig& config = context.workspaces->config();
            const bool python = proposal.value("type", std::string()) == "python";
            json record{{"name", agent.value("name", std::string())},
                        {"purpose", agent.value("purpose", std::string())},
                        {"description", agent.value("purpose", std::string())},
                        {"work_area", agent.value("work_area", json::object())},
                        {"manifesto", agent.value("manifesto", std::string())},
                        {"goal", agent.value("goal", std::string())},
                        {"skills", agent.value("skills", json::array())},
                        {"trigger", agent.value("trigger", std::string())}};
            if (!proposal.value("pipelines", json::array()).empty()) {
                json grants = json::array();
                for (const json& grant : proposal["pipelines"]) {
                    grants.push_back({{"project", grant.value("project", std::string())},
                                      {"graph", grant.value("graph", std::string())}});
                }
                record["pipelines"] = std::move(grants);
            }
            json made = json::object();
            if (!python) {
                record["runtime"] = "native";
                record["model"] = config.base_agent_model;
                record["model_name"] = "Orama base model";
                record["system_prompt"] = agent.value("manifesto", std::string());
            } else {
                record["runtime"] = "python";
                record["model_name"] = config.frontier_model;
                const json files = proposal_files(proposal);
                std::string entrypoint;
                for (const json& file : files) {
                    if (file.value("path", std::string()) == "agent.toml") {
                        const std::string toml = file.value("contents", std::string());
                        const auto at = toml.find("entrypoint");
                        const auto open = toml.find('"', at);
                        const auto close = toml.find('"', open + 1);
                        if (at != std::string::npos && open != std::string::npos &&
                            close != std::string::npos) {
                            entrypoint = toml.substr(open + 1, close - open - 1);
                        }
                    }
                }
                if (proposal.value("storage", std::string()) == "lake") {
                    // The code goes into a private lake repository; the agent pins its commit.
                    const std::string slug = slug_of(agent.value("name", std::string()), 40);
                    const ApiResult repository = called(co_await call_capability(
                        context,
                        "create_repository",
                        {{"customer_id", customer_id},
                         {"name", "agent-" + (slug.empty() ? std::string("agent") : slug) + "-" +
                                      control::new_uuid_v4().substr(0, 4)},
                         {"description",
                          "The code of the agent " + agent.value("name", std::string())},
                         {"visibility", "private"}}));
                    if (!repository.ok) {
                        co_return repository;
                    }
                    json operations = json::array();
                    for (const json& file : files) {
                        operations.push_back(
                            {{"type", "file"},
                             {"path", file.value("path", std::string())},
                             {"content", base64(file.value("contents", std::string()))}});
                    }
                    const std::string repository_id = repository.value.value("id", std::string());
                    const ApiResult committed = called(co_await call_capability(
                        context,
                        "commit_repository_files",
                        {{"id", repository_id},
                         {"message",
                          "The agent " + agent.value("name", std::string()) +
                              ", as approved in the Agent Factory"},
                         {"operations", std::move(operations)}}));
                    if (!committed.ok) {
                        co_return committed;
                    }
                    const std::string revision = committed.value.value("commitOid", std::string());
                    record["definition"] = {{"source", "lake"},
                                            {"repository_id", repository_id},
                                            {"revision", revision},
                                            {"entrypoint", entrypoint}};
                    made["repository_id"] = repository_id;
                    made["revision"] = revision;
                } else {
                    std::string instructions;
                    json inline_files = json::array();
                    for (const json& file : files) {
                        if (file.value("path", std::string()) == "instructions.md") {
                            instructions = file.value("contents", std::string());
                        } else {
                            inline_files.push_back(file);
                        }
                    }
                    record["definition"] = {{"source", "zetta"},
                                            {"instructions", instructions},
                                            {"files", std::move(inline_files)},
                                            {"entrypoint", entrypoint}};
                }
            }
            const ApiResult created = called(co_await call_capability(
                context,
                "workspace_act",
                {{"customer_id", customer_id}, {"capability", "create_agent"}, {"input", record}}));
            if (!created.ok) {
                co_return created;
            }
            const std::string agent_id = created.value.value("id", std::string());
            made["agent_id"] = agent_id;
            made["name"] = agent.value("name", std::string());
            made["runtime"] = python ? "python" : "native";
            if (python && !agent.value("trigger", std::string()).empty()) {
                const ApiResult scheduled = called(co_await call_capability(
                    context,
                    "schedule_agent",
                    {{"customer_id", customer_id},
                     {"agent_id", agent_id},
                     {"trigger", agent["trigger"]}}));
                made["scheduled"] = scheduled.ok;
                if (!scheduled.ok) {
                    made["schedule_error"] = scheduled.error.message;
                }
            }
            if (python) {
                // The first place a new agent runs: a short check job the person can read.
                const ApiResult checked = called(co_await call_capability(
                    context,
                    "launch_agent",
                    {{"customer_id", customer_id},
                     {"agent_id", agent_id},
                     {"mode", "job"},
                     {"input",
                      "A first check: say in two sentences what you are for and what you can "
                      "reach."}}));
                if (checked.ok) {
                    made["check_launch_id"] = checked.value.value("id", std::string());
                }
            }
            co_return ApiResult::success(std::move(made));
        }});
    registry.add(Capability{
        .name = "create_projects_from_opportunities",
        .title = "Create projects from Opportunities",
        .description =
            "Create one draft project per chosen Opportunity (agent_turn with agent-autosuggest "
            "answers them): named from its intent, with the Opportunity it came from in its "
            "description. The Project Agent then designs each project's pipeline in the "
            "background (builds: {project, task_id}); the project's page compiles the design "
            "when it is done. A project gets its Opportunity's methodology's own pipeline "
            "(zetta#1061); one whose methodology the engine cannot run yet gets no pipeline and is "
            "listed in `waiting` with the issue that tracks it. Answers the projects' names, the "
            "builds and `waiting`.",
        .input_schema = customer_input_schema(
            {{"opportunities",
              {{"type", "array"}, {"items", {{"type", "object"}}}, {"minItems", 1}}},
             {"opportunity_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
            json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string customer_id = input.at("customer_id");
            // Saved Opportunities by id (Design → Opportunities), or Opportunities as given.
            json items = input.value("opportunities", json::array());
            std::vector<std::string> ids;
            if (const json chosen = input.value("opportunity_ids", json::array());
                !chosen.empty() && context.store) {
                auto saved = context.store->list_opportunities(customer_id);
                for (const json& id : chosen) {
                    const auto row = saved ? std::ranges::find(*saved, id.get<std::string>(),
                                                               &control::Opportunity::id)
                                           : std::vector<control::Opportunity>::iterator{};
                    if (!saved || row == saved->end()) {
                        co_return ApiResult::failure("not_found", "no such Opportunity " +
                                                                      id.get<std::string>());
                    }
                    json item = row->payload.is_object() ? row->payload : json::object();
                    item["intent"] = row->intent;
                    if (!row->solution.empty()) item["solution"] = row->solution;
                    if (!row->methodology.empty()) item["methodology"] = row->methodology;
                    items.push_back(std::move(item));
                    ids.push_back(row->id);
                }
            }
            if (items.empty()) {
                co_return ApiResult::failure("invalid_input", "choose at least one Opportunity");
            }
            agent_audit(context, "discover.create", customer_id, {{"count", items.size()}});
            json names = json::array();
            json builds = json::array();
            json waiting = json::array();
            // What the workspace's engine can run (zetta#1061); nullopt on an engine that predates
            // the registry, which keeps compiling from the intent alone.
            auto listed = co_await workspace_methodologies(context, customer_id);
            const json registry = listed ? *listed : json(nullptr);
            const bool builds_pipelines =
                context.workspaces && context.agent_bus &&
                context.workspaces->config().agent_endpoints.contains("agent-project");
            std::size_t position = 0;
            for (const json& item : items) {
                const std::string intent = item.value("intent", std::string());
                if (intent.empty()) {
                    co_return ApiResult::failure("invalid_input", "every Opportunity needs an intent");
                }
                const std::string slug = slug_of(intent, 40);
                const std::string name = (slug.empty() ? std::string("opportunity") : slug) + "-" +
                                         control::new_uuid_v4().substr(0, 4);
                const std::string description =
                    intent + "\n\nSource Opportunity (Auto-Suggest): " +
                    item.value("solution", std::string()) + ", " +
                    item.value("methodology", std::string()) + ". " +
                    item.value("why_this_data_supports_it", std::string());
                // The methodology the Opportunity needs (lake#289): the project gets that
                // methodology's own pipeline, or none, with the reason, when it cannot run yet.
                const json entry =
                    find_methodology(registry, item.value("methodology", std::string()),
                                     item.value("solution", std::string()));
                const bool known = entry.is_object();
                const bool runnable = known && entry.value("runnable", false);
                json project_input{{"name", name}, {"description", description}};
                if (!known) {
                    project_input["intent"] = intent;  // the engine compiles from the words
                } else if (!runnable) {
                    project_input["description"] = description + "\n\n" + methodology_waits(entry);
                }
                const ApiResult created = called(co_await call_capability(
                    context,
                    "workspace_act",
                    {{"customer_id", customer_id},
                     {"capability", "create_project"},
                     {"input", std::move(project_input)}}));
                if (!created.ok) {
                    co_return created;
                }
                names.push_back(name);
                if (known && !runnable) {
                    waiting.push_back({{"project", name},
                                       {"methodology", entry.value("id", std::string())},
                                       {"name", entry.value("name", std::string())},
                                       {"issue", entry.value("issue", std::string())},
                                       {"message", methodology_waits(entry)}});
                } else if (runnable) {
                    const ApiResult compiled = called(co_await compile_pipeline(
                        context, customer_id, name, intent, "", nullptr,
                        entry.value("id", std::string())));
                    if (!compiled.ok) {
                        co_return compiled;
                    }
                }
                if (position < ids.size()) {
                    context.store->set_opportunity_status(ids[position], "project", name);
                }
                ++position;
                // The Project Agent designs its pipeline in the background; the project page
                // compiles that design when it is done (kind "build:<project>").
                if (builds_pipelines && !(known && !runnable)) {
                    std::string brief = "Design the pipeline for the project " + name + ": " +
                                        intent + ".";
                    for (const auto& [field, label] :
                         {std::pair{"business_action", " What it changes: "},
                          std::pair{"why_this_data_supports_it", " Why the data supports it: "},
                          std::pair{"solution", " Catalogue solution: "},
                          std::pair{"methodology", " Methodology: "}}) {
                        if (const std::string value = item.value(field, std::string());
                            !value.empty()) {
                            brief += label + value;
                        }
                    }
                    const ApiResult started = called(co_await call_capability(
                        context,
                        "agent_turn",
                        {{"customer_id", customer_id},
                         {"agent", "agent-project"},
                         {"kind", "build:" + name},
                         {"message", brief},
                         {"context", {{"opportunity", item}}}}));
                    if (started.ok) {
                        builds.push_back(
                            {{"project", name},
                             {"task_id", started.value.value("task_id", std::string())}});
                    }
                }
            }
            co_return ApiResult::success({{"projects", std::move(names)},
                                          {"builds", std::move(builds)},
                                          {"waiting", std::move(waiting)}});
        }});
    registry.add(Capability{
        .name = "get_ask_conversation",
        .title = "Read an Ask Orama conversation",
        .description = "The messages of one of your conversations with Ask Orama or the Agent "
                       "Factory, oldest first, with each answer's reads, proposals and "
                       "delegations. Only the person who had the conversation can read it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"conversation_id", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}}}},
                         {"required", json::array({"conversation_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = [](const json&,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            return "principal/" + context.acting_identity;
        },
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store || context.acting_user_id.empty()) {
                return ApiResult::failure("unauthorized", "reading a conversation needs a person");
            }
            auto messages = context.store->ask_conversation(
                input.at("conversation_id"), context.acting_user_id, input.value("limit", 200));
            if (!messages) {
                return ApiResult::failure(messages.error().code, messages.error().message);
            }
            json out = json::array();
            for (const control::AskMessage& message : *messages) {
                out.push_back({{"id", message.id},
                               {"role", message.role},
                               {"content", message.content},
                               {"payload", message.payload},
                               {"created_at", message.created_at}});
            }
            return ApiResult::success({{"messages", std::move(out)}});
        }});
}

namespace {
} // namespace

void register_agent_run_capabilities(Registry& registry) {
    const auto customer_agent_schema = [](json properties, json required) {
        return customer_input_schema(std::move(properties), std::move(required));
    };
    registry.add(Capability{
        .name = "launch_agent",
        .title = "Run an agent",
        .description = "Run a user-created Python agent on agent-runtime: `job` runs it once "
                       "(unattended), `service` keeps it up for conversations. The operator runs "
                       "the launch; list_agent_launches follows it.",
        .input_schema =
            customer_agent_schema({{"agent_id", {{"type", "string"}}},
                                   {"mode", {{"type", "string"}, {"enum", {"job", "service"}}}},
                                   {"input", {{"description", "what a job is asked to do"}}}},
                                  json::array({"agent_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent_id = input.at("agent_id");
            auto agent = answered(
                co_await zetta_call(context, customer_id, "get_agent", {{"id", agent_id}}));
            if (!agent) {
                co_return agent.error();
            }
            if (agent->value("runtime", std::string("native")) != "python") {
                co_return ApiResult::failure(
                    "invalid_input",
                    "only Python agents run on agent-runtime; a native agent runs in the "
                    "workspace (talk to it with agent_turn)");
            }
            const std::string mode = input.value("mode", std::string("job"));
            agent_audit(
                context, "agent.launch", customer_id, {{"agent_id", agent_id}, {"mode", mode}});
            auto launched = context.store->create_agent_launch(
                control::NewAgentLaunch{.customer_id = customer_id,
                                        .agent_id = agent_id,
                                        .agent_name = agent->value("name", std::string()),
                                        .mode = mode,
                                        .trigger = "manual",
                                        .input = input.value("input", json(nullptr)),
                                        .scopes = control::agent_launch_scopes,
                                        .created_by_user_id = context.acting_user_id,
                                        .created_by_taap_id = context.acting_identity});
            if (!launched) {
                co_return ApiResult::failure(launched.error().code, launched.error().message);
            }
            co_return ApiResult::success(launch_json(*launched));
        }});
    registry.add(Capability{
        .name = "list_agent_launches",
        .title = "List agent runs",
        .description = "The runs of the space's agents (or one agent's), newest first, with the "
                       "operator's phase and each finished job's result.",
        .input_schema = customer_agent_schema(
            {{"agent_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}, json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        // Like every list_ capability it authorizes itself: read on the customer.
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            if (auto refused = authorize_customer_read(input, context)) {
                return *refused;
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent_id = input.value("agent_id", std::string());
            const int limit = input.value("limit", 50);
            auto listed = context.store->list_agent_launches(customer_id, agent_id, limit);
            if (!listed) {
                return ApiResult::failure(listed.error().code, listed.error().message);
            }
            // The runs the schedules' CronJobs started sit beside the launches, newest first.
            auto scheduled = context.store->list_agent_cron_runs(customer_id, agent_id, limit);
            if (!scheduled) {
                return ApiResult::failure(scheduled.error().code, scheduled.error().message);
            }
            std::vector<control::AgentLaunch> runs = std::move(*listed);
            runs.insert(runs.end(), scheduled->begin(), scheduled->end());
            std::ranges::stable_sort(runs, [](const auto& left, const auto& right) {
                return left.created_at > right.created_at;
            });
            if (limit > 0 && static_cast<int>(runs.size()) > limit) {
                runs.resize(static_cast<std::size_t>(limit));
            }
            json launches = json::array();
            for (const control::AgentLaunch& launch : runs) {
                launches.push_back(launch_json(launch));
            }
            return ApiResult::success({{"launches", std::move(launches)}});
        }});
    registry.add(Capability{
        .name = "stop_agent_launch",
        .title = "Stop an agent run",
        .description = "Stop a run: the operator removes its Job or Deployment. A finished job "
                       "is left as it is.",
        .input_schema = customer_agent_schema({{"launch_id", {{"type", "string"}}}},
                                              json::array({"launch_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            auto launch = context.store->get_agent_launch(input.at("launch_id"));
            if (!launch || launch->customer_id != customer_id) {
                return ApiResult::failure("not_found", "no such run in this space");
            }
            agent_audit(context,
                        "agent.stop",
                        customer_id,
                        {{"launch_id", launch->id}, {"agent_id", launch->agent_id}});
            auto stopped = context.store->withdraw_agent_launch(launch->id);
            if (!stopped) {
                return ApiResult::failure(stopped.error().code, stopped.error().message);
            }
            return ApiResult::success(launch_json(*stopped));
        }});
    registry.add(Capability{
        .name = "schedule_agent",
        .title = "Schedule an agent",
        .description =
            "Add a cron schedule to a Python agent: it runs as a job on every tick, as whoever "
            "set the schedule. An agent may have several; each is its own Kubernetes CronJob, "
            "which the Orama operator runs. Give `cron` (five fields, or @hourly, @daily, "
            "@weekly, @monthly) or `trigger` in plain words (every N minutes, hours or days, "
            "hourly, daily, weekly, every day|weekday|<weekday> at HH:MM). At most once every "
            "five minutes; `time_zone` is an IANA zone, UTC by default.",
        .input_schema = customer_agent_schema(
            {{"agent_id", {{"type", "string"}}},
             {"cron", {{"type", "string"}}},
             {"trigger", {{"type", "string"}}},
             {"time_zone", {{"type", "string"}}},
             {"input", {{"description", "what each run is asked to do"}}}},
            json::array({"agent_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent_id = input.at("agent_id");
            const std::string trigger = input.value("trigger", std::string());
            std::string cron = input.value("cron", std::string());
            if (cron.empty() == trigger.empty()) {
                co_return ApiResult::failure(
                    "invalid_input", "give either cron or trigger", cron.empty() ? "" : "/cron");
            }
            if (cron.empty()) {
                const auto translated = control::trigger_cron(trigger);
                if (!translated) {
                    co_return ApiResult::failure(
                        "invalid_input",
                        "lake cannot place '" + trigger +
                            "' in time; use every N minutes, hours or days, daily, weekly, every "
                            "<day> at HH:MM, or a cron expression",
                        "/trigger");
                }
                cron = *translated;
            }
            const auto normalized = control::normalize_cron(cron);
            if (!normalized) {
                co_return ApiResult::failure("invalid_input", normalized.error(), "/cron");
            }
            const std::string time_zone = input.value("time_zone", std::string("Etc/UTC"));
            if (time_zone.empty() || time_zone.size() > 64 ||
                time_zone.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/_+-") !=
                    std::string::npos) {
                co_return ApiResult::failure(
                    "invalid_input", "time_zone is an IANA zone such as Etc/UTC", "/time_zone");
            }
            auto agent = answered(
                co_await zetta_call(context, customer_id, "get_agent", {{"id", agent_id}}));
            if (!agent) {
                co_return agent.error();
            }
            if (agent->value("runtime", std::string("native")) != "python") {
                co_return ApiResult::failure("invalid_input",
                                             "only Python agents run unattended on a schedule");
            }
            json run_input = input.value("input", json(nullptr));
            if (run_input.is_null()) {
                run_input = {{"trigger", trigger.empty() ? *normalized : trigger}};
            }
            agent_audit(context,
                        "agent.schedule",
                        customer_id,
                        {{"agent_id", agent_id}, {"cron", *normalized}, {"time_zone", time_zone}});
            auto stored = context.store->create_agent_cron_schedule(
                control::NewAgentCronSchedule{.customer_id = customer_id,
                                              .agent_id = agent_id,
                                              .agent_name = agent->value("name", std::string()),
                                              .cron = *normalized,
                                              .time_zone = time_zone,
                                              .description = trigger,
                                              .input = std::move(run_input),
                                              .scopes = control::agent_launch_scopes,
                                              .created_by_user_id = context.acting_user_id,
                                              .created_by_taap_id = context.acting_identity});
            if (!stored) {
                co_return ApiResult::failure(stored.error().code, stored.error().message);
            }
            co_return ApiResult::success(schedule_json(*stored));
        }});
    registry.add(Capability{
        .name = "list_agent_schedules",
        .title = "List agent schedules",
        .description = "The cron schedules of the space's agents (or one agent's), oldest first, "
                       "with the operator's phase and when each last fired.",
        .input_schema =
            customer_agent_schema({{"agent_id", {{"type", "string"}}}}, json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            if (auto refused = authorize_customer_read(input, context)) {
                return *refused;
            }
            auto listed = context.store->list_agent_cron_schedules(
                input.at("customer_id"), input.value("agent_id", std::string()));
            if (!listed) {
                return ApiResult::failure(listed.error().code, listed.error().message);
            }
            json schedules = json::array();
            for (const control::AgentCronSchedule& schedule : *listed) {
                schedules.push_back(schedule_json(schedule));
            }
            return ApiResult::success({{"schedules", std::move(schedules)}});
        }});
    registry.add(Capability{
        .name = "unschedule_agent",
        .title = "Remove an agent schedule",
        .description = "Remove one cron schedule: the operator deletes its CronJob, which stops "
                       "a run under way. Results already written stay in list_agent_launches.",
        .input_schema = customer_agent_schema({{"schedule_id", {{"type", "string"}}}},
                                              json::array({"schedule_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            auto schedule = context.store->get_agent_cron_schedule(input.at("schedule_id"));
            if (!schedule || schedule->customer_id != customer_id) {
                return ApiResult::failure("not_found", "no such schedule in this space");
            }
            agent_audit(context,
                        "agent.unschedule",
                        customer_id,
                        {{"schedule_id", schedule->id}, {"agent_id", schedule->agent_id}});
            auto removed = context.store->withdraw_agent_cron_schedule(schedule->id);
            if (!removed) {
                return ApiResult::failure(removed.error().code, removed.error().message);
            }
            return ApiResult::success(schedule_json(*removed));
        }});
    registry.add(Capability{
        .name = "get_agent_task",
        .title = "Follow an agent turn",
        .description = "One background agent turn (from agent_turn): its status (pending, "
                       "running, succeeded, failed) and, once it finished, the agent's answer.",
        .input_schema = customer_agent_schema({{"task_id", {{"type", "string"}}}},
                                              json::array({"task_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return unavailable();
            }
            auto task = context.store->get_agent_task(input.at("task_id"));
            if (!task || task->customer_id != input.at("customer_id").get<std::string>() ||
                (task->requested_by_user_id != context.acting_user_id &&
                 context.acting_identity != "admin")) {
                co_return ApiResult::failure("not_found", "no such task in this space");
            }
            settle_discover(context, *task);
            json out = task_json(*task);
            // A project's pipeline design is applied once it is done, wherever it is read.
            if (const ApiResult built = called(co_await settle_build(context, *task)); !built.ok) {
                out["build_error"] = built.error.message;
            }
            co_return ApiResult::success(std::move(out));
        }});
    registry.add(Capability{
        .name = "list_opportunities",
        .title = "List Opportunities",
        .description = "What Auto-Suggest recommended for the space's data, kept across scans, "
                       "newest scan first: each Opportunity's intent, catalogue solution, "
                       "methodology, why the data supports it, and its status (new, project, "
                       "dismissed, or not_yet when Auto-Suggest declined it). `task_id` settles a "
                       "scan that just finished.",
        .input_schema = customer_agent_schema({{"task_id", {{"type", "string"}}}}, json::array()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            if (auto refused = authorize_customer_read(input, context)) {
                return *refused;
            }
            const std::string customer_id = input.at("customer_id");
            if (const std::string id = input.value("task_id", std::string()); !id.empty()) {
                if (auto task = context.store->get_agent_task(id);
                    task && task->customer_id == customer_id) {
                    settle_discover(context, *task);
                }
            }
            auto rows = context.store->list_opportunities(customer_id);
            if (!rows) {
                return ApiResult::failure(rows.error().code, rows.error().message);
            }
            json out = json::array();
            for (const control::Opportunity& row : *rows) {
                out.push_back(opportunity_json(row));
            }
            return ApiResult::success({{"opportunities", std::move(out)}});
        }});
    registry.add(Capability{
        .name = "dismiss_opportunity",
        .title = "Dismiss an Opportunity",
        .description = "Mark one Opportunity as not wanted; it stays listed as dismissed.",
        .input_schema = customer_agent_schema({{"opportunity_id", {{"type", "string"}}}},
                                              json::array({"opportunity_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return unavailable();
            }
            agent_audit(context, "opportunity.dismiss", input.at("customer_id"),
                        {{"opportunity_id", input.at("opportunity_id")}});
            auto row = context.store->set_opportunity_status(input.at("opportunity_id"),
                                                             "dismissed", "");
            if (!row || row->customer_id != input.at("customer_id").get<std::string>()) {
                return ApiResult::failure("not_found", "no such Opportunity in this space");
            }
            return ApiResult::success(opportunity_json(*row));
        }});
    registry.add(Capability{
        .name = "agent_turn",
        .title = "Talk to an agent",
        .description =
            "Send one message to any agent: a specialized agent of the Agent Library by its key "
            "(agent-ask-orama, agent-project, agent-autosuggest, agent-creator) or an agent of "
            "this space by its id. A library agent's turn runs in the background: the answer is "
            "{task_id, status} at once, and get_agent_task follows it to its answer. A native "
            "agent answers from the workspace; a Python agent from its running service (started "
            "on first use).",
        .input_schema = customer_agent_schema({{"agent", {{"type", "string"}}},
                                               {"message", {{"type", "string"}}},
                                               {"conversation_id", {{"type", "string"}}},
                                               {"context", {{"type", "object"}}},
                                               {"page", {{"type", "object"}}},
                                               {"kind", {{"type", "string"}}}},
                                              json::array({"agent", "message"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [&registry](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.workspaces || !context.store) {
                co_return unavailable();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string agent = input.at("agent");
            const std::string message = input.at("message");
            const control::WorkspaceConfig& config = context.workspaces->config();
            agent_audit(context, "agent.turn", customer_id, {{"agent", agent}});
            json body{{"conversation_id", input.value("conversation_id", std::string())},
                      {"message", message},
                      {"customer_id", customer_id},
                      {"page", input.value("page", json::object())}};

            if (const control::LibraryAgent* library = control::library_agent(agent)) {
                if (library->runs_as == "offline") {
                    co_return ApiResult::failure(
                        "invalid_input", std::string(library->name) + " is an offline tool");
                }
                const auto endpoint = config.agent_endpoints.find(agent);
                if (endpoint == config.agent_endpoints.end() || context.agent_caller == nullptr) {
                    co_return ApiResult::failure("store_unavailable",
                                                 std::string(library->name) + " is not running on "
                                                                              "this lake");
                }
                std::vector<std::string> scopes{"workspace_query", "workspace_openapi"};
                if (agent == "agent-ask-orama") {
                    // Ask Orama reads everything the person may read and changes nothing itself:
                    // its key carries every read capability and agent_turn, never an action.
                    scopes = {"agent_turn", "workspace_openapi"};
                    for (const auto& capability : registry.list()) {
                        // Every read, including actions that only read (workspace_query).
                        if (capability->kind == CapabilityKind::Read ||
                            capability->action == "read") {
                            scopes.push_back(capability->name);
                        }
                    }
                    scopes.push_back("workspace_query");
                }
                // The turn runs in the background (lake#269): lake records it, publishes it on
                // the agent bus and answers at once. The agent claims it through lake, which
                // replaces "{{credential}}" with a credential it hands over once and never stores
                // (docs/api/agent-bus.md).
                scopes.push_back("get_agent_task");
                body["lake_token"] = "{{credential}}";
                body["lake_url"] = config.internal_url;
                // The agents that read the person's workspace get lake's read-only path to it.
                if (agent == "agent-project" || agent == "agent-autosuggest" ||
                    agent == "agent-creator" || agent == "agent-project-corpus") {
                    body["zetta_base_url"] =
                        config.internal_url + "/workspaces/" + customer_id + "/zetta";
                    body["zetta_token"] = "{{credential}}";
                }
                if (agent == "agent-project") {
                    body["request"] = message;
                    body["context"] = input.value("context", json::object());
                    // A request about an existing pipeline is a change request (lake#249): the
                    // agent is handed the graph as it is and answers with a proposal.
                    json& handed = body["context"];
                    if (!handed.is_object()) {
                        handed = json::object();
                    }
                    if (handed.value("project", std::string()) != "" &&
                        handed.value("graph", std::string()) != "" &&
                        !handed.contains("pipeline")) {
                        auto graph = answered(co_await zetta_call(
                            context,
                            customer_id,
                            "get_graph",
                            {{"project", handed["project"]}, {"graph", handed["graph"]}}));
                        if (!graph) {
                            co_return graph.error();
                        }
                        handed["pipeline"] = std::move(*graph);
                    }
                } else if (agent == "agent-eval-generator" || agent == "agent-autosuggest-corpus" ||
                           agent == "agent-project-corpus") {
                    body["request"] = message;
                } else if (agent == "agent-autosuggest") {
                    // Auto-Suggest builds the workspace profile itself, with the task's
                    // read-only credential; lake reads nothing inside the request.
                    if (const json profile =
                            input.value("context", json::object()).value("profile", json());
                        profile.is_object()) {
                        body["profile"] = profile;
                    }
                    body["request"] = message;
                }
                if (context.acting_user_id.empty()) {
                    co_return ApiResult::failure(
                        "unauthorized", "talking to " + agent + " needs a signed-in person");
                }
                if (context.agent_bus == nullptr) {
                    co_return ApiResult::failure("store_unavailable",
                                                 "the agent bus is not configured on this lake");
                }
                auto task = context.store->create_agent_task(
                    control::AgentTask{.customer_id = customer_id,
                                       .agent = agent,
                                       .kind = input.value("kind", std::string("api")),
                                       .conversation_id =
                                           input.value("conversation_id", std::string()),
                                       .body = std::move(body),
                                       .scopes = std::move(scopes),
                                       .requested_by_user_id = context.acting_user_id,
                                       .requested_by_taap_id = context.acting_identity});
                if (!task) {
                    co_return ApiResult::failure(task.error().code, task.error().message);
                }
                // A publish that fails is not the caller's failure: the task is recorded, and
                // lake's bus loop publishes an unclaimed task again.
                static_cast<void>(co_await context.agent_bus->publish_task(*task));
                co_return ApiResult::success(task_json(*task));
            }

            auto record =
                answered(co_await zetta_call(context, customer_id, "get_agent", {{"id", agent}}));
            if (!record) {
                co_return record.error();
            }
            const std::string runtime = record->value("runtime", std::string("native"));
            if (runtime == "native") {
                auto chatted = answered(co_await zetta_call(
                    context, customer_id, "chat", {{"agent", agent}, {"message", message}}));
                if (!chatted) {
                    co_return chatted.error();
                }
                json answer = std::move(*chatted);
                answer["agent"] = agent;
                co_return ApiResult::success(std::move(answer));
            }
            if (runtime != "python") {
                co_return ApiResult::failure("invalid_input", "this agent is not talked to here");
            }
            auto launches = context.store->list_agent_launches(customer_id, agent, 20);
            if (!launches) {
                co_return ApiResult::failure(launches.error().code, launches.error().message);
            }
            const auto live = std::ranges::find_if(*launches, [](const control::AgentLaunch& l) {
                return l.mode == "service" && l.desired == "present";
            });
            if (live == launches->end()) {
                auto started = context.store->create_agent_launch(
                    control::NewAgentLaunch{.customer_id = customer_id,
                                            .agent_id = agent,
                                            .agent_name = record->value("name", std::string()),
                                            .mode = "service",
                                            .trigger = "conversation",
                                            .scopes = control::agent_launch_scopes,
                                            .created_by_user_id = context.acting_user_id,
                                            .created_by_taap_id = context.acting_identity});
                if (!started) {
                    co_return ApiResult::failure(started.error().code, started.error().message);
                }
                co_return ApiResult::success(
                    {{"agent", agent},
                     {"status", "starting"},
                     {"launch_id", started->id},
                     {"reply", "The agent is starting. Send your message again in a moment."}});
            }
            if (live->phase != "Ready" || live->endpoint.empty() ||
                context.agent_caller == nullptr) {
                co_return ApiResult::success(
                    {{"agent", agent},
                     {"status", "starting"},
                     {"launch_id", live->id},
                     {"reply",
                      "The agent is still starting (" +
                          (live->phase.empty() ? std::string("pending") : live->phase) +
                          "). Send your message again in a moment."}});
            }
            auto reply = co_await context.agent_caller->turn(
                live->endpoint,
                config.agent_service_token,
                {{"conversation_id", body["conversation_id"]}, {"message", message}});
            if (!reply) {
                co_return ApiResult::failure("store_error", reply.error().message);
            }
            if (!*reply) {
                co_return ApiResult::failure(reply->error().code, reply->error().message);
            }
            json answer = std::move(**reply);
            answer["agent"] = agent;
            co_return answer.value("ok", false)
                ? ApiResult::success(std::move(answer))
                : ApiResult::failure("engine_error",
                                     answer.value("error", std::string("the agent failed")));
        }});
}

bool workspace_read_allowed(std::string_view capability) {
    return listed(workspace_reads, capability);
}

bool workspace_action_allowed(std::string_view capability) {
    return listed(workspace_actions, capability);
}

bool workspace_uses_get(std::string_view capability) {
    return workspace_read_allowed(capability) || capability == "chat";
}

// A workspace capability's description as a customer reads it (lake#257): the engine's issue
// references ("(#1008)") and permission notes ("Requires orchestration:add_dataset.") removed.
static std::string customer_description(std::string text) {
    for (std::size_t at = text.find("(#"); at != std::string::npos; at = text.find("(#", at)) {
        const std::size_t close = text.find(')', at);
        if (close == std::string::npos) {
            break;
        }
        std::size_t start = at;
        while (start > 0 && text[start - 1] == ' ') {
            --start;
        }
        text.erase(start, close + 1 - start);
        at = start;
    }
    for (std::size_t at = text.find("Requires "); at != std::string::npos;
         at = text.find("Requires ", at)) {
        const std::size_t end = text.find('.', at);
        const std::string_view clause(text.data() + at, (end == std::string::npos ? text.size() : end) - at);
        if (clause.find(':') == std::string_view::npos) {
            at += 9;
            continue;
        }
        std::size_t start = at;
        while (start > 0 && text[start - 1] == ' ') {
            --start;
        }
        text.erase(start, (end == std::string::npos ? text.size() : end + 1) - start);
        at = start;
    }
    return text;
}

json workspace_openapi(const json& document, std::string_view base) {
    json filtered = document.is_object() ? document : json::object();
    json paths = json::object();
    const json source = filtered.value("paths", json::object());
    for (const auto& [path, operations] : source.items()) {
        if (!path.starts_with("/v1/") || !operations.is_object()) {
            continue;
        }
        const std::string name = path.substr(4);
        const bool read = workspace_read_allowed(name);
        const bool action = workspace_action_allowed(name);
        json kept = json::object();
        for (const auto& [method, operation] : operations.items()) {
            if ((method == "get" && read) || (method == "post" && action)) {
                json shown = operation;
                for (const char* key : {"description", "summary"}) {
                    if (shown.is_object() && shown.contains(key) && shown[key].is_string()) {
                        shown[key] = customer_description(shown[key].get<std::string>());
                    }
                }
                kept[method] = std::move(shown);
            }
        }
        if (!kept.empty()) {
            paths[path] = std::move(kept);
        }
    }
    filtered["paths"] = std::move(paths);
    filtered["servers"] = json::array({{{"url", std::string(base)}}});
    json info = filtered.value("info", json::object());
    info["description"] =
        "A lake workspace's Zetta, reduced to the capabilities lake allows. Reads are GET "
        "/v1/<name> with the input in the query string; actions are POST /v1/<name> with a JSON "
        "body. Authenticate with a lake token or agent key; every call is authorized on the "
        "customer and audited.";
    filtered["info"] = std::move(info);
    return filtered;
}

// Agent traffic over Zetta pub/sub (docs/api/agent-bus.md, lake#269). The first three are the
// library agents' side, each agent authenticated by its own bus credential; receive_agent_events
// is for anyone who may read the task.
namespace {
// True when the caller is that library agent, by its own bus credential.
bool agent_service(const ApiContext& context, const json& input) {
    return context.credential_kind == "agent_service" && !context.agent_service.empty() &&
           input.value("agent", std::string()) == context.agent_service;
}

// The bus's agent side is authorized by the service token, not by policy; this names the
// object for the audit trail.
std::expected<std::string, ApiError> agent_bus_object(const json&, ApiContext&) {
    return std::string("agent_bus");
}

ApiResult not_agent_service() {
    return ApiResult::failure("unauthorized",
                              "only that library agent, with its own bus credential, may call this");
}

// Replaces every "{{credential}}" string in a task body.
void fill_credential(json& value, const std::string& credential) {
    if (value.is_string() && value.get<std::string>() == "{{credential}}") {
        value = credential;
    } else if (value.is_structured()) {
        for (json& item : value) {
            fill_credential(item, credential);
        }
    }
}

json bus_wait_schema() {
    return {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 10000},
            {"description", "long poll: wait up to this many milliseconds for an event"}};
}
} // namespace

void register_agent_bus_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "receive_agent_tasks",
        .title = "Receive agent tasks",
        .description =
            "A library agent's inbox (topic agent-tasks.<agent> on lake's Zetta pub/sub): the "
            "tasks published for it from `from_sequence` on, oldest first, without those "
            "already claimed. Long-polls up to `wait_ms`. Pass the returned `next_sequence` to "
            "continue. Only that agent may call it, with its own bus credential.",
        .input_schema = {{"type", "object"},
                         {"required", json::array({"agent"})},
                         {"additionalProperties", false},
                         {"properties",
                          {{"agent", {{"type", "string"}}},
                           {"from_sequence", {{"type", "integer"}, {"minimum", 1}}},
                           {"wait_ms", bus_wait_schema()}}}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "agent_bus",
        .action = "read",
        .resolve_object = agent_bus_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!agent_service(context, input)) {
                co_return not_agent_service();
            }
            if (context.agent_bus == nullptr || context.store == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the agent bus is not configured on this lake");
            }
            const std::string agent = input.at("agent");
            if (!std::ranges::contains(context.agent_bus->agents(), agent)) {
                co_return ApiResult::failure("not_found", "no library agent " + agent, "/agent");
            }
            const std::uint64_t from = input.value("from_sequence", std::uint64_t{1});
            auto batch = control::settled(co_await context.agent_bus->bus().receive(
                control::agent_tasks_topic(agent),
                from,
                100,
                std::chrono::milliseconds(input.value("wait_ms", 0))));
            if (!batch) {
                co_return ApiResult::failure("store_unavailable", batch.error().message);
            }
            json tasks = json::array();
            for (const control::BusEvent& event : batch->events) {
                auto task = context.store->get_agent_task(event.correlation_id);
                if (!task || !task->phase.empty() || task->desired != "present") {
                    continue;
                }
                tasks.push_back({{"sequence", event.sequence},
                                 {"task_id", task->id},
                                 {"customer_id", task->customer_id},
                                 {"kind", task->kind}});
            }
            co_return ApiResult::success(
                {{"tasks", std::move(tasks)}, {"next_sequence", batch->next_sequence}});
        }});
    registry.add(Capability{
        .name = "claim_agent_task",
        .title = "Claim an agent task",
        .description =
            "A library agent takes one task it received: exactly one claim succeeds (another is "
            "a conflict). Answers the task's body with the task's own credential in it, handed "
            "over this once; lake keeps only its SHA-256. Only that agent may call it, with "
            "its own bus credential.",
        .input_schema = {{"type", "object"},
                         {"required", json::array({"task_id", "agent"})},
                         {"additionalProperties", false},
                         {"properties",
                          {{"task_id", {{"type", "string"}}}, {"agent", {{"type", "string"}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "agent_bus",
        .action = "write",
        .resolve_object = agent_bus_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!agent_service(context, input)) {
                return not_agent_service();
            }
            if (context.store == nullptr) {
                return unavailable();
            }
            const std::string task_id = input.at("task_id");
            const std::string credential = std::string(control::agent_task_credential_prefix) +
                                           task_id + "." +
                                           control::identity_crypto::random_secret();
            // Where its events will start on agent-events, so following it never reads the
            // topic's whole history (lake#275).
            auto task = context.store->claim_agent_task(
                task_id,
                input.at("agent"),
                control::identity_crypto::sha256_hex(credential),
                context.agent_bus ? context.agent_bus->cursor() : 0);
            if (!task) {
                return ApiResult::failure(task.error().code, task.error().message, "/task_id");
            }
            json body = task->body;
            fill_credential(body, credential);
            context.audit_customer_id = task->customer_id;
            context.audit_object = "agent_task/" + task->id;
            return ApiResult::success({{"task_id", task->id},
                                       {"customer_id", task->customer_id},
                                       {"agent", task->agent},
                                       {"kind", task->kind},
                                       {"deadline_seconds", task->deadline_seconds},
                                       {"body", std::move(body)}});
        }});
    registry.add(Capability{
        .name = "publish_agent_event",
        .title = "Publish an agent event",
        .description =
            "A library agent reports on a task it claimed, on topic agent-events: `progress` "
            "(a message while it works), `result` (its answer, which finishes the task) or "
            "`failed`. Lake applies it to the task. Only that agent may call it, with its own "
            "bus credential, and only for a task it claimed that has not finished.",
        .input_schema = {{"type", "object"},
                         {"required", json::array({"task_id", "agent", "type"})},
                         {"additionalProperties", false},
                         {"properties",
                          {{"task_id", {{"type", "string"}}},
                           {"agent", {{"type", "string"}}},
                           {"type",
                            {{"type", "string"},
                             {"enum", json::array({"progress", "result", "failed"})}}},
                           {"message", {{"type", "string"}}},
                           {"result", {{"type", "object"}}}}}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "agent_bus",
        .action = "write",
        .resolve_object = agent_bus_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!agent_service(context, input)) {
                co_return not_agent_service();
            }
            if (context.agent_bus == nullptr || context.store == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the agent bus is not configured on this lake");
            }
            const std::string task_id = input.at("task_id");
            auto task = context.store->get_agent_task(task_id);
            if (!task || task->agent != input.at("agent").get<std::string>() ||
                task->phase != "Running") {
                co_return ApiResult::failure(
                    "conflict", "the task is not running on this agent", "/task_id");
            }
            json event = input;
            auto published = control::settled(co_await context.agent_bus->bus().publish(
                std::string(control::agent_events_topic), std::move(event), task_id));
            if (!published) {
                co_return ApiResult::failure("store_unavailable", published.error().message);
            }
            co_return ApiResult::success({{"event_id", *published}});
        }});
    registry.add(Capability{
        .name = "receive_agent_events",
        .title = "Follow an agent turn live",
        .description =
            "What the agent of one background turn has published (topic agent-events on lake's "
            "Zetta pub/sub): progress messages and its result, from `from_sequence` on, "
            "long-polling up to `wait_ms`. Always answers the task as it stands (as "
            "get_agent_task); pass `next_sequence` to continue.",
        .input_schema = customer_input_schema({{"task_id", {{"type", "string"}}},
                                               {"from_sequence", {{"type", "integer"}, {"minimum", 1}}},
                                               {"wait_ms", bus_wait_schema()}},
                                              json::array({"task_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (context.store == nullptr) {
                co_return unavailable();
            }
            const std::string task_id = input.at("task_id");
            const auto owned = [&](const control::Result<control::AgentTask>& task) {
                return task && task->customer_id == input.at("customer_id").get<std::string>() &&
                       (task->requested_by_user_id == context.acting_user_id ||
                        context.acting_identity == "admin");
            };
            if (!owned(context.store->get_agent_task(task_id))) {
                co_return ApiResult::failure("not_found", "no such task in this space");
            }
            if (context.agent_bus == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the agent bus is not configured on this lake");
            }
            // Never before the task's own first event (lake#275): the topic only grows.
            std::uint64_t next = std::max(input.value("from_sequence", std::uint64_t{1}),
                                          context.store->get_agent_task(task_id)->events_from);
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(input.value("wait_ms", 0));
            json events = json::array();
            // Other tasks' events share the topic: read past them until one of this task's
            // arrives or the wait is over.
            for (int page = 0; page < 50; ++page) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    until - std::chrono::steady_clock::now());
                auto batch = control::settled(co_await context.agent_bus->bus().receive(
                    std::string(control::agent_events_topic),
                    next,
                    1000,
                    std::max(left, std::chrono::milliseconds(0))));
                if (!batch) {
                    co_return ApiResult::failure("store_unavailable", batch.error().message);
                }
                for (const control::BusEvent& event : batch->events) {
                    if (event.correlation_id != task_id || !event.payload.is_object()) {
                        continue;
                    }
                    json row{{"sequence", event.sequence},
                             {"type", event.payload.value("type", std::string())},
                             {"message", event.payload.value("message", std::string())}};
                    if (event.payload.contains("result")) {
                        row["result"] = event.payload["result"];
                    }
                    events.push_back(std::move(row));
                }
                const bool moved = batch->next_sequence > next;
                next = std::max(next, batch->next_sequence);
                if (!events.empty() || (!moved && std::chrono::steady_clock::now() >= until)) {
                    break;
                }
            }
            auto task = context.store->get_agent_task(task_id);
            if (!task) {
                co_return ApiResult::failure(task.error().code, task.error().message);
            }
            settle_discover(context, *task);
            static_cast<void>(co_await settle_build(context, *task));
            co_return ApiResult::success({{"events", std::move(events)},
                                          {"next_sequence", next},
                                          {"task", task_json(*task)}});
        }});
}

void register_workspace_capabilities(Registry& registry) {
    register_agent_run_capabilities(registry);
    register_agent_flow_capabilities(registry);
    register_agent_bus_capabilities(registry);
    register_sql_capability(registry);
    register_pipeline_capabilities(registry);
    register_parity_capabilities(registry);
    register_connect_capabilities(registry);
    register_search_capabilities(registry);
    register_predict_capability(registry);
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
    registry.add(Capability{
        .name = "workspace_openapi",
        .title = "A workspace's OpenAPI",
        .description =
            "The customer's Zetta OpenAPI document reduced to what lake allows, served under "
            "/workspaces/<customer_id>/zetta/v1/. Agents derive their Zetta tools from it.",
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
            const std::string customer_id = input.at("customer_id");
            auto called = co_await context.workspaces->call(
                customer_id, "openapi.json", true, json::object());
            if (!called) {
                co_return ApiResult::failure("store_unavailable", called.error().message);
            }
            if (!*called) {
                co_return ApiResult::failure(called->error().code, called->error().message);
            }
            co_return ApiResult::success(
                workspace_openapi(**called, "/workspaces/" + customer_id + "/zetta"));
        }});
    registry.add(Capability{
        .name = "list_agent_library",
        .title = "The Agent Library",
        .description =
            "Every specialized agent Orama ships (Ask Orama, the Project Agent, Auto-Suggest, "
            "AgentCreator and the offline evaluation and corpus agents): what each does, where "
            "you meet it, and whether this lake runs it.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = [](const json&,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            return "principal/" + context.acting_identity;
        },
        // The library is the same for everyone signed in.
        .authorizes_candidates = true,
        .handler =
            [](const json&, ApiContext& context) {
                const std::map<std::string, std::string> none;
                return ApiResult::success(control::agent_library_json(
                    context.workspaces ? context.workspaces->config().agent_endpoints : none));
            }});
    register_workspace_call(registry, true);
    register_workspace_call(registry, false);
    register_service_capabilities(registry);
    register_store_capabilities(registry);
}

} // namespace lake::api
