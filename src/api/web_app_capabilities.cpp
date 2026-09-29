#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lake/control/agent_bus.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "scan/detectors.hpp"
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
        return std::unexpected(ApiResult::failure("not_found", "no such app in this space"));
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

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 31 || name.front() < 'a' || name.front() > 'z' ||
        name.back() == '-') {
        return false;
    }
    return std::ranges::all_of(name, [](const unsigned char character) {
        return std::islower(character) || std::isdigit(character) || character == '-';
    });
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
    const std::string host = input.value("host", name + "." + wildcard);
    if (!valid_domain(host) || host == wildcard || !host.ends_with("." + wildcard) ||
        host.substr(0, host.size() - wildcard.size() - 1).contains('.')) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "host must be below the lake apps wildcard " + wildcard, "/host"));
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
                "the value looks like a secret; store it as a Secret and pass secret_ref",
                "/env/" + key);
        }
    }
    return std::nullopt;
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
        if (value.is_object() && value.size() == 1 && value.contains("secret") &&
            value.at("secret").is_string() && !value.at("secret").get<std::string>().empty()) {
            secret_refs[key] = value.at("secret");
            continue;
        }
        if (!value.is_string()) {
            return std::unexpected(ApiResult::failure(
                "invalid_input",
                "env values must be strings or {secret: <operator reference>}",
                "/env/" + key));
        }
        if (flagged_secret(key, value.get<std::string>())) {
            return std::unexpected(ApiResult::failure(
                "secret_required",
                "the value looks like a secret; use {secret: <operator reference>}",
                "/env/" + key));
        }
        plain[key] = value;
    }
    return std::pair{std::move(plain), std::move(secret_refs)};
}

std::expected<json, ApiResult>
normalized_bindings(const json& value, const std::string& customer_id, ApiContext& context) {
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
    for (const json& service_id : services) {
        if (!service_id.is_string()) {
            return std::unexpected(ApiResult::failure(
                "invalid_input", "service ids must be strings", "/bindings/services"));
        }
        auto service = context.store->get_service(service_id.get<std::string>());
        if (!service || service->customer_id != customer_id || service->desired != "present") {
            return std::unexpected(ApiResult::failure(
                "not_found", "no such service in this space", "/bindings/services"));
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
                                     "the app event bus is not configured on this lake");
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
            "not_found", "no such repository in this space", "/source/repository");
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
        return std::unexpected(ApiError{"not_found", "no such app in this space", "/app_id"});
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
    }
    return std::nullopt;
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
            {"image", {{"type", "string"}}}};
}

} // namespace

void register_web_app_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_apps",
        .title = "List apps",
        .description = "List the Ship apps in a space, including desired and observed state.",
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
                                              "principal is not allowed to read this space's apps");
                }
            }
            auto apps = context.store->list_web_apps(input.at("customer_id"));
            if (!apps) {
                return store_failure(apps.error());
            }
            json items = json::array();
            for (const control::WebApp& app : *apps) {
                if (app.workspace_id != input.at("workspace_id").get<std::string>()) {
                    continue;
                }
                items.push_back(control::web_app_json(app));
            }
            context.audit_kind = "app.list";
            return ApiResult::success({{"apps", std::move(items)},
                                       {"app_limit", context.apps_per_customer},
                                       {"replica_limit", context.app_replicas_per_customer}});
        }});
    registry.add(Capability{.name = "get_app",
                            .title = "Get app",
                            .description = "Get one Ship app in a space.",
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
                                return ApiResult::success(control::web_app_json(*app));
                            }});
    registry.add(Capability{
        .name = "create_app",
        .title = "Create app",
        .description = "Create runtime or image desired state for the app operator.",
        .input_schema = input_schema(
            {{"name", {{"type", "string"}}},
             {"mode", {{"type", "string"}, {"enum", json::array({"runtime", "image"})}}},
             {"source", deployment_properties().at("source")},
             {"image", {{"type", "string"}}},
             {"framework", {{"type", "string"}}},
             {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
             {"replicas", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
             {"host", {{"type", json::array({"string", "null"})}}},
             {"auth",
              {{"type", "string"}, {"enum", json::array({"lake", "none", "public"})}}},
             {"bindings", {{"type", "object"}}},
             {"env", {{"type", "object"}}}},
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
            json host_input = input;
            if (host_input.value("host", json()).is_null()) {
                host_input.erase("host");
            }
            auto host = app_host(host_input, name, context);
            if (!host) {
                co_return host.error();
            }
            auto bindings = normalized_bindings(
                input.value("bindings", json::object()), input.at("customer_id"), context);
            if (!bindings) {
                co_return bindings.error();
            }
            const int replicas = input.value("replicas", 1);
            if (replicas < 1 || replicas > 100) {
                co_return ApiResult::failure(
                    "invalid_input", "replicas must be between 1 and 100", "/replicas");
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
            auto created = context.store->create_web_app(app);
            if (!created) {
                co_return store_failure(created.error());
            }
            app_audit(context, "app.create", *created);
            co_return co_await publish_event(context, *created, "create");
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
                co_return co_await patch_and_publish(normalized,
                                                     context,
                                                     deployment_patch(normalized),
                                                     rollback ? "app.rollback" : "app.deploy",
                                                     rollback ? "rollback" : "deploy",
                                                     std::move(details));
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
        .description = "Set one plain value or one opaque Secret reference after secret scanning.",
        .input_schema = app_input_schema({{"key", {{"type", "string"}}},
                                          {"value", {{"type", "string"}}},
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
            if (input.contains("value") == input.contains("secret_ref")) {
                co_return ApiResult::failure("invalid_input",
                                             "provide exactly one of value or secret_ref");
            }
            control::WebAppPatch patch;
            json details{{"key", key}};
            if (input.contains("value")) {
                const std::string value = input.at("value");
                if (flagged_secret(key, value)) {
                    co_return ApiResult::failure(
                        "secret_required",
                        "the value looks like a secret; store it as a Secret and pass secret_ref",
                        "/value");
                }
                app->env[key] = value;
                app->secret_env_refs.erase(key);
                details["secret"] = false;
            } else {
                const std::string reference = input.at("secret_ref");
                if (reference.empty()) {
                    co_return ApiResult::failure(
                        "invalid_input", "secret_ref cannot be empty", "/secret_ref");
                }
                app->env.erase(key);
                app->secret_env_refs[key] = reference;
                details["secret"] = true;
            }
            patch.env = app->env;
            patch.secret_env_refs = app->secret_env_refs;
            co_return co_await patch_and_publish(
                input, context, std::move(patch), "app.env.set", "env", std::move(details));
        }});
    registry.add(Capability{
        .name = "unset_app_env",
        .title = "Unset app environment value",
        .description = "Remove one plain value or Secret reference from desired state.",
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
            .description = "Change the app's desired lifecycle state.",
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
        .description = "Require lake sign-in or make an app public after explicit confirmation.",
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
        .description = "Set desired replicas within the space's aggregate quota.",
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
        .name = "set_app_zetta_binding",
        .title = "Set app Zetta binding",
        .description = "Attach or detach read-only access to the space's workspace Zetta.",
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
            auto service = context.store->get_service(input.at("service_id"));
            if (!service || service->customer_id != app->customer_id ||
                service->desired != "present") {
                co_return ApiResult::failure("not_found", "no such service in this space");
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
                co_return ApiResult::failure("not_found", "no such app in this space");
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
