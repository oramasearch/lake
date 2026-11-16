#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agent_task_records_api.hpp"
#include "lake/api/resource_repairs.hpp"
#include "lake/api/images.hpp"
#include "lake/api/sealed_delivery.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "scan/detectors.hpp"
#include "store_mounts.hpp"
#include "workspace_bus.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

json input_schema(json properties, json required = json::array()) {
    properties["customer_id"] = {{"type", "string"}};
    properties["workspace_id"] = {{"type", "string"}};
    required.push_back("workspace_id");
    if (std::ranges::find(required, "customer_id") == required.end()) {
        required.insert(required.begin(), "customer_id");
    }
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json app_input_schema(json properties = json::object(), json required = json::array()) {
    properties["app_id"] = {{"type", "string"}};
    required.push_back("app_id");
    return input_schema(std::move(properties), std::move(required));
}

ApiResult store_failure(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_object = "customer/" + customer_id + "/apps";
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

std::expected<control::WebApp, ApiResult> owned_app(const json& input, ApiContext& context) {
    auto app = context.store->get_web_app(input.at("app_id"));
    if (!app || app->customer_id != input.at("customer_id").get<std::string>() ||
        app->workspace_id != input.at("workspace_id").get<std::string>()) {
        return std::unexpected(ApiResult::failure("not_found", "no such app in this account"));
    }
    return *app;
}

void app_audit(ApiContext& context,
               std::string kind,
               const control::WebApp& app,
               json details = json::object()) {
    context.audit_kind = std::move(kind);
    context.audit_object = "app/" + app.id;
    context.audit_details = {{"customer_id", app.customer_id},
                             {"app_id", app.id},
                             {"app_name", app.name},
                             {"generation", app.generation}};
    context.audit_details.update(std::move(details));
}

// An app's answer with the storage it mounts (lake#666): `stores`, read from store_mounts.
void add_app_stores(ApiContext& context, json& value) {
    if (!value.is_object() || !value.contains("id") || !value.at("id").is_string()) {
        return;
    }
    const auto mounts = context.store->list_store_mounts(value.value("customer_id", ""),
                                                         value.value("workspace_id", ""));
    value["stores"] = mounts
                          ? workload_stores_json(*mounts, "app", value.at("id").get<std::string>())
                          : json::array();
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 31 || name.front() < 'a' || name.front() > 'z' ||
        name.back() == '-') {
        return false;
    }
    return std::ranges::all_of(name, [](const unsigned char character) {
        return std::islower(character) || std::isdigit(character) || character == '-';
    });
}

// Whether the calling Application Agent turn was confirmed by a person who acknowledged that the
// proposed app is public: lake's confirmation form sets context.confirm_public (lake#452).
// The confirmation is in the turn's request, a record in its workspace (lake#374); a workspace
// that cannot answer acknowledges nothing.
liquid::runtime::task<bool> public_acknowledged_by_task(const ApiContext& context) {
    constexpr std::string_view prefix = "task:";
    if (context.credential_kind != "agent_task" || context.store == nullptr ||
        !context.credential_session_id.starts_with(prefix)) {
        co_return false;
    }
    auto task = context.store->get_agent_task(context.credential_session_id.substr(prefix.size()));
    if (!task || task->agent != "agent-application" ||
        !control::settled(co_await load_agent_task_content(context, *task))) {
        co_return false;
    }
    const auto task_context = task->body.find("context");
    if (task_context == task->body.end() || !task_context->is_object()) {
        co_return false;
    }
    const auto confirmed = task_context->find("confirm_public");
    co_return confirmed != task_context->end() && confirmed->is_boolean() &&
        confirmed->get<bool>();
}

bool reserved_name(std::string_view name, std::string_view configured) {
    static constexpr std::array<std::string_view, 27> built_in{
        "lake",   "www",      "api",    "app",    "apps",    "admin",    "mcp",
        "docs",   "status",   "mail",   "smtp",   "auth",    "login",    "sso",
        "id",     "cdn",      "static", "assets", "grafana", "prometheus",
        "argocd", "registry", "git",    "dev",    "staging", "prod",     "test",
    };
    const bool built_in_name = std::ranges::any_of(built_in, [&](std::string_view candidate) {
        return candidate == name;
    });
    if (built_in_name || name.starts_with("lake-") || name.starts_with("orama")) {
        return true;
    }
    while (!configured.empty()) {
        const std::size_t comma = configured.find(',');
        std::string_view candidate = configured.substr(0, comma);
        while (!candidate.empty() &&
               std::isspace(static_cast<unsigned char>(candidate.front()))) {
            candidate.remove_prefix(1);
        }
        while (!candidate.empty() &&
               std::isspace(static_cast<unsigned char>(candidate.back()))) {
            candidate.remove_suffix(1);
        }
        if (candidate == name) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        configured.remove_prefix(comma + 1);
    }
    return false;
}

bool valid_domain(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' ||
        host.contains("..") || host.contains('/') || host.contains(':')) {
        return false;
    }
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const std::size_t dot = host.find('.', label_start);
        const std::size_t label_end = dot == std::string_view::npos ? host.size() : dot;
        const std::string_view label = host.substr(label_start, label_end - label_start);
        const auto alphanumeric = [](const unsigned char character) {
            return std::islower(character) || std::isdigit(character);
        };
        if (label.empty() || label.size() > 63 || !alphanumeric(label.front()) ||
            !alphanumeric(label.back()) ||
            !std::ranges::all_of(label, [&](const unsigned char character) {
                return alphanumeric(character) || character == '-';
            })) {
            return false;
        }
        label_start = label_end + 1;
    }
    return true;
}

std::string wildcard_domain(std::string wildcard) {
    if (wildcard.starts_with("*.")) {
        wildcard.erase(0, 2);
    }
    while (wildcard.ends_with('.')) {
        wildcard.pop_back();
    }
    return wildcard;
}

std::expected<std::string, ApiResult>
app_host(const json& input, const std::string& name, const ApiContext& context) {
    const std::string wildcard = wildcard_domain(context.apps_wildcard);
    if (!valid_domain(wildcard)) {
        return std::unexpected(ApiResult::failure(
            "store_unavailable", "LAKE_APPS_WILDCARD is not configured with a valid domain"));
    }
    // An absent, null or empty host means "the default": <name>.<wildcard>. Agents send
    // "host": "" when the person did not ask for a specific address.
    std::string host = name + "." + wildcard;
    if (const auto found = input.find("host");
        found != input.end() && found->is_string() && !found->get<std::string>().empty()) {
        host = found->get<std::string>();
    }
    if (!valid_domain(host) || host == wildcard || !host.ends_with("." + wildcard) ||
        host.substr(0, host.size() - wildcard.size() - 1).contains('.')) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "host must be below the Lake apps wildcard " + wildcard, "/host"));
    }
    // An explicit host is held to the same reserved names as an app's name (lake#947): its label
    // is the name it takes beneath the wildcard. And it may never be one of the Lake's own hosts:
    // its public host, the apex host of its region picker or a region's host, which sit beneath
    // the wildcard when the wildcard is the zone itself (oramadev.net on the managing cluster).
    const std::string label = host.substr(0, host.size() - wildcard.size() - 1);
    if (reserved_name(label, context.apps_reserved_names)) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", host + " is reserved: " + label + " is a reserved name", "/host"));
    }
    bool lake_host = host == context.public_host;
    if (context.regions != nullptr) {
        lake_host = lake_host || host == context.regions->apex_host ||
                    std::ranges::any_of(context.regions->list,
                                        [&](const Region& region) { return region.host == host; });
    }
    if (lake_host) {
        return std::unexpected(
            ApiResult::failure("invalid_input", host + " is a host of the Lake itself", "/host"));
    }
    return host;
}

bool valid_env_key(std::string_view key) {
    if (key.empty() ||
        !(std::isalpha(static_cast<unsigned char>(key.front())) || key.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(key.substr(1), [](const unsigned char character) {
        return std::isalnum(character) || character == '_';
    });
}

bool flagged_secret(const std::string& key, const std::string& value) {
    const std::string assignment = key + "=" + value;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(assignment.data()),
                                           assignment.size());
    const liquid::scan_detail::report report =
        liquid::scan_detail::detector_set{}.scan("app.env", bytes, {});
    return !report.blocked.empty();
}

std::optional<ApiResult> validate_plain_env(const json& env) {
    if (!env.is_object()) {
        return ApiResult::failure("invalid_input", "env must be an object", "/env");
    }
    for (const auto& [key, value] : env.items()) {
        if (!valid_env_key(key) || !value.is_string()) {
            return ApiResult::failure(
                "invalid_input", "env keys must be names and values must be strings", "/env");
        }
        if (flagged_secret(key, value.get<std::string>())) {
            return ApiResult::failure(
                "secret_required",
                "the value looks like a secret; put it in a secret mount and pass secret",
                "/env/" + key);
        }
    }
    return std::nullopt;
}

// A secret environment value names one field of one secret in a lake secret mount (lake#625): the
// object {mount_id, name, field} or the string `lake-secret://<mount id>/<name>#<field>`, stored as
// the string. A Kubernetes Secret name is no longer accepted: a Kubernetes Secret is only the
// projection the operator writes from the secret when the app is deployed.
std::expected<std::string, ApiResult> lake_secret_reference(const json& value,
                                                            const std::string& path) {
    const std::string usage = "a secret is {mount_id, name, field} of a Lake secret mount, or "
                              "lake-secret://<mount id>/<name>#<field>";
    if (value.is_object()) {
        const auto text = [&](const char* key) {
            const auto found = value.find(key);
            return found != value.end() && found->is_string() ? found->get<std::string>()
                                                              : std::string();
        };
        const LakeSecretRef reference{text("mount_id"), text("name"), text("field")};
        if (value.size() != 3 || reference.mount_id.empty() || reference.name.empty() ||
            !kubernetes_secret_key(reference.field)) {
            return std::unexpected(ApiResult::failure("invalid_input", usage, path));
        }
        return lake_secret_ref(reference);
    }
    if (value.is_string() && parse_lake_secret_ref(value.get<std::string>())) {
        return value.get<std::string>();
    }
    return std::unexpected(ApiResult::failure("invalid_input", usage, path));
}

std::expected<std::pair<json, json>, ApiResult> normalized_app_env(const json& env) {
    if (!env.is_object()) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "env must be an object", "/env"));
    }
    json plain = json::object();
    json secret_refs = json::object();
    for (const auto& [key, value] : env.items()) {
        if (!valid_env_key(key)) {
            return std::unexpected(
                ApiResult::failure("invalid_input", "env keys must be names", "/env"));
        }
        if (value.is_object() && value.size() == 1 && value.contains("secret")) {
            auto reference = lake_secret_reference(value.at("secret"), "/env/" + key + "/secret");
            if (!reference) {
                return std::unexpected(reference.error());
            }
            secret_refs[key] = *reference;
            continue;
        }
        if (!value.is_string()) {
            return std::unexpected(ApiResult::failure(
                "invalid_input",
                "env values must be strings or {secret: {mount_id, name, field}}",
                "/env/" + key));
        }
        if (flagged_secret(key, value.get<std::string>())) {
            return std::unexpected(ApiResult::failure(
                "secret_required",
                "the value looks like a secret; put it in a secret mount and use "
                "{secret: {mount_id, name, field}}",
                "/env/" + key));
        }
        plain[key] = value;
    }
    return std::pair{std::move(plain), std::move(secret_refs)};
}

// The secret references of the calling Application Agent turn's confirmed proposal (lake#891):
// `{KEY: "lake-secret://…"}` from `context.confirmed_proposal.app.env` of the turn's request, a
// record lake wrote in its workspace (lake#374) after agent_turn verified the proposal against the
// one the person reviewed (`proposal_sha256`) and checked these references with the person's own
// credential (check_proposed_app_secret_references). Empty for any other caller, and when the
// turn's request cannot be read.
liquid::runtime::task<json> confirmed_secret_references(const ApiContext& context) {
    constexpr std::string_view prefix = "task:";
    if (context.credential_kind != "agent_task" || context.store == nullptr ||
        !context.credential_session_id.starts_with(prefix)) {
        co_return json::object();
    }
    auto task = context.store->get_agent_task(context.credential_session_id.substr(prefix.size()));
    if (!task || task->agent != "agent-application" ||
        !control::settled(co_await load_agent_task_content(context, *task))) {
        co_return json::object();
    }
    const json proposal = task->body.value("context", json::object()).is_object()
                              ? task->body.at("context").value("confirmed_proposal", json())
                              : json();
    if (!proposal.is_object() || !proposal.value("app", json()).is_object()) {
        co_return json::object();
    }
    const auto env = normalized_app_env(proposal.at("app").value("env", json::object()));
    co_return env ? env->second : json::object();
}

// Each secret reference must name a mount of the app's workspace that the caller may read, and a
// live secret with that field. Checked with the caller's own credential (get_secret_mount and
// read_secret, each audited in the mount's workspace); the value read is never kept or answered.
//
// A confirmed Application Agent turn holds no secret read (lake#891). A reference its confirmed
// proposal carries under the same key (`confirmed`, from confirmed_secret_references) was checked
// with the person's credential when they confirmed it, so here lake checks only, server-side, that
// the mount still exists and belongs to the app's workspace. Any other reference the turn sends is
// checked with its own credential, and refused.
liquid::runtime::task<std::optional<ApiResult>>
check_secret_references(const json& secret_refs,
                        const std::string& workspace_id,
                        ApiContext& context,
                        const json& confirmed = json::object()) {
    if (secret_refs.empty()) {
        co_return std::nullopt;
    }
    if (context.registry == nullptr) {
        co_return ApiResult::failure("store_unavailable", "no registry to check secret references");
    }
    for (const auto& [key, stored] : secret_refs.items()) {
        const auto reference = parse_lake_secret_ref(stored.get<std::string>());
        const std::string path = "/env/" + key + "/secret";
        if (!reference) {
            co_return ApiResult::failure("invalid_input", "not a Lake secret reference", path);
        }
        if (confirmed.is_object() && confirmed.contains(key) && confirmed.at(key) == stored) {
            if (context.store == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "no store to check secret references");
            }
            const auto route = context.store->get_secret_mount_route(reference->mount_id);
            if (!route) {
                co_return ApiResult::failure(
                    "not_found", "no secret mount " + reference->mount_id, path);
            }
            if (route->workspace_id != workspace_id) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "the secret mount belongs to another workspace than the app",
                    path);
            }
            continue;
        }
        ApiContext inner = context;
        inner.audit_details = json::object();
        auto mount = co_await context.registry->call_async(
            "get_secret_mount", {{"mount_id", reference->mount_id}}, inner);
        if (!mount || !mount->ok) {
            co_return ApiResult::failure(
                mount && mount->error.code == "unauthorized" ? "unauthorized" : "not_found",
                "no secret mount " + reference->mount_id + " this caller can read",
                path);
        }
        if (mount->value.value("workspace_id", "") != workspace_id) {
            co_return ApiResult::failure(
                "invalid_input",
                "the secret mount belongs to another workspace than the app",
                path);
        }
        ApiContext reader = context;
        reader.audit_details = json::object();
        auto read = co_await context.registry->call_async(
            "read_secret", {{"mount_id", reference->mount_id}, {"name", reference->name}}, reader);
        if (!read || !read->ok) {
            co_return ApiResult::failure(
                "not_found", "the mount has no live secret " + reference->name, path);
        }
        const json value = read->value.value("value", json::object());
        if (!value.is_object() || !value.contains(reference->field)) {
            co_return ApiResult::failure("not_found",
                                         "the secret " + reference->name + " has no field " +
                                             reference->field,
                                         path);
        }
    }
    co_return std::nullopt;
}

// A binding names a live service by id or, for agents and people, by its unique name in the
// customer. The stored binding is always the id.
// Only a service of the app's own workspace can be bound (lake#945): its connection is that
// workspace's, and an app of another workspace must never receive its credential.
std::optional<control::ManagedService> bound_service(const std::string& reference,
                                                     const std::string& customer_id,
                                                     const std::string& workspace_id,
                                                     ApiContext& context) {
    if (auto service = context.store->get_service(reference);
        service && service->customer_id == customer_id && service->desired == "present") {
        if (service->workspace_id != workspace_id) {
            return std::nullopt;
        }
        return *service;
    }
    auto listed = context.store->list_services(customer_id);
    if (!listed) {
        return std::nullopt;
    }
    std::optional<control::ManagedService> found;
    for (const control::ManagedService& service : *listed) {
        if (service.desired != "present" || service.name != reference ||
            service.workspace_id != workspace_id) {
            continue;
        }
        if (found) {
            return std::nullopt;  // ambiguous: the caller must use the id
        }
        found = service;
    }
    return found;
}

// Binding a service hands the app that service's connection credential, which
// service_connection gives only to an administrator of the account (lake#945): a binding needs the
// same right. Empty: allowed.
std::optional<ApiResult> service_binding_refused(const std::string& customer_id, ApiContext& context) {
    if (context.operator_credential) {
        return std::nullopt;
    }
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message);
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, object->id, "admin", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure(
            "unauthorized",
            "binding a service gives the app its credential: only an administrator of the account may",
            "/bindings/services");
    }
    return std::nullopt;
}

std::expected<json, ApiResult> normalized_bindings(const json& value,
                                                   const std::string& customer_id,
                                                   const std::string& workspace_id,
                                                   ApiContext& context) {
    if (!value.is_object()) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "bindings must be an object", "/bindings"));
    }
    json output{{"zetta", nullptr}, {"services", json::array()}};
    if (value.contains("zetta") && !value.at("zetta").is_null() && value.at("zetta") != false) {
        const json& binding = value.at("zetta");
        const json scopes = binding == true       ? json::array({"read"})
                            : binding.is_array()  ? binding
                            : binding.is_object() ? binding.value("scopes", json())
                                                  : json();
        if (scopes != json::array({"read"})) {
            return std::unexpected(ApiResult::failure(
                "invalid_input", "zetta binding scopes must be [read]", "/bindings/zetta"));
        }
        output["zetta"] = {{"scopes", scopes}};
    }
    const json services = value.value("services", json::array());
    if (!services.is_array()) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "bindings.services must be an array", "/bindings/services"));
    }
    if (!services.empty()) {
        if (auto refused = service_binding_refused(customer_id, context)) {
            return std::unexpected(*refused);
        }
    }
    for (const json& service_id : services) {
        if (!service_id.is_string()) {
            return std::unexpected(ApiResult::failure(
                "invalid_input", "service ids must be strings", "/bindings/services"));
        }
        auto service =
            bound_service(service_id.get<std::string>(), customer_id, workspace_id, context);
        if (!service) {
            return std::unexpected(ApiResult::failure(
                "not_found",
                "no such service in this app's workspace (use a live service's id or unique name)",
                "/bindings/services"));
        }
        if (std::ranges::find(output["services"], json(service->id)) == output["services"].end()) {
            output["services"].push_back(service->id);
        }
    }
    return output;
}

std::string app_events_topic(std::string_view app_id) {
    return "app-events." + std::string(app_id);
}

liquid::runtime::task<ApiResult> publish_event(ApiContext& context,
                                               const control::WebApp& app,
                                               std::string type,
                                               json details = json::object()) {
    if (context.workspaces == nullptr) {
        co_return ApiResult::failure("store_unavailable",
                                     "the app event bus is not configured on this Lake");
    }
    const std::string topic = app_events_topic(app.id);
    WorkspaceApiBus bus(context, app.customer_id, app.workspace_id);
    if (app.desired == "deleted" || app.phase == "Deleted") {
        const auto dropped = control::settled(co_await bus.call("drop_topic", {{"topic", topic}}));
        if (!dropped && dropped.error().code != "not_found") {
            co_return ApiResult::failure("store_unavailable", dropped.error().message);
        }
        co_return ApiResult::success(control::web_app_json(app));
    }
    auto created = control::settled(co_await bus.create_topic(topic));
    if (!created) {
        co_return ApiResult::failure("store_unavailable", created.error().message);
    }
    json event{{"app_id", app.id},
               {"customer_id", app.customer_id},
               {"type", std::move(type)},
               {"generation", app.generation},
               {"principal", context.acting_identity},
               {"details", std::move(details)}};
    auto published = control::settled(
        co_await bus.publish(topic, std::move(event), app.id));
    if (!published) {
        co_return ApiResult::failure("store_unavailable", published.error().message);
    }
    json value = control::web_app_json(app);
    value["event_id"] = *published;
    co_return ApiResult::success(std::move(value));
}

liquid::runtime::task<ApiResult> patch_and_publish(const json& input,
                                                   ApiContext& context,
                                                   control::WebAppPatch patch,
                                                   std::string audit_kind,
                                                   std::string event_type,
                                                   json details = json::object()) {
    auto current = owned_app(input, context);
    if (!current) {
        co_return current.error();
    }
    patch.max_replicas = context.app_replicas_per_customer;
    auto updated = context.store->update_web_app(current->id, patch);
    if (!updated) {
        co_return store_failure(updated.error());
    }
    app_audit(context, std::move(audit_kind), *updated, details);
    co_return co_await publish_event(context, *updated, std::move(event_type), std::move(details));
}

control::WebAppPatch deployment_patch(const json& input) {
    control::WebAppPatch patch;
    patch.desired = "running";
    if (input.contains("source")) {
        const json& source = input.at("source");
        patch.mode = "runtime";
        patch.source_repository = source.value("repository", "");
        patch.source_revision = source.value("revision", "");
        patch.source_path = source.value("path", ".");
        patch.source_entrypoint = source.value("entrypoint", "");
        patch.image = "";
    } else {
        patch.mode = "image";
        patch.source_repository = "";
        patch.source_revision = "";
        patch.source_path = "";
        patch.source_entrypoint = "";
        patch.image = input.value("image", "");
    }
    return patch;
}

std::optional<ApiResult> validate_source_repository(const json& input, ApiContext& context) {
    if (!input.contains("source")) {
        return std::nullopt;
    }
    const auto repository =
        context.store->get_repository(input.at("source").at("repository").get<std::string>());
    if (!repository || repository->customer_id != input.at("customer_id").get<std::string>()) {
        return ApiResult::failure(
            "not_found", "no such repository in this account", "/source/repository");
    }
    return std::nullopt;
}

json event_input_schema() {
    json schema = app_input_schema({{"type", {{"type", "string"}}},
                                    {"message", {{"type", "string"}}},
                                    {"status", {{"type", "object"}}},
                                    {"event", {{"type", "object"}}}});
    schema["required"] = json::array({"app_id", "workspace_id"});
    return schema;
}

std::expected<std::string, ApiError> event_customer_object(const json& input, ApiContext& context) {
    const auto app = context.store->get_web_app(input.at("app_id"));
    if (!app || (input.contains("customer_id") && input.at("customer_id") != app->customer_id)) {
        return std::unexpected(ApiError{"not_found", "no such app in this account", "/app_id"});
    }
    return customer_object({{"customer_id", app->customer_id}}, context);
}

std::optional<ApiResult> validate_deployment(const json& input) {
    const bool source = input.contains("source");
    const bool image = input.contains("image");
    if (source == image) {
        return ApiResult::failure("invalid_input",
                                  "provide exactly one of source or image",
                                  source ? "/image" : "/source");
    }
    if (source) {
        const json& value = input.at("source");
        if (!value.is_object() || value.value("repository", "").empty() ||
            value.value("revision", "").empty()) {
            return ApiResult::failure(
                "invalid_input", "source requires repository and revision", "/source");
        }
    } else if (!input.at("image").is_string() || input.at("image").get<std::string>().empty()) {
        return ApiResult::failure("invalid_input", "image cannot be empty", "/image");
    } else if (!digest_pinned_image(input.at("image").get<std::string>())) {
        // The operator runs only a digest-pinned image (lake#783): refused here, not after the
        // app exists and has failed.
        return ApiResult::failure(
            "invalid_input",
            "image must be pinned by digest, as name@sha256:<digest> (64 hex digits); a tag "
            "such as name:1.27 is not accepted",
            "/image");
    }
    return std::nullopt;
}

// An image app's image, pinned by digest (lake#783).
json image_property() {
    return {{"type", "string"},
            {"description", "an image pinned by digest: name@sha256:<64 hex digits>"}};
}

json deployment_properties() {
    return {{"source",
             {{"type", "object"},
              {"properties",
               {{"repository", {{"type", "string"}}},
                {"revision", {{"type", "string"}}},
                {"path", {{"type", "string"}}},
                {"entrypoint", {{"type", "string"}}}}},
              {"additionalProperties", false}}},
            {"image", image_property()}};
}

} // namespace

liquid::runtime::task<std::optional<ApiResult>> check_proposed_app_secret_references(
    const json& app, const std::string& workspace_id, ApiContext& context) {
    const auto env = normalized_app_env(app.value("env", json::object()));
    if (!env) {
        co_return env.error();
    }
    auto checked = co_await check_secret_references(env->second, workspace_id, context);
    if (!checked) {
        co_return ApiResult::failure("store_error", checked.error().message);
    }
    co_return std::move(*checked);
}

void register_web_app_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_apps",
        .title = "List apps",
        .description = "List the Ship apps in an account, including desired and observed state.",
        .input_schema = input_schema(json::object()),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            const auto object = customer_object(input, context);
            if (!object) {
                return ApiResult::failure(
                    object.error().code, object.error().message, object.error().path);
            }
            if (context.acting_identity != "admin") {
                const auto decision = control::authorize(*context.store,
                                                         context.acting_identity,
                                                         *object,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    return store_failure(decision.error());
                }
                if (!decision->allowed) {
                    return ApiResult::failure("unauthorized",
                                              "principal is not allowed to read this account's apps");
                }
            }
            auto apps = context.store->list_web_apps(input.at("customer_id"));
            if (!apps) {
                return store_failure(apps.error());
            }
            const auto mounts =
                context.store->list_store_mounts(input.at("customer_id"), input.at("workspace_id"));
            if (!mounts) {
                return store_failure(mounts.error());
            }
            json items = json::array();
            for (const control::WebApp& app : *apps) {
                if (app.workspace_id != input.at("workspace_id").get<std::string>()) {
                    continue;
                }
                json value = control::web_app_json(app);
                value["stores"] = workload_stores_json(*mounts, "app", app.id);
                items.push_back(std::move(value));
            }
            context.audit_kind = "app.list";
            return ApiResult::success({{"apps", std::move(items)},
                                       {"app_limit", context.apps_per_customer},
                                       {"replica_limit", context.app_replicas_per_customer}});
        }});
    registry.add(Capability{.name = "get_app",
                            .title = "Get app",
                            .description =
                                "Read one Ship app: its hosts, revision or image, replicas, bound "
                                "services, the storage it mounts (`stores`), and what the "
                                "operator reports about its rollout.",
                            .input_schema = app_input_schema(),
                            .output_schema = {{"type", "object"}},
                            .kind = CapabilityKind::Read,
                            .object = "customer",
                            .action = "read",
                            .resolve_object = customer_object,
                            .handler = [](const json& input, ApiContext& context) {
                                auto app = owned_app(input, context);
                                if (!app) {
                                    return app.error();
                                }
                                app_audit(context, "app.read", *app);
                                json value = control::web_app_json(*app);
                                add_app_stores(context, value);
                                return ApiResult::success(std::move(value));
                            }});
    registry.add(Capability{
        .name = "create_app",
        .title = "Create app",
        .description = "Create runtime or image desired state for the app operator. App names "
                       "cannot use reserved platform hosts. `stores` mounts the workspace's "
                       "storage: a volume at `mount_path`, a bucket as environment variables.",
        .input_schema = input_schema(
            {{"name", {{"type", "string"}}},
             {"mode", {{"type", "string"}, {"enum", json::array({"runtime", "image"})}}},
             {"source", deployment_properties().at("source")},
             {"image", image_property()},
             {"framework", {{"type", "string"}}},
             {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
             {"replicas", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
             {"host", {{"type", json::array({"string", "null"})}}},
             {"auth", {{"type", "string"}, {"enum", json::array({"lake", "none", "public"})}}},
             {"confirm_public", {{"type", "boolean"}}},
             {"bindings", {{"type", "object"}}},
             {"env", {{"type", "object"}}},
             {"stores", store_mounts_schema()}},
            json::array({"name", "mode", "framework", "port"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (context.store == nullptr || context.workspaces == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the app store and event bus must be configured");
            }
            const std::string name = input.at("name");
            if (!valid_name(name)) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "name must start with a letter and use lowercase letters, digits and '-', at "
                    "most 31 characters",
                    "/name");
            }
            if (reserved_name(name, context.apps_reserved_names)) {
                co_return ApiResult::failure("invalid_input", name + " is reserved", "/name");
            }
            // A public app is acknowledged as public, as set_app_auth requires (lake#452): by the
            // caller's confirm_public, or, for an Application Agent turn, by the person who
            // confirmed the proposal (its task context's confirm_public).
            const std::string requested_auth = input.value("auth", std::string("lake"));
            const bool public_auth = requested_auth == "none" || requested_auth == "public";
            const bool caller_confirmed =
                input.contains("confirm_public") && input.at("confirm_public") == true;
            bool task_confirmed = false;
            if (public_auth && !caller_confirmed) {
                const auto acknowledged = co_await public_acknowledged_by_task(context);
                task_confirmed = acknowledged && *acknowledged;
            }
            if (public_auth && !caller_confirmed && !task_confirmed) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "a public app can be opened by anyone with its address, without signing in; "
                    "confirm that with confirm_public: true",
                    "/confirm_public");
            }
            json deployment = input;
            const auto workspace = co_await context.workspaces->state(input.at("customer_id"), input.at("workspace_id"));
            if (!workspace || !*workspace || !(**workspace).ready()) {
                co_return ApiResult::failure("workspace_not_ready", "workspace_id needs a ready workspace Zetta");
            }
            const std::string mode = input.at("mode");
            if ((mode == "runtime") != input.contains("source") ||
                (mode == "image") != input.contains("image")) {
                co_return ApiResult::failure(
                    "invalid_input", "mode must match exactly one source or image", "/mode");
            }
            if (const auto invalid = validate_deployment(deployment)) {
                co_return *invalid;
            }
            if (const auto invalid = validate_source_repository(input, context)) {
                co_return *invalid;
            }
            auto env = normalized_app_env(input.value("env", json::object()));
            if (!env) {
                co_return env.error();
            }
            // A confirmed Application Agent turn's references were checked as the person when
            // they confirmed its proposal (lake#891).
            json confirmed = json::object();
            if (!env->second.empty() && context.credential_kind == "agent_task") {
                confirmed = co_await confirmed_secret_references(context);
            }
            auto checked = co_await check_secret_references(
                env->second, input.at("workspace_id").get<std::string>(), context, confirmed);
            if (!checked) {
                co_return ApiResult::failure("store_error", checked.error().message);
            }
            if (*checked) {
                co_return **checked;
            }
            json host_input = input;
            if (host_input.value("host", json()).is_null()) {
                host_input.erase("host");
            }
            auto host = app_host(host_input, name, context);
            if (!host) {
                co_return host.error();
            }
            auto bindings = normalized_bindings(input.value("bindings", json::object()),
                                                input.at("customer_id"),
                                                input.at("workspace_id"),
                                                context);
            if (!bindings) {
                co_return bindings.error();
            }
            const int replicas = input.value("replicas", 1);
            if (replicas < 1 || replicas > 100) {
                co_return ApiResult::failure(
                    "invalid_input", "replicas must be between 1 and 100", "/replicas");
            }
            auto mounts = checked_store_mounts(input.value("stores", json::array()),
                                               input.at("customer_id"),
                                               input.at("workspace_id"),
                                               {},
                                               true,
                                               *context.store);
            if (!mounts) {
                co_return mounts.error();
            }
            control::NewWebApp app;
            app.customer_id = input.at("customer_id");
            app.workspace_id = input.at("workspace_id");
            app.name = name;
            app.mode = mode;
            app.framework = input.at("framework");
            app.port = input.at("port");
            app.replicas = replicas;
            app.host = *host;
            app.auth = input.value("auth", "lake") == "public" ? "none"
                                                                  : input.value("auth", "lake");
            app.bindings = std::move(*bindings);
            app.env = std::move(env->first);
            app.secret_env_refs = std::move(env->second);
            app.max_apps = context.apps_per_customer;
            app.max_replicas = context.app_replicas_per_customer;
            const control::WebAppPatch sources = deployment_patch(deployment);
            app.source_repository = sources.source_repository.value_or("");
            app.source_revision = sources.source_revision.value_or("");
            app.source_path = sources.source_path.value_or("");
            app.source_entrypoint = sources.source_entrypoint.value_or("");
            app.image = sources.image.value_or("");
            app.stores = std::move(*mounts);
            auto created = context.store->create_web_app(app);
            if (!created) {
                co_return store_failure(created.error());
            }
            app_audit(context, "app.create", *created);
            // Who deployed it and from which source, for its notifications and repairs (lake#1207).
            static_cast<void>(co_await note_deployment(context,
                                                       "app",
                                                       created->id,
                                                       created->customer_id,
                                                       created->workspace_id,
                                                       input.value("source", json::object())));
            auto published = co_await publish_event(context, *created, "create");
            if (published && published->ok) {
                add_app_stores(context, published->value);
            }
            co_return published;
        }});

    const auto register_deployment = [&](std::string name, bool rollback) {
        json properties = deployment_properties();
        json schema;
        if (rollback) {
            schema = app_input_schema(std::move(properties));
        } else {
            properties["app_id"] = {{"type", "string"}};
            properties["app"] = {{"type", "string"}};
            schema = input_schema(std::move(properties));
        }
        registry.add(Capability{
            .name = std::move(name),
            .title = rollback ? "Rollback app" : "Deploy app",
            .description = rollback ? "Set a prior source revision or image as desired."
                                    : "Set a new source revision or image as desired.",
            .input_schema = std::move(schema),
            .output_schema = {{"type", "object"}},
            .kind = CapabilityKind::Action,
            .object = "customer",
            .action = "write",
            .resolve_object = customer_object,
            .async_handler = [rollback](const json& input,
                                        ApiContext& context) -> liquid::runtime::task<ApiResult> {
                json normalized = input;
                if (!rollback) {
                    const bool has_app_id = input.contains("app_id");
                    const bool has_app = input.contains("app");
                    if (has_app_id == has_app) {
                        co_return ApiResult::failure(
                            "invalid_input", "give exactly one of app_id or app", "/app_id");
                    }
                    if (has_app) {
                        normalized["app_id"] = input.at("app");
                        normalized.erase("app");
                    }
                }
                if (const auto invalid = validate_deployment(normalized)) {
                    co_return *invalid;
                }
                if (const auto invalid = validate_source_repository(normalized, context)) {
                    co_return *invalid;
                }
                json details;
                if (normalized.contains("source")) {
                    details["source"] = normalized.at("source");
                } else {
                    details["image"] = normalized.at("image");
                }
                auto completed = co_await patch_and_publish(normalized,
                                                                context,
                                                                deployment_patch(normalized),
                                                                rollback ? "app.rollback" : "app.deploy",
                                                                rollback ? "rollback" : "deploy",
                                                                std::move(details));
                if (!completed) {
                    co_return ApiResult::failure("store_error", completed.error().message);
                }
                ApiResult deployed = std::move(*completed);
                // Who deployed it and from which source, for its notifications and repairs
                // (lake#1207).
                if (deployed.ok) {
                    if (auto app = context.store->get_web_app(normalized.at("app_id")); app) {
                        static_cast<void>(co_await note_deployment(
                            context, "app", app->id, app->customer_id, app->workspace_id,
                            normalized.value("source", json::object())));
                    }
                }
                co_return deployed;
            }});
    };
    register_deployment("deploy_app", false);
    register_deployment("rollback_app", true);

    registry.add(Capability{
        .name = "delete_app",
        .title = "Delete app",
        .description = "Request app deletion. confirm must equal its name.",
        .input_schema =
            app_input_schema({{"confirm", {{"type", "string"}}}}, json::array({"confirm"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            if (input.at("confirm") != app->name) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must be the app name " + app->name, "/confirm");
            }
            control::WebAppPatch patch;
            patch.desired = "deleted";
            co_return co_await patch_and_publish(
                input, context, std::move(patch), "app.delete", "delete");
        }});

    registry.add(Capability{
        .name = "set_app_env",
        .title = "Set app environment value",
        .description =
            "Set one plain value, scanned for secrets, or one secret: a field of a secret in a "
            "secret mount of the app's workspace ({mount_id, name, field}, or secret_ref "
            "lake-secret://<mount id>/<name>#<field>), which the operator projects into the app "
            "and rewrites when the secret gets a new version.",
        .input_schema =
            app_input_schema({{"key", {{"type", "string"}}},
                              {"value", {{"type", "string"}}},
                              {"secret",
                               {{"type", "object"},
                                {"properties",
                                 {{"mount_id", {{"type", "string"}}},
                                  {"name", {{"type", "string"}}},
                                  {"field", {{"type", "string"}}}}},
                                {"required", json::array({"mount_id", "name", "field"})}}},
                              {"secret_ref", {{"type", "string"}}}},
                             json::array({"key"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            const std::string key = input.at("key");
            if (!valid_env_key(key)) {
                co_return ApiResult::failure(
                    "invalid_input", "key is not an environment name", "/key");
            }
            const int given = static_cast<int>(input.contains("value")) +
                              static_cast<int>(input.contains("secret")) +
                              static_cast<int>(input.contains("secret_ref"));
            if (given != 1) {
                co_return ApiResult::failure("invalid_input",
                                             "provide exactly one of value, secret or secret_ref");
            }
            control::WebAppPatch patch;
            json details{{"key", key}};
            if (input.contains("value")) {
                const std::string value = input.at("value");
                if (flagged_secret(key, value)) {
                    co_return ApiResult::failure(
                        "secret_required",
                        "the value looks like a secret; put it in a secret mount and pass secret",
                        "/value");
                }
                app->env[key] = value;
                app->secret_env_refs.erase(key);
                details["secret"] = false;
            } else {
                const bool object = input.contains("secret");
                auto reference =
                    lake_secret_reference(object ? input.at("secret") : input.at("secret_ref"),
                                          object ? "/secret" : "/secret_ref");
                if (!reference) {
                    co_return reference.error();
                }
                json one{{key, *reference}};
                auto checked = co_await check_secret_references(one, app->workspace_id, context);
                if (!checked) {
                    co_return ApiResult::failure("store_error", checked.error().message);
                }
                if (*checked) {
                    ApiResult refused = std::move(**checked);
                    refused.error.path = object ? "/secret" : "/secret_ref";
                    co_return refused;
                }
                app->env.erase(key);
                app->secret_env_refs[key] = *reference;
                details["secret"] = true;
                // The reference, never a value: which mount, secret and field the app reads.
                details["secret_ref"] = *reference;
            }
            patch.env = app->env;
            patch.secret_env_refs = app->secret_env_refs;
            co_return co_await patch_and_publish(
                input, context, std::move(patch), "app.env.set", "env", std::move(details));
        }});
    registry.add(Capability{
        .name = "unset_app_env",
        .title = "Unset app environment value",
        .description = "Remove one plain value or secret reference from desired state.",
        .input_schema = app_input_schema({{"key", {{"type", "string"}}}}, json::array({"key"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            const std::string key = input.at("key");
            app->env.erase(key);
            app->secret_env_refs.erase(key);
            control::WebAppPatch patch;
            patch.env = app->env;
            patch.secret_env_refs = app->secret_env_refs;
            co_return co_await patch_and_publish(
                input, context, std::move(patch), "app.env.unset", "env", {{"key", key}});
        }});

    const auto register_state = [&](std::string name, std::string desired, std::string event) {
        registry.add(Capability{
            .name = std::move(name),
            .title = event + " app",
            .description = event == "start"
                               ? std::string("Start a stopped app again at its current revision, "
                                             "so it serves its hosts.")
                           : event == "stop"
                               ? std::string("Stop an app: its pods are removed and its hosts stop "
                                             "answering, while its configuration is kept.")
                               : std::string("Restart an app's pods at the same revision, for "
                                             "example after changing a bound secret. The app's "
                                             "`restart_generation` becomes its new generation, "
                                             "which the operator writes on the pod template, so "
                                             "the pods are replaced; a stopped app starts."),
            .input_schema = app_input_schema(),
            .output_schema = {{"type", "object"}},
            .kind = CapabilityKind::Action,
            .object = "customer",
            .action = "write",
            .resolve_object = customer_object,
            .async_handler = [desired = std::move(desired), event = std::move(event)](
                                 const json& input,
                                 ApiContext& context) -> liquid::runtime::task<ApiResult> {
                control::WebAppPatch patch;
                patch.desired = desired;
                // Without a change of its own the pod template stayed the same, and the operator
                // replaced nothing (lake#1228).
                patch.restart = event == "restart";
                co_return co_await patch_and_publish(
                    input, context, std::move(patch), "app." + event, event);
            }});
    };
    register_state("restart_app", "running", "restart");
    register_state("stop_app", "stopped", "stop");
    register_state("start_app", "running", "start");

    registry.add(Capability{
        .name = "set_app_auth",
        .title = "Set app authentication",
        .description = "Require Lake sign-in or make an app public after explicit confirmation.",
        .input_schema = app_input_schema(
            {{"auth", {{"type", "string"}, {"enum", json::array({"lake", "none"})}}},
             {"confirm_public", {{"type", "boolean"}}}},
            json::array({"auth"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const std::string auth = input.at("auth");
            if (auth == "none" && !input.value("confirm_public", false)) {
                co_return ApiResult::failure("invalid_input",
                                             "making an app public requires confirmation",
                                             "/confirm_public");
            }
            control::WebAppPatch patch;
            patch.auth = auth;
            co_return co_await patch_and_publish(
                input, context, std::move(patch), "app.auth.set", "auth", {{"auth", auth}});
        }});

    registry.add(Capability{
        .name = "scale_app",
        .title = "Scale app",
        .description = "Set desired replicas within the account's aggregate quota.",
        .input_schema = app_input_schema(
            {{"replicas", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
            json::array({"replicas"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const int replicas = input.at("replicas");
            control::WebAppPatch patch;
            patch.replicas = replicas;
            co_return co_await patch_and_publish(
                input, context, std::move(patch), "app.scale", "scale", {{"replicas", replicas}});
        }});

    registry.add(Capability{
        .name = "set_app_stores",
        .title = "Set app storage",
        .description = "Choose the workspace storage the app mounts, replacing what it mounted: a "
                       "volume at an absolute `mount_path` (a volume is mounted by one service or "
                       "app), a bucket in your own account as environment variables "
                       "(STORE_<NAME>_BUCKET and the rest). The operator rolls the app's pods. [] "
                       "mounts nothing.",
        .input_schema =
            app_input_schema({{"stores", store_mounts_schema()}}, json::array({"stores"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            auto mounts = checked_store_mounts(
                input.at("stores"), app->customer_id, app->workspace_id, {}, true, *context.store);
            if (!mounts) {
                co_return mounts.error();
            }
            json ids = json::array();
            for (const control::StoreMountRequest& mount : *mounts) {
                ids.push_back(mount.store_id);
            }
            control::WebAppPatch patch;
            patch.stores = std::move(*mounts);
            auto result = co_await patch_and_publish(
                input, context, std::move(patch), "app.stores", "stores", {{"stores", ids}});
            if (result && result->ok) {
                add_app_stores(context, result->value);
            }
            co_return result;
        }});

    registry.add(Capability{
        .name = "set_app_zetta_binding",
        .title = "Set app Zetta binding",
        .description = "Attach or detach read-only access to the account's workspace Zetta.",
        .input_schema =
            app_input_schema({{"attached", {{"type", "boolean"}}}}, json::array({"attached"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            const bool attached = input.at("attached");
            app->bindings["zetta"] =
                attached ? json({{"scopes", json::array({"read"})}}) : json(nullptr);
            control::WebAppPatch patch;
            patch.bindings = app->bindings;
            patch.rotate_tokens = !attached;
            co_return co_await patch_and_publish(input,
                                                 context,
                                                 std::move(patch),
                                                 "app.binding.zetta",
                                                 "binding",
                                                 {{"zetta", attached}});
        }});
    registry.add(Capability{
        .name = "bind_service_to_app",
        .title = "Bind service to app",
        .description =
            "Deliver one existing service connection to an app through an operator Secret.",
        .input_schema =
            app_input_schema({{"service_id", {{"type", "string"}}}}, json::array({"service_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            if (auto refused = service_binding_refused(app->customer_id, context)) {
                co_return *refused;
            }
            auto service = bound_service(input.at("service_id").get<std::string>(),
                                         app->customer_id,
                                         app->workspace_id,
                                         context);
            if (!service) {
                co_return ApiResult::failure(
                    "not_found",
                    "no such service in this app's workspace (use a live service's id or unique name)");
            }
            json services = app->bindings.value("services", json::array());
            if (std::ranges::find(services, json(service->id)) == services.end()) {
                services.push_back(service->id);
            }
            app->bindings["services"] = std::move(services);
            control::WebAppPatch patch;
            patch.bindings = app->bindings;
            co_return co_await patch_and_publish(input,
                                                 context,
                                                 std::move(patch),
                                                 "app.binding.service",
                                                 "binding",
                                                 {{"service_id", service->id}});
        }});
    registry.add(Capability{
        .name = "unbind_service_from_app",
        .title = "Detach service from app",
        .description = "Remove one managed service connection from an app.",
        .input_schema =
            app_input_schema({{"service_id", {{"type", "string"}}}}, json::array({"service_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            const std::string service_id = input.at("service_id");
            json services = app->bindings.value("services", json::array());
            const auto found = std::ranges::find(services, json(service_id));
            if (found == services.end()) {
                co_return ApiResult::failure(
                    "not_found", "that service is not bound to this app", "/service_id");
            }
            services.erase(found);
            app->bindings["services"] = std::move(services);
            control::WebAppPatch patch;
            patch.bindings = app->bindings;
            co_return co_await patch_and_publish(input,
                                                 context,
                                                 std::move(patch),
                                                 "app.binding.service.remove",
                                                 "binding",
                                                 {{"service_id", service_id}, {"attached", false}});
        }});
    registry.add(Capability{
        .name = "create_service_and_bind_to_app",
        .title = "Create service and bind to app",
        .description =
            "Create a managed service, then bind its Secret-delivered connection to an app.",
        .input_schema = app_input_schema(
            {{"kind", {{"type", "string"}}},
             {"name", {{"type", "string"}}},
             {"instances", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}},
             {"storage_gi", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
            json::array({"kind", "name"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [&registry](const json& input,
                                     ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (context.workspaces == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "managed services are not configured");
            }
            const auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            json create{{"customer_id", input.at("customer_id")},
                        {"kind", input.at("kind")},
                        {"name", input.at("name")},
                        {"instances", input.value("instances", 1)},
                        {"storage_gi", input.value("storage_gi", 10)}};
            create["workspace_id"] = input.at("workspace_id");
            auto created = co_await registry.call_async("create_service", create, context);
            if (!created) {
                co_return ApiResult::failure("store_error", created.error().message);
            }
            if (!created->ok) {
                co_return *created;
            }
            json bind{{"customer_id", input.at("customer_id")},
                      {"workspace_id", input.at("workspace_id")},
                      {"app_id", input.at("app_id")},
                      {"service_id", created->value.at("id")}};
            auto bound = co_await registry.call_async("bind_service_to_app", bind, context);
            if (!bound) {
                co_return ApiResult::failure("store_error", bound.error().message);
            }
            if (bound->ok) {
                bound->value["service"] = created->value;
            }
            co_return *bound;
        }});

    registry.add(Capability{
        .name = "app_logs",
        .title = "Read app logs",
        .description = "Read an app namespace from the configured Loki-compatible source.",
        .input_schema = app_input_schema(
            {{"since", {{"type", "string"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
             {"direction", {{"type", "string"}, {"enum", json::array({"forward", "backward"})}}}}),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = owned_app(input, context);
            if (!app) {
                co_return app.error();
            }
            if (context.platform == nullptr) {
                co_return ApiResult::failure("store_unavailable", "app logs are not configured");
            }
            auto completed =
                co_await context.platform->app_logs(app->id,
                                                    input.value("since", ""),
                                                    input.value("limit", 200),
                                                    input.value("direction", "forward"));
            if (!completed) {
                co_return ApiResult::failure("store_error", completed.error().message);
            }
            auto logs = std::move(*completed);
            if (!logs) {
                co_return ApiResult::failure(logs.error().code, logs.error().message);
            }
            app_audit(context, "app.logs.read", *app);
            co_return ApiResult::success(std::move(*logs));
        }});
    registry.add(Capability{
        .name = "publish_app_event",
        .title = "Publish app event",
        .description = "Publish operator rollout or pod status on this app's dedicated topic.",
        .input_schema = event_input_schema(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = event_customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto app = context.store->get_web_app(input.at("app_id"));
            if (!app ||
                app->workspace_id != input.at("workspace_id").get<std::string>() ||
                (input.contains("customer_id") && input.at("customer_id") != app->customer_id)) {
                co_return ApiResult::failure("not_found", "no such app in this account");
            }
            if (!input.contains("event") && !input.contains("type")) {
                co_return ApiResult::failure("invalid_input", "event or type is required");
            }
            if (context.workspaces == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the app event bus is not configured");
            }
            const std::string topic = app_events_topic(app->id);
            WorkspaceApiBus bus(context, app->customer_id, app->workspace_id);
            if (app->desired == "deleted" || app->phase == "Deleted") {
                co_return ApiResult::failure("not_found", "app events have been deleted");
            }
            auto created = control::settled(co_await bus.create_topic(topic));
            if (!created) {
                co_return ApiResult::failure("store_unavailable", created.error().message);
            }
            json event{{"app_id", app->id},
                       {"customer_id", app->customer_id},
                       {"type", input.value("type", "rollout")},
                       {"generation", app->generation},
                       {"message", input.value("message", "")},
                       {"status", input.value("event", input.value("status", json::object()))},
                       {"principal", context.acting_identity}};
            auto published = control::settled(
                co_await bus.publish(topic, std::move(event), app->id));
            if (!published) {
                co_return ApiResult::failure("store_unavailable", published.error().message);
            }
            app_audit(
                context, "app.event.publish", *app, {{"type", input.value("type", "rollout")}});
            co_return ApiResult::success({{"event_id", *published}});
        }});
    registry.add(
        Capability{.name = "receive_app_events",
                   .title = "Receive app events",
                   .description = "Long-poll one app's lifecycle, rollout, and pod events.",
                   .input_schema = app_input_schema(
                       {{"from_sequence", {{"type", "integer"}, {"minimum", 1}}},
                        {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}}}),
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Read,
                   .long_poll = true,
                   .object = "customer",
                   .action = "read",
                   .resolve_object = customer_object,
                   .async_handler = [](const json& input,
                                       ApiContext& context) -> liquid::runtime::task<ApiResult> {
                       auto app = owned_app(input, context);
                       if (!app) {
                           co_return app.error();
                       }
                       if (context.workspaces == nullptr) {
                           co_return ApiResult::failure("store_unavailable",
                                                        "the app event bus is not configured");
                       }
                       WorkspaceApiBus bus(context, app->customer_id, app->workspace_id);
                       auto batch = control::settled(co_await bus.receive(
                           app_events_topic(app->id),
                           input.value("from_sequence", std::uint64_t{1}),
                           1000,
                           std::chrono::milliseconds(input.value("wait_ms", 0))));
                       if (!batch) {
                           co_return ApiResult::failure("store_unavailable", batch.error().message);
                       }
                       json events = json::array();
                       for (const control::BusEvent& event : batch->events) {
                           if (event.payload.is_object()) {
                               json value = event.payload;
                               value["sequence"] = event.sequence;
                               events.push_back(std::move(value));
                           }
                       }
                       app_audit(context, "app.events.read", *app);
                       co_return ApiResult::success({{"events", std::move(events)},
                                                     {"next_sequence", batch->next_sequence}});
                   }});
}

} // namespace lake::api
