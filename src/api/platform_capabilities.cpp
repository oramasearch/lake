#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lake/control/platform.hpp"
#include "lake/control/platform_records.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "scan/detectors.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

// A lake started without the operator's platform source has no Deployments: the answer is
// `not_supported` (501), never `store_unavailable`, because asking again changes nothing until an
// operator configures it (lake#575, docs/api/platform.md#a-lake-without-the-platform-mirror).
// The message names no engine: the web app shows it to people (lake#583). The configuration
// (LAKE_OPERATOR_ZETTA_URL, LAKE_OPERATOR_ZETTA_TOKEN) is in the page it points to.
constexpr std::string_view kNotSetUp =
    "deployments are not set up on this Lake: an operator turns them on by connecting Lake to "
    "the cluster operator's platform source (docs/api/platform.md#a-lake-without-the-platform-"
    "mirror)";

ApiResult not_set_up() {
    return ApiResult::failure("not_supported", std::string(kNotSetUp));
}

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

// An environment's policy object stays in the control store under the environment's derived id
// (platform_environment_id), so authorization never reads the workspace: a caller without the
// role is refused before any workspace call.
std::expected<std::string, ApiError> environment_object(const json& input, ApiContext& context) {
    if (!context.platform || !context.store) {
        return std::unexpected(ApiError{"not_supported", std::string(kNotSetUp), {}});
    }
    const std::string environment = input.at("environment").get<std::string>();
    if (const auto repository = context.store->get_repository(input.at("repository_id"));
        !repository) {
        return std::unexpected(
            ApiError{repository.error().code, repository.error().message, "/repository_id"});
    }
    // Only the object under the repository's own counts; another account's is `conflict`
    // (lake#913).
    const auto object = control::environment_policy_object(
        *context.store, input.at("repository_id").get<std::string>(), environment);
    if (!object) {
        if (object.error().code == "not_found") {
            return std::unexpected(
                ApiError{"not_found", "no environment " + environment, "/environment"});
        }
        return std::unexpected(ApiError{object.error().code, object.error().message, {}});
    }
    return object->id;
}

// The records of the workspace that keeps a repository's environments: the repository's own
// (lake#358, lake#359). The caller never chooses it, and nothing falls back: a repository without
// a workspace is `conflict` at /repository_id, a workspace that is not ready `workspace_not_ready`.
std::expected<control::WorkspaceRecords, ApiError> repository_records(const ApiContext& context,
                                                                      const std::string& id) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto repository = context.store->get_repository(id);
    if (!repository) {
        return std::unexpected(
            ApiError{repository.error().code, repository.error().message, "/repository_id"});
    }
    if (repository->workspace_id.empty()) {
        return std::unexpected(ApiError{"conflict",
                                        "the repository has no workspace yet, so it has nowhere "
                                        "to keep environments; an operator must assign one",
                                        "/repository_id"});
    }
    auto records = workspace_records(context, repository->customer_id, repository->workspace_id);
    if (!records) {
        return std::unexpected(ApiError{records.error().code, records.error().message, {}});
    }
    return std::move(*records);
}

// Whether the caller may read at least one of the repository's environments, decided on their
// policy objects in the control store. A caller who may read none is answered without a workspace
// call (least privilege); the operator may read every one.
std::expected<bool, ApiError> may_read_an_environment(const json& input, ApiContext& context) {
    if (context.acting_identity == "admin") {
        return true;
    }
    const auto parent = repository_object(input, context);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    const auto children = context.store->list_policy_objects(*parent, "environment", 1000);
    if (!children) {
        return std::unexpected(ApiError{children.error().code, children.error().message, {}});
    }
    std::vector<std::string> ids;
    for (const control::PolicyObject& child : *children) {
        ids.push_back(child.id);
    }
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, ids, "read", context.authorization_cache);
    if (!decisions) {
        return std::unexpected(ApiError{decisions.error().code, decisions.error().message, {}});
    }
    return std::ranges::any_of(*decisions,
                               [](const auto& decision) { return decision.second.allowed; });
}

ApiResult refused(const ApiError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
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

// A tenant's domain may not be one of this Lake's own names (lake#949): its public host and
// aliases, the zone its public host sits in, and the apps wildcard. Empty: the host may be claimed.
std::string platform_host_problem(std::string_view requested, const ApiContext& context) {
    const auto lower = [](std::string_view text) {
        std::string out(text);
        std::ranges::transform(out, out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        while (out.ends_with('.')) {
            out.pop_back();
        }
        return out;
    };
    const std::string host = lower(requested);
    std::vector<std::string> reserved;
    if (!context.public_host.empty()) {
        const std::string own = lower(context.public_host);
        reserved.push_back(own);
        // The zone the Lake's host is in (`oramadev.net` for `lake-dev.oramadev.net`), when the
        // host is below one; a two-label host (`lake.live`) is the zone itself.
        if (std::ranges::count(own, '.') >= 2) {
            reserved.push_back(own.substr(own.find('.') + 1));
        }
    }
    for (const std::string& alias : context.host_aliases) {
        reserved.push_back(lower(alias));
    }
    std::string wildcard = lower(context.apps_wildcard);
    if (wildcard.starts_with("*.")) {
        wildcard.erase(0, 2);
    }
    if (!wildcard.empty()) {
        reserved.push_back(wildcard);
    }
    for (const std::string& name : reserved) {
        if (!name.empty() && (host == name || host.ends_with("." + name))) {
            return "host " + host + " is one of this Lake's own names (" + name +
                   "); use a domain you own";
        }
    }
    return {};
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
        .description = "List mirrored runtime environments the caller may read, from the "
                       "repository's workspace.",
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
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.platform || !context.store) {
                co_return not_set_up();
            }
            const std::string repository_id = input.at("repository_id");
            const auto readable = may_read_an_environment(input, context);
            if (!readable) {
                co_return refused(readable.error());
            }
            if (!*readable) {
                set_platform_audit(context, repository_id, "*", "platform.environments.read");
                co_return ApiResult::success(
                    {{"repository_id", repository_id}, {"items", json::array()}});
            }
            auto records = repository_records(context, repository_id);
            if (!records) {
                co_return refused(records.error());
            }
            auto completed = co_await context.platform->list_environments(*records, repository_id);
            if (!completed) {
                co_return ApiResult::failure("store_error", completed.error().message);
            }
            if (!*completed) {
                co_return platform_error(completed->error());
            }
            set_platform_audit(context, repository_id, "*", "platform.environments.read");
            co_return filter_environment_items(std::move(**completed), context);
        }});
    registry.add(Capability{
        .name = "get_environment",
        .title = "Get runtime environment",
        .description = "Get the mirrored deployment, domains, and environment values from the "
                       "repository's workspace.",
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
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.platform) {
                co_return not_set_up();
            }
            const std::string repository_id = input.at("repository_id");
            const std::string environment = input.at("environment");
            auto records = repository_records(context, repository_id);
            if (!records) {
                co_return refused(records.error());
            }
            auto completed =
                co_await context.platform->get_environment(*records, repository_id, environment);
            if (!completed) {
                co_return ApiResult::failure("store_error", completed.error().message);
            }
            if (!*completed) {
                co_return platform_error(completed->error());
            }
            set_platform_audit(context, repository_id, environment, "platform.environment.read");
            co_return ApiResult::success(std::move(**completed));
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
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.platform || !context.store) {
                co_return not_set_up();
            }
            const std::string repository_id = input.at("repository_id");
            const std::string environment = input.value("environment", "");
            const int limit = std::clamp(input.value("limit", 100), 1, 1000);
            const auto readable = may_read_an_environment(input, context);
            if (!readable) {
                co_return refused(readable.error());
            }
            if (!*readable) {
                set_platform_audit(context,
                                   repository_id,
                                   environment.empty() ? "*" : environment,
                                   "platform.deployments.read");
                co_return ApiResult::success(
                    {{"repository_id", repository_id}, {"items", json::array()}});
            }
            auto records = repository_records(context, repository_id);
            if (!records) {
                co_return refused(records.error());
            }
            auto completed = co_await context.platform->list_deployments(
                *records, repository_id, environment, limit);
            if (!completed) {
                co_return ApiResult::failure("store_error", completed.error().message);
            }
            if (!*completed) {
                co_return platform_error(completed->error());
            }
            const json& result = **completed;
            // Each row is shown only to a caller who may read its environment; the environments'
            // policy objects are found in the control store, without another workspace read.
            std::map<std::string, std::string> object_by_environment;
            std::vector<std::string> objects;
            for (const json& row : result.at("items")) {
                const std::string name = row.value("environment", "");
                if (object_by_environment.contains(name)) {
                    continue;
                }
                const auto object = context.store->find_policy_object(
                    "environment", control::platform_environment_id(repository_id, name));
                object_by_environment[name] = object ? object->id : std::string();
                if (object) {
                    objects.push_back(object->id);
                }
            }
            const auto decisions = control::authorize_batch(*context.store,
                                                            context.acting_identity,
                                                            objects,
                                                            "read",
                                                            context.authorization_cache);
            if (!decisions) {
                co_return store_error(decisions.error());
            }
            json filtered = json::array();
            for (const json& row : result.at("items")) {
                const std::string& object_id =
                    object_by_environment.at(row.value("environment", ""));
                if (!object_id.empty() && decisions->at(object_id).allowed) {
                    filtered.push_back(row);
                }
            }
            json output = result;
            output["items"] = std::move(filtered);
            set_platform_audit(context,
                               repository_id,
                               environment.empty() ? "*" : environment,
                               "platform.deployments.read");
            co_return ApiResult::success(std::move(output));
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
                co_return not_set_up();
            }
            const std::string direction = input.value("direction", "forward");
            if (direction != "forward" && direction != "backward") {
                co_return ApiResult::failure(
                    "invalid_input", "direction must be forward or backward", "/direction");
            }
            const std::string repository_id = input.at("repository_id");
            const std::string environment = input.at("environment");
            auto records = repository_records(context, repository_id);
            if (!records) {
                co_return refused(records.error());
            }
            auto completed = co_await context.platform->environment_logs(
                *records,
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
                    co_return not_set_up();
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
                auto records = repository_records(context, repository_id);
                if (!records) {
                    co_return refused(records.error());
                }
                auto completed = co_await context.platform->set_environment_env(
                    *records, repository_id, environment, key, value, deleted,
                    context.acting_identity);
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
                    co_return not_set_up();
                }
                const std::string host = input.at("host");
                const std::string tls = input.value("tls", "");
                if (!valid_domain(host)) {
                    co_return ApiResult::failure(
                        "invalid_input", "host must be a DNS hostname", "/host");
                }
                if (!deleted) {
                    if (const std::string problem = platform_host_problem(host, context);
                        !problem.empty()) {
                        co_return ApiResult::failure("invalid_input", problem, "/host");
                    }
                }
                if (!deleted && tls != "acme" && tls != "provided") {
                    co_return ApiResult::failure(
                        "invalid_input", "tls must be acme or provided", "/tls");
                }
                const std::string repository_id = input.at("repository_id");
                const std::string environment = input.at("environment");
                auto records = repository_records(context, repository_id);
                if (!records) {
                    co_return refused(records.error());
                }
                auto completed = co_await context.platform->change_domain(
                    *records, repository_id, environment, host, tls, deleted,
                    context.acting_identity);
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
