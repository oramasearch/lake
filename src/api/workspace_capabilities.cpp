#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <string_view>

#include <optional>

#include "lake/api/pipeline.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

// The Zetta capabilities lake's pages may call on a customer's workspace (docs/api/workspaces.md).
// Reads need `read` on the customer, actions `write`. Anything else is refused, so a lake caller
// can never reach Zetta's administrative surface (sql, users, cloud provisioning) through lake.
constexpr std::array<std::string_view, 22> workspace_reads = {"server_info",
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
                                                              "list_dataset_generators"};
constexpr std::array<std::string_view, 25> workspace_actions = {"create_project",
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
                                                                "generate_dataset"};

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
            "(step), add_edge (from, to, kind data_flow|control), remove_edge (from, to) or rename "
            "(name). The result must keep unique step ids, known kinds, edges between existing "
            "steps "
            "and no cycle. A saved pipeline is proposed again and must be re-approved to run.",
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
                                                               "rename"})}}},
                                               {"step", {{"type", "string"}}},
                                               {"kind", {{"type", "string"}}},
                                               {"name", {{"type", "string"}}},
                                               {"from", {{"type", "string"}}},
                                               {"to", {{"type", "string"}}}},
                                              json::array({"project", "graph", "op"})),
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
            pipeline::Edit edit{.op = input.at("op"),
                                .step = input.value("step", std::string()),
                                .kind = input.value("kind", std::string()),
                                .name = input.value("name", std::string()),
                                .from = input.value("from", std::string()),
                                .to = input.value("to", std::string())};
            auto edited = pipeline::apply(*current, edit);
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
        .description = "Run one read-only SQL query (SELECT or WITH) over the customer's workspace "
                       "data and answer {columns, rows}. Anything else is refused; the workspace's "
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

void register_workspace_capabilities(Registry& registry) {
    register_sql_capability(registry);
    register_pipeline_capabilities(registry);
    register_parity_capabilities(registry);
    register_connect_capabilities(registry);
    register_search_capabilities(registry);
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
    register_store_capabilities(registry);
}

} // namespace lake::api
