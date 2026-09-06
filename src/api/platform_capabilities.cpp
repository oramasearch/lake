#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "lake/control/platform.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "scan/detectors.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult platform_error(const control::PlatformError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::string, ApiError> repository_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("repository", input.at("repository_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/repository_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> environment_object(const json& input, ApiContext& context) {
    if (!context.platform) {
        return std::unexpected(
            ApiError{"store_unavailable", "the platform mirror is not configured", {}});
    }
    const std::string environment = input.contains("environment")
                                        ? input.at("environment").get<std::string>()
                                        : input.at("name").get<std::string>();
    const auto document = context.platform->get_environment(input.at("repository_id"), environment);
    if (!document) {
        return std::unexpected(ApiError{document.error().code, document.error().message, {}});
    }
    return document->at("object_id").get<std::string>();
}

void set_platform_audit(ApiContext& context,
                        const std::string& repository_id,
                        const std::string& environment,
                        const std::string& kind) {
    const auto repository = context.store->get_repository(repository_id);
    if (repository) {
        context.audit_customer_id = repository->customer_id;
    }
    context.audit_kind = kind;
    context.audit_object = "environment/" + repository_id + "/" + environment;
    context.audit_details = {{"repository_id", repository_id}, {"environment", environment}};
}

ApiResult filter_environment_items(json document, ApiContext& context) {
    std::vector<std::string> object_ids;
    for (const json& item : document.at("items")) {
        object_ids.push_back(item.at("object_id").get<std::string>());
    }
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, object_ids, "read", context.authorization_cache);
    if (!decisions) {
        return store_error(decisions.error());
    }
    json filtered = json::array();
    for (const json& item : document.at("items")) {
        if (decisions->at(item.at("object_id").get<std::string>()).allowed) {
            filtered.push_back(item);
        }
    }
    document["items"] = std::move(filtered);
    return ApiResult::success(std::move(document));
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
        liquid::scan_detail::detector_set{}.scan("environment.env", bytes, {});
    return !report.blocked.empty();
}

bool valid_domain(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' ||
        host.contains("//")) {
        return false;
    }
    return std::ranges::all_of(host, [](const unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '.';
    });
}

json object_schema() {
    return {{"type", "object"}};
}

json repository_environment_properties() {
    return {{"repository_id", {{"type", "string"}}}, {"environment", {{"type", "string"}}}};
}

} // namespace

void register_platform_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_environments",
        .title = "List repository environments",
        .description = "List mirrored runtime environments the caller may read.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"repository_id", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "environment",
        .action = "read",
        .resolve_object = repository_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.platform || !context.store) {
                return ApiResult::failure("store_unavailable",
                                          "the platform mirror is not configured");
            }
            const std::string repository_id = input.at("repository_id");
            const auto result = context.platform->list_environments(repository_id);
            if (!result) {
                return platform_error(result.error());
            }
            set_platform_audit(context, repository_id, "*", "platform.environments.read");
            return filter_environment_items(*result, context);
        }});
    registry.add(
        Capability{.name = "get_environment",
                   .title = "Get runtime environment",
                   .description = "Get the mirrored deployment, domains, and environment values.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"repository_id", {{"type", "string"}}},
                                      {"environment", {{"type", "string"}}}}},
                                    {"required", json::array({"repository_id", "environment"})},
                                    {"additionalProperties", false}},
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "environment",
                   .action = "read",
                   .resolve_object = environment_object,
                   .resolves_operator_audit_target = true,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.platform) {
                           return ApiResult::failure("store_unavailable",
                                                     "the platform mirror is not configured");
                       }
                       const auto result = context.platform->get_environment(
                           input.at("repository_id"), input.at("environment"));
                       if (!result) {
                           return platform_error(result.error());
                       }
                       set_platform_audit(context,
                                          input.at("repository_id"),
                                          input.at("environment"),
                                          "platform.environment.read");
                       return ApiResult::success(*result);
                   }});
    registry.add(Capability{
        .name = "list_deployments",
        .title = "List mirrored deployments",
        .description = "List latest-wins deployment records for a repository or environment.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"environment", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"repository_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "environment",
        .action = "read",
        .resolve_object = repository_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.platform || !context.store) {
                return ApiResult::failure("store_unavailable",
                                          "the platform mirror is not configured");
            }
            const std::string repository_id = input.at("repository_id");
            const std::string environment = input.value("environment", "");
            const int limit = std::clamp(input.value("limit", 100), 1, 1000);
            const auto result =
                context.platform->list_deployments(repository_id, environment, limit);
            if (!result) {
                return platform_error(result.error());
            }
            json filtered = json::array();
            std::map<std::string, std::string> object_by_environment;
            std::vector<std::string> objects;
            for (const json& row : result->at("items")) {
                const std::string name = row.value("environment", "");
                if (object_by_environment.contains(name)) {
                    continue;
                }
                const auto document = context.platform->get_environment(repository_id, name);
                if (!document) {
                    return platform_error(document.error());
                }
                const std::string object_id = document->at("object_id");
                object_by_environment[name] = object_id;
                objects.push_back(object_id);
            }
            const auto decisions = control::authorize_batch(*context.store,
                                                            context.acting_identity,
                                                            objects,
                                                            "read",
                                                            context.authorization_cache);
            if (!decisions) {
                return store_error(decisions.error());
            }
            for (const json& row : result->at("items")) {
                if (decisions->at(object_by_environment.at(row.value("environment", ""))).allowed) {
                    filtered.push_back(row);
                }
            }
            json output = *result;
            output["items"] = std::move(filtered);
            set_platform_audit(context,
                               repository_id,
                               environment.empty() ? "*" : environment,
                               "platform.deployments.read");
            return ApiResult::success(std::move(output));
        }});
    registry.add(Capability{
        .name = "environment_logs",
        .title = "Read environment logs",
        .description = "Read one cursor page from the configured Loki-compatible source.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"environment", {{"type", "string"}}},
                           {"since", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}},
                           {"direction", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id", "environment"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "environment",
        .action = "read",
        .resolve_object = environment_object,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.platform) {
                co_return ApiResult::failure("store_unavailable",
                                             "the platform mirror is not configured");
            }
            const std::string direction = input.value("direction", "forward");
            if (direction != "forward" && direction != "backward") {
                co_return ApiResult::failure(
                    "invalid_input", "direction must be forward or backward", "/direction");
            }
            const std::string repository_id = input.at("repository_id");
            const std::string environment = input.at("environment");
            auto completed = co_await context.platform->environment_logs(
                repository_id,
                environment,
                input.value("since", ""),
                std::clamp(input.value("limit", 200), 1, 1000),
                direction);
            if (!completed) {
                co_return ApiResult::failure("store_error", completed.error().message);
            }
            auto result = std::move(*completed);
            if (!result) {
                co_return platform_error(result.error());
            }
            set_platform_audit(
                context, repository_id, environment, "platform.environment.logs.read");
            co_return ApiResult::success(*result);
        }});

    const auto register_env_action = [&](std::string name, bool deleted) {
        registry.add(Capability{
            .name = std::move(name),
            .title = deleted ? "Unset environment value" : "Set environment value",
            .description = deleted ? "Append a tombstone for one plain environment value."
                                   : "Append one plain environment value after secret scanning.",
            .input_schema = {{"type", "object"},
                             {"properties",
                              {{"repository_id", {{"type", "string"}}},
                               {"environment", {{"type", "string"}}},
                               {"key", {{"type", "string"}}},
                               {"value", {{"type", "string"}}}}},
                             {"required",
                              deleted
                                  ? json::array({"repository_id", "environment", "key"})
                                  : json::array({"repository_id", "environment", "key", "value"})},
                             {"additionalProperties", false}},
            .output_schema = object_schema(),
            .kind = CapabilityKind::Action,
            .object = "environment",
            .action = "write",
            .resolve_object = environment_object,
            .resolves_operator_audit_target = true,
            .async_handler = [deleted](const json& input,
                                       ApiContext& context) -> liquid::runtime::task<ApiResult> {
                if (!context.platform) {
                    co_return ApiResult::failure("store_unavailable",
                                                 "the platform mirror is not configured");
                }
                const std::string key = input.at("key");
                const std::string value = input.value("value", "");
                if (!valid_env_key(key)) {
                    co_return ApiResult::failure(
                        "invalid_input", "key must be an environment variable name", "/key");
                }
                if (!deleted && flagged_secret(key, value)) {
                    co_return ApiResult::failure("invalid_input", "use a sealed secret", "/value");
                }
                const std::string repository_id = input.at("repository_id");
                const std::string environment = input.at("environment");
                auto completed = co_await context.platform->set_environment_env(
                    repository_id, environment, key, value, deleted, context.acting_identity);
                if (!completed) {
                    co_return ApiResult::failure("store_error", completed.error().message);
                }
                auto result = std::move(*completed);
                if (!result) {
                    co_return platform_error(result.error());
                }
                set_platform_audit(context,
                                   repository_id,
                                   environment,
                                   deleted ? "platform.environment.env.unset"
                                           : "platform.environment.env.set");
                context.audit_details["key"] = key;
                co_return ApiResult::success({{"repository_id", repository_id},
                                              {"environment", environment},
                                              {"key", key},
                                              {"deleted", deleted}});
            }});
    };
    register_env_action("set_environment_env", false);
    register_env_action("unset_environment_env", true);

    const auto register_domain_action = [&](std::string name, bool deleted) {
        registry.add(Capability{
            .name = std::move(name),
            .title = deleted ? "Delete environment domain" : "Create environment domain",
            .description = deleted ? "Append a domain tombstone for operator reconciliation."
                                   : "Append an ACME or provided-certificate domain request.",
            .input_schema = {{"type", "object"},
                             {"properties",
                              {{"repository_id", {{"type", "string"}}},
                               {"environment", {{"type", "string"}}},
                               {"host", {{"type", "string"}}},
                               {"tls", {{"type", "string"}}}}},
                             {"required",
                              deleted
                                  ? json::array({"repository_id", "environment", "host"})
                                  : json::array({"repository_id", "environment", "host", "tls"})},
                             {"additionalProperties", false}},
            .output_schema = object_schema(),
            .kind = CapabilityKind::Action,
            .object = "environment",
            .action = "deploy",
            .resolve_object = environment_object,
            .resolves_operator_audit_target = true,
            .async_handler = [deleted](const json& input,
                                       ApiContext& context) -> liquid::runtime::task<ApiResult> {
                if (!context.platform) {
                    co_return ApiResult::failure("store_unavailable",
                                                 "the platform mirror is not configured");
                }
                const std::string host = input.at("host");
                const std::string tls = input.value("tls", "");
                if (!valid_domain(host)) {
                    co_return ApiResult::failure(
                        "invalid_input", "host must be a DNS hostname", "/host");
                }
                if (!deleted && tls != "acme" && tls != "provided") {
                    co_return ApiResult::failure(
                        "invalid_input", "tls must be acme or provided", "/tls");
                }
                const std::string repository_id = input.at("repository_id");
                const std::string environment = input.at("environment");
                auto completed = co_await context.platform->change_domain(
                    repository_id, environment, host, tls, deleted, context.acting_identity);
                if (!completed) {
                    co_return ApiResult::failure("store_error", completed.error().message);
                }
                auto result = std::move(*completed);
                if (!result) {
                    co_return platform_error(result.error());
                }
                set_platform_audit(context,
                                   repository_id,
                                   environment,
                                   deleted ? "platform.domain.delete" : "platform.domain.create");
                context.audit_details["host"] = host;
                co_return ApiResult::success({{"repository_id", repository_id},
                                              {"environment", environment},
                                              {"host", host},
                                              {"status", "pending"},
                                              {"deleted", deleted}});
            }});
    };
    register_domain_action("create_domain", false);
    register_domain_action("delete_domain", true);
}

} // namespace lake::api
