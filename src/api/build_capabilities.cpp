#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <expected>
#include <string_view>

#include "lake/control/agent_bus.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
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

json build_input_schema(json properties = json::object(), json required = json::array()) {
    properties["build_id"] = {{"type", "string"}};
    required.push_back("build_id");
    return input_schema(std::move(properties), std::move(required));
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_object = "customer/" + customer_id + "/builds";
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

std::expected<control::Build, ApiResult> owned_build(const json& input, ApiContext& context) {
    auto build = context.store->get_build(input.at("build_id"));
    if (!build || build->customer_id != input.at("customer_id").get<std::string>() ||
        build->workspace_id != input.at("workspace_id").get<std::string>()) {
        return std::unexpected(ApiResult::failure("not_found", "no such build in this space"));
    }
    return *build;
}

void build_audit(ApiContext& context,
                 std::string kind,
                 const control::Build& build,
                 json details = json::object()) {
    context.audit_kind = std::move(kind);
    context.audit_object = "build/" + build.id;
    context.audit_details = {{"customer_id", build.customer_id},
                             {"build_id", build.id},
                             {"repository", build.repository},
                             {"revision", build.revision}};
    context.audit_details.update(std::move(details));
}

ApiResult store_failure(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

bool valid_target(std::string_view target) {
    const std::size_t colon = target.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == target.size() ||
        target.contains('/') || target.size() > 256) {
        return false;
    }
    const std::string_view name = target.substr(0, colon);
    const std::string_view tag = target.substr(colon + 1);
    const auto name_character = [](const unsigned char character) {
        return std::islower(character) || std::isdigit(character) || character == '.' ||
               character == '_' || character == '-';
    };
    const auto tag_character = [](const unsigned char character) {
        return std::isalnum(character) || character == '.' || character == '_' || character == '-';
    };
    return std::isalnum(static_cast<unsigned char>(name.front())) &&
           std::isalnum(static_cast<unsigned char>(name.back())) &&
           std::ranges::all_of(name, name_character) && tag.size() <= 128 &&
           std::isalnum(static_cast<unsigned char>(tag.front())) &&
           std::ranges::all_of(tag, tag_character);
}

bool valid_source_path(std::string_view path, bool allow_inline = false) {
    if (path.empty() || path.contains('\0') || path.contains('\\') || path.starts_with('/') ||
        (!allow_inline && path.starts_with("inline:"))) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view component = path.substr(
            start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (component == ".." || component == ".git") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

std::expected<std::string, ApiResult> registry_authority(const ApiContext& context) {
    std::string value = context.internal_url;
    if (value.starts_with("http://")) {
        value.erase(0, 7);
    } else if (value.starts_with("https://")) {
        value.erase(0, 8);
    } else {
        return std::unexpected(ApiResult::failure(
            "store_unavailable", "LAKE_INTERNAL_URL must be an HTTP(S) in-cluster Service URL"));
    }
    while (value.ends_with('/')) {
        value.pop_back();
    }
    if (value.empty() || value.contains('/') || value.contains('@') || value.contains('?') ||
        value.contains('#') || std::ranges::any_of(value, [](const unsigned char character) {
            return std::isspace(character);
        })) {
        return std::unexpected(ApiResult::failure(
            "store_unavailable", "LAKE_INTERNAL_URL must contain only the Service authority"));
    }
    return value;
}

std::optional<std::string> invalid_build_args(const json& arguments) {
    if (!arguments.is_object()) {
        return "build_args must be an object of string values";
    }
    static constexpr std::array<std::string_view, 3> allowed{
        "PYTHON_VERSION", "APP_DEPS", "APP_RUNTIME_IMAGE"};
    for (const auto& [name, value] : arguments.items()) {
        if (!value.is_string()) {
            return "build argument " + name + " must be a string";
        }
        if (std::ranges::find(allowed, name) == allowed.end()) {
            return "build argument " + name + " is not in BUILD_ARG_ALLOW";
        }
    }
    return std::nullopt;
}

std::expected<control::Build, ApiResult> create_build(const json& input,
                                                      ApiContext& context,
                                                      std::string repository,
                                                      std::string revision,
                                                      std::string source_context,
                                                      std::string dockerfile,
                                                      std::string target,
                                                      json build_args) {
    auto customer = context.store->get_customer(input.at("customer_id"));
    if (!customer) {
        return std::unexpected(store_failure(customer.error()));
    }
    auto source = context.store->get_repository(repository);
    if (!source || source->customer_id != customer->id || !source->deleted_at.empty()) {
        return std::unexpected(ApiResult::failure("not_found", "no such repository in this space"));
    }
    if (revision.empty()) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "revision is required", "/revision"));
    }
    if (!valid_source_path(source_context)) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "context must be a repository-relative path", "/context"));
    }
    if (dockerfile.empty() || dockerfile.contains('\0') ||
        (!dockerfile.starts_with("inline:") && !valid_source_path(dockerfile))) {
        return std::unexpected(
            ApiResult::failure("invalid_input",
                               "dockerfile must be inline text or a repository-relative path",
                               "/dockerfile"));
    }
    if (!valid_target(target)) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "target must be name:tag", "/target"));
    }
    if (const auto invalid = invalid_build_args(build_args)) {
        return std::unexpected(ApiResult::failure("invalid_input", *invalid, "/build_args"));
    }
    auto authority = registry_authority(context);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    control::NewBuild requested;
    requested.customer_id = customer->id;
    requested.workspace_id = input.at("workspace_id");
    requested.repository = std::move(repository);
    requested.revision = std::move(revision);
    requested.context = std::move(source_context);
    requested.dockerfile = std::move(dockerfile);
    requested.target = *authority + "/" + customer->name + "/" + target;
    requested.build_args = std::move(build_args);
    auto created = context.store->create_build(requested);
    if (!created) {
        return std::unexpected(store_failure(created.error()));
    }
    return *created;
}

std::string build_events_topic(std::string_view build_id) {
    return "build-events." + std::string(build_id);
}

json event_json(const control::BusEvent& event) {
    if (!event.payload.is_object()) {
        return nullptr;
    }
    json value = event.payload;
    value["sequence"] = event.sequence;
    return value;
}

} // namespace

void register_build_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "build_image",
        .title = "Build image",
        .description = "Queue an in-cluster image build and return without waiting for its Job.",
        .input_schema = input_schema({{"repository", {{"type", "string"}}},
                                      {"revision", {{"type", "string"}}},
                                      {"context", {{"type", "string"}}},
                                      {"dockerfile", {{"type", "string"}}},
                                      {"target", {{"type", "string"}}},
                                      {"build_args", {{"type", "object"}}}},
                                     json::array({"repository", "revision", "target"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            auto build = create_build(input,
                                      context,
                                      input.at("repository"),
                                      input.at("revision"),
                                      input.value("context", "."),
                                      input.value("dockerfile", "Dockerfile"),
                                      input.at("target"),
                                      input.value("build_args", json::object()));
            if (!build) {
                return build.error();
            }
            build_audit(context, "build.create", *build);
            return ApiResult::success({{"build_id", build->id}});
        }});
    registry.add(Capability{
        .name = "build_app",
        .title = "Build app image",
        .description = "Queue an app-runtime image build for one source-backed Ship app.",
        .input_schema = input_schema({{"app", {{"type", "string"}}}}, json::array({"app"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            if (context.apps_runtime_image.empty()) {
                return ApiResult::failure("store_unavailable",
                                          "LAKE_APPS_RUNTIME_IMAGE is not configured");
            }
            auto app = context.store->get_web_app(input.at("app"));
            if (!app || app->customer_id != input.at("customer_id").get<std::string>() ||
                app->workspace_id != input.at("workspace_id").get<std::string>() ||
                app->desired == "deleted") {
                return ApiResult::failure("not_found", "no such app in this space");
            }
            if (app->mode != "runtime" || app->source_repository.empty() ||
                app->source_revision.empty()) {
                return ApiResult::failure(
                    "conflict", "only a source-backed runtime app can be built as an image");
            }
            json arguments = json::object();
            arguments["APP_RUNTIME_IMAGE"] = context.apps_runtime_image;
            if (app->env.contains("APP_DEPS") && app->env.at("APP_DEPS").is_string()) {
                arguments["APP_DEPS"] = app->env.at("APP_DEPS");
            }
            // app-runtime's /app/.venv is built by uv (no pip) and owned by root, while the image
            // runs as uid 10001: install with the base image's system pip into that venv as root,
            // with globbing off so extras such as psycopg[binary] stay literal.
            const std::string dockerfile =
                "inline:ARG APP_RUNTIME_IMAGE\nFROM ${APP_RUNTIME_IMAGE}\nARG APP_DEPS=\"\"\n"
                "USER root\nRUN set -f; if [ -n \"$APP_DEPS\" ]; then "
                "/usr/local/bin/python -m pip --python /app/.venv/bin/python install "
                "--no-cache-dir $APP_DEPS; fi\nUSER 10001\nCOPY . /app\n";
            const std::string target = app->name + ":build-" + control::new_uuid_v4().substr(0, 8);
            auto build = create_build(input,
                                      context,
                                      app->source_repository,
                                      app->source_revision,
                                      app->source_path.empty() ? "." : app->source_path,
                                      dockerfile,
                                      target,
                                      arguments);
            if (!build) {
                return build.error();
            }
            build_audit(context, "build.app.create", *build, {{"app_id", app->id}});
            return ApiResult::success(
                {{"build_id", build->id}, {"app_id", app->id}, {"target", build->target}});
        }});
    registry.add(Capability{.name = "get_build",
                            .title = "Get build",
                            .description = "Read one image build's desired and observed state.",
                            .input_schema = build_input_schema(),
                            .output_schema = {{"type", "object"}},
                            .kind = CapabilityKind::Read,
                            .object = "customer",
                            .action = "read",
                            .resolve_object = customer_object,
                            .handler = [](const json& input, ApiContext& context) {
                                auto build = owned_build(input, context);
                                if (!build) {
                                    return build.error();
                                }
                                build_audit(context, "build.read", *build);
                                return ApiResult::success(control::build_json(*build));
                            }});
    registry.add(Capability{
        .name = "list_builds",
        .title = "List builds",
        .description = "List a space's image builds, newest first.",
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
                    return ApiResult::failure(
                        "unauthorized", "principal is not allowed to read this space's builds");
                }
            }
            auto builds = context.store->list_builds(input.at("customer_id"));
            if (!builds) {
                return store_failure(builds.error());
            }
            json items = json::array();
            for (const control::Build& build : *builds) {
                if (build.workspace_id != input.at("workspace_id").get<std::string>()) {
                    continue;
                }
                items.push_back(control::build_json(build));
            }
            context.audit_kind = "build.list";
            return ApiResult::success({{"builds", std::move(items)}});
        }});
    registry.add(
        Capability{.name = "cancel_build",
                   .title = "Cancel build",
                   .description = "Ask the operator to cancel one queued or running build.",
                   .input_schema = build_input_schema(),
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Action,
                   .object = "customer",
                   .action = "write",
                   .resolve_object = customer_object,
                   .handler = [](const json& input, ApiContext& context) {
                       auto build = owned_build(input, context);
                       if (!build) {
                           return build.error();
                       }
                       auto cancelled = context.store->cancel_build(build->id);
                       if (!cancelled) {
                           return store_failure(cancelled.error());
                       }
                       build_audit(context, "build.cancel", *cancelled);
                       return ApiResult::success(control::build_json(*cancelled));
                   }});
    registry.add(Capability{
        .name = "publish_build_event",
        .title = "Publish build event",
        .description = "Publish one builder or operator event on this build's retained topic.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"build_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"customer_id", {{"type", "string"}}},
                           {"type",
                            {{"type", "string"},
                             {"enum", json::array({"phase", "log", "done", "error"})}}},
                           {"phase",
                            {{"type", "string"},
                             {"enum", json::array({"fetching", "building", "pushing", "done"})}}},
                           {"step", {{"type", "integer"}, {"minimum", 0}}},
                           {"steps", {{"type", "integer"}, {"minimum", 0}}},
                           {"lines", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                           {"digest", {{"type", "string"}}},
                           {"size", {{"type", "integer"}, {"minimum", 0}}},
                           {"message", {{"type", "string"}}}}},
                         {"required", json::array({"build_id", "workspace_id", "type"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = [](const json& input,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            auto build = context.store->get_build(input.at("build_id"));
            if (!build ||
                build->workspace_id != input.at("workspace_id").get<std::string>() ||
                (input.contains("customer_id") && input.at("customer_id") != build->customer_id)) {
                return std::unexpected(
                    ApiError{"not_found", "no such build in this space", "/build_id"});
            }
            json resolved = input;
            resolved["customer_id"] = build->customer_id;
            return customer_object(resolved, context);
        },
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto build = context.store->get_build(input.at("build_id"));
            if (!build ||
                build->workspace_id != input.at("workspace_id").get<std::string>() ||
                (input.contains("customer_id") && input.at("customer_id") != build->customer_id)) {
                co_return ApiResult::failure("not_found", "no such build in this space");
            }
            if (input.dump().size() > 65'536) {
                co_return ApiResult::failure("invalid_input",
                                             "one build event must not exceed 64 KiB");
            }
            if (context.workspaces == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the build event bus is not configured");
            }
            const std::string topic = build_events_topic(build->id);
            WorkspaceApiBus bus(context, build->customer_id, build->workspace_id);
            if (control::build_topic_expired(*build)) {
                co_return ApiResult::failure("not_found", "build events have expired");
            }
            auto created = control::settled(co_await bus.create_topic(topic));
            if (!created) {
                co_return ApiResult::failure("store_unavailable", created.error().message);
            }
            json event = input;
            event["customer_id"] = build->customer_id;
            event["principal"] = context.acting_identity;
            auto published = control::settled(
                co_await bus.publish(topic, std::move(event), build->id));
            if (!published) {
                co_return ApiResult::failure("store_unavailable", published.error().message);
            }
            build_audit(context, "build.event.publish", *build, {{"type", input.at("type")}});
            co_return ApiResult::success({{"event_id", *published}});
        }});
    registry.add(Capability{
        .name = "receive_build_events",
        .title = "Receive build events",
        .description = "Long-poll one build's retained progress topic from a sequence.",
        .input_schema = build_input_schema(
            {{"from_sequence", {{"type", "integer"}}}, {"wait_ms", {{"type", "integer"}}}}),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto build = owned_build(input, context);
            if (!build) {
                co_return build.error();
            }
            const auto from = std::max<std::int64_t>(input.value("from_sequence", 1), 1);
            const auto wait = std::clamp<std::int64_t>(input.value("wait_ms", 0), 0, 30'000);
            if (context.workspaces == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the build event bus is not configured");
            }
            const std::string topic = build_events_topic(build->id);
            WorkspaceApiBus bus(context, build->customer_id, build->workspace_id);
            auto batch = control::settled(co_await bus.receive(
                topic, static_cast<std::uint64_t>(from), 1000, std::chrono::milliseconds(wait)));
            if (!batch) {
                if (batch.error().code == "not_found") {
                    co_return ApiResult::success({{"events", json::array()}, {"next_sequence", from}});
                }
                co_return ApiResult::failure("store_unavailable", batch.error().message);
            }
            json events = json::array();
            for (const control::BusEvent& event : batch->events) {
                json value = event_json(event);
                if (!value.is_null()) {
                    events.push_back(std::move(value));
                }
            }
            build_audit(context, "build.events.read", *build);
            co_return ApiResult::success(
                {{"events", std::move(events)}, {"next_sequence", batch->next_sequence}});
        }});
    registry.add(Capability{
        .name = "build_logs",
        .title = "Read build logs",
        .description = "Read retained log lines and events from one build's topic.",
        .input_schema = build_input_schema(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto build = owned_build(input, context);
            if (!build) {
                co_return build.error();
            }
            if (context.workspaces == nullptr) {
                co_return ApiResult::failure("store_unavailable",
                                             "the build event bus is not configured");
            }
            const std::string topic = build_events_topic(build->id);
            WorkspaceApiBus bus(context, build->customer_id, build->workspace_id);
            json events = json::array();
            json lines = json::array();
            std::uint64_t next_sequence = 1;
            for (int page = 0; page < 100; ++page) {
                auto batch = control::settled(co_await bus.receive(
                    topic, next_sequence, 1000, std::chrono::milliseconds(0)));
                if (!batch) {
                    if (batch.error().code == "not_found") {
                        break;
                    }
                    co_return ApiResult::failure("store_unavailable", batch.error().message);
                }
                for (const control::BusEvent& event : batch->events) {
                    json value = event_json(event);
                    if (value.is_null()) {
                        continue;
                    }
                    for (const json& line : value.value("lines", json::array())) {
                        if (line.is_string()) {
                            lines.push_back(line);
                        }
                    }
                    events.push_back(std::move(value));
                }
                if (batch->events.empty() || batch->next_sequence <= next_sequence) {
                    next_sequence = batch->next_sequence;
                    break;
                }
                next_sequence = batch->next_sequence;
            }
            build_audit(context, "build.logs.read", *build);
            co_return ApiResult::success({{"lines", std::move(lines)},
                                          {"events", std::move(events)},
                                          {"next_sequence", next_sequence}});
        }});
}

} // namespace lake::api
