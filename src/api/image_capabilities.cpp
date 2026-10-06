// Tenant image routes (lake#383 phase A, decision 54 S4-S6, docs/api/registry.md#where-a-tenant-
// image-lives): create_image_repository, list_image_repositories, the legacy routing rule the
// one-shot copy uses, and the image bytes metering counts.

#include "lake/api/images.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "lake/api/capabilities.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/web_apps.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

bool sha256_digest(std::string_view value) {
    if (!value.starts_with("sha256:") || value.size() != 71) {
        return false;
    }
    return std::ranges::all_of(value.substr(7), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

std::string build_target_image(std::string_view target, std::string_view space) {
    const std::size_t authority = target.find('/');
    const std::size_t tag = target.rfind(':');
    if (authority == std::string_view::npos || tag == std::string_view::npos ||
        tag <= authority + 1) {
        return {};
    }
    const std::string_view repository = target.substr(authority + 1, tag - authority - 1);
    if (!repository.starts_with(space) || repository.size() <= space.size() + 1 ||
        repository[space.size()] != '/') {
        return {};
    }
    const std::string_view image = repository.substr(space.size() + 1);
    return image.contains('/') ? std::string() : std::string(image);
}

namespace {

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_object = "customer/" + customer_id + "/images";
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

json route_json(const control::OciRepositoryRoute& route, const std::string& space) {
    return {{"customer_id", route.customer_id},
            {"workspace_id", route.workspace_id},
            {"name", route.name},
            {"repository", space + "/" + route.name},
            {"created_at", route.created_at}};
}

json tag_json(const control::OciTag& tag) {
    return {{"tag", tag.tag},
            {"digest", tag.digest},
            {"content_type", tag.content_type},
            {"updated_at", tag.updated_at}};
}

} // namespace

std::optional<TenantRepository> tenant_repository(std::string_view repository) {
    const std::size_t slash = repository.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= repository.size()) {
        return std::nullopt;
    }
    return TenantRepository{std::string(repository.substr(0, slash)),
                            std::string(repository.substr(slash + 1))};
}

std::optional<control::Customer> space_customer(control::Store& store, std::string_view space) {
    const auto customers = store.list_customers(10000);
    if (!customers) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(*customers, space, &control::Customer::name);
    if (found == customers->end()) {
        return std::nullopt;
    }
    return *found;
}

std::expected<std::vector<std::string>, std::string>
manifest_digests(const json& manifest) {
    std::vector<std::string> digests;
    if (manifest.contains("config") && manifest.at("config").is_object()) {
        digests.push_back(manifest.at("config").value("digest", ""));
    }
    if (!manifest.contains("layers") || !manifest.at("layers").is_array()) {
        return std::unexpected("OCI manifest has no layer array");
    }
    for (const json& layer : manifest.at("layers")) {
        digests.push_back(layer.is_object() ? layer.value("digest", "") : std::string());
    }
    if (!std::ranges::all_of(digests, [](const std::string& digest) { return sha256_digest(digest); })) {
        return std::unexpected("OCI manifest contains an invalid digest");
    }
    return digests;
}

ImageOwner legacy_image_owner(control::Store& store, const control::Customer& customer,
                              const std::string& name, bool write) {
    if (!control::valid_oci_route_name(name)) {
        return {{}, "invalid_name", false};
    }
    if (const auto route = store.get_oci_repository_route(customer.id, name)) {
        return {route->workspace_id, "route_exists", true};
    }
    std::set<std::string> live;
    if (const auto workspaces = store.list_workspaces(customer.id)) {
        for (const control::Workspace& workspace : *workspaces) {
            if (workspace.desired != "deleted") {
                live.insert(workspace.id);
            }
        }
    }
    const std::string image = name.starts_with("cache/") ? name.substr(6) : name;
    std::set<std::string> users;
    if (const auto apps = store.list_web_apps(customer.id)) {
        for (const control::WebApp& app : *apps) {
            if (app.desired != "deleted" && app.name == image && live.contains(app.workspace_id)) {
                users.insert(app.workspace_id);
            }
        }
    }
    if (const auto builds = store.list_builds(customer.id)) {
        for (const control::Build& build : *builds) {
            if (build_target_image(build.target, customer.name) == image &&
                live.contains(build.workspace_id)) {
                users.insert(build.workspace_id);
            }
        }
    }
    ImageOwner owner;
    if (users.size() == 1) {
        owner = {*users.begin(), "image_workspace", false};
    } else if (users.empty() && live.size() == 1) {
        owner = {*live.begin(), "sole_workspace", false};
    } else {
        return {{}, "needs_selection", false};
    }
    if (!write) {
        return owner;
    }
    const auto created = store.create_oci_repository_route(customer.id, name, owner.workspace_id);
    if (!created) {
        // A concurrent run or push routed it first: its route decides.
        if (const auto route = store.get_oci_repository_route(customer.id, name)) {
            return {route->workspace_id, "route_exists", true};
        }
        return {{}, "needs_selection", false};
    }
    owner.routed = true;
    return owner;
}

liquid::runtime::task<ImageBytes> tenant_image_bytes(ApiContext& context,
                                                     std::string customer_id) {
    ImageBytes result;
    const auto routes = context.store->list_oci_repository_routes(customer_id, {});
    if (!routes) {
        result.unmeasured.push_back("Container images: " + routes.error().message);
        co_return result;
    }
    std::map<std::string, std::uint64_t> sizes;
    std::set<std::string> failed;
    // Blobs whose bytes are objects of a workspace's liquid store (lake#383 phase B), per
    // workspace: that store's usage already counts them, so they are not counted again here. A
    // digest one workspace stores and another still reads from the instance registry is in both.
    std::map<std::string, std::set<std::string>> stored_in;
    std::map<std::string, std::uint64_t> stored_sizes;
    for (const control::OciRepositoryRoute& route : *routes) {
        if (failed.contains(route.workspace_id)) {
            continue;
        }
        auto records = workspace_records(context, customer_id, route.workspace_id);
        if (!records) {
            failed.insert(route.workspace_id);
            result.unmeasured.push_back("Container images in workspace " + route.workspace_id +
                                        ": " + records.error().message);
            continue;
        }
        if (!stored_in.contains(route.workspace_id)) {
            std::set<std::string>& stored = stored_in[route.workspace_id];
            auto blobs = control::settled(co_await control::list_oci_blobs(*records, customer_id));
            if (!blobs) {
                failed.insert(route.workspace_id);
                result.unmeasured.push_back("Container images in workspace " + route.workspace_id +
                                            ": " + blobs.error().message);
                continue;
            }
            for (const control::OciBlob& blob : *blobs) {
                if (!blob.object.empty()) {
                    stored.insert(blob.digest);
                }
            }
        }
        auto tags = control::settled(
            co_await control::list_oci_tags(*records, customer_id, route.name));
        if (!tags) {
            failed.insert(route.workspace_id);
            result.unmeasured.push_back("Container images in workspace " + route.workspace_id +
                                        ": " + tags.error().message);
            continue;
        }
        const std::set<std::string>& stored = stored_in[route.workspace_id];
        for (const control::OciTag& tag : *tags) {
            sizes[tag.digest] = tag.manifest_size;
            for (const control::OciReference& reference : tag.references) {
                (stored.contains(reference.digest) ? stored_sizes : sizes)[reference.digest] =
                    reference.size;
            }
        }
    }
    for (const auto& [digest, size] : sizes) {
        result.bytes += size;
    }
    for (const auto& [digest, size] : stored_sizes) {
        result.stored_bytes += size;
    }
    co_return result;
}

void register_image_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_image_repository",
        .title = "Create image repository",
        .description =
            "Route a tenant image repository `<space>/<name>` (or `<space>/cache/<name>`) to one "
            "workspace, whose Zetta then holds its manifests and tags. `workspace_id` is required "
            "and there is no default; a build's first push routes its target from the build's "
            "workspace. Repeating it for the same workspace answers the route; a name another "
            "workspace holds is a conflict, and a route never moves.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id", "workspace_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string customer_id = input.at("customer_id");
            const std::string workspace_id = input.at("workspace_id");
            const std::string name = input.at("name");
            const auto customer = context.store->get_customer(customer_id);
            if (!customer) {
                return ApiResult::failure("invalid_input", "customer_id is unknown", "/customer_id");
            }
            if (!control::valid_oci_route_name(name)) {
                return ApiResult::failure(
                    "invalid_input",
                    "name must be `<image>` or `cache/<image>`: lowercase letters, digits, `.`, "
                    "`_` and `-`",
                    "/name");
            }
            const auto route =
                context.store->create_oci_repository_route(customer_id, name, workspace_id);
            if (!route) {
                const bool name_fault = route.error().message.starts_with("name");
                return ApiResult::failure(route.error().code, route.error().message,
                                          route.error().code == "invalid_input"
                                              ? (name_fault ? "/name" : "/workspace_id")
                                              : std::string());
            }
            context.audit_kind = "image.route.create";
            context.audit_object = "image/" + customer->name + "/" + name;
            context.audit_details = {{"customer_id", customer_id},
                                     {"workspace_id", workspace_id},
                                     {"name", name}};
            return ApiResult::success(route_json(*route, customer->name));
        }});
    registry.add(Capability{
        .name = "unroute_image_repository",
        .title = "Unroute image repository",
        .description =
            "Operator only. Removes the route of a tenant image repository that went to the wrong "
            "workspace, for example an app recreated in another workspace; removing `<image>` also "
            "removes `cache/<image>`. With `workspace_id` it moves them there instead, in the same "
            "transaction, so no push in between can claim the name. The old workspace's manifest "
            "and tag records stay where they are (they go with that workspace); after a removal a "
            "later create_image_repository, build push or copy may route the name again.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "image route",
        .action = "admin",
        .resolve_object = [](const json&, ApiContext&) -> std::expected<std::string, ApiError> {
            return std::unexpected(ApiError{"unauthorized", "operator credential is required", {}});
        },
        .handler = [](const json& input, ApiContext& context) {
            if (!context.operator_credential) {
                return ApiResult::failure("unauthorized", "operator credential is required");
            }
            const std::string customer_id = input.at("customer_id");
            const std::string name = input.at("name");
            const std::string to = input.value("workspace_id", std::string());
            const auto customer = context.store->get_customer(customer_id);
            if (!customer) {
                return ApiResult::failure("invalid_input", "customer_id is unknown", "/customer_id");
            }
            if (input.contains("workspace_id") && to.empty()) {
                return ApiResult::failure("invalid_input",
                                          "workspace_id must name a live workspace of this space",
                                          "/workspace_id");
            }
            const auto removed = context.store->delete_oci_repository_route(customer_id, name, to);
            if (!removed) {
                const std::string& code = removed.error().code;
                return ApiResult::failure(code, removed.error().message,
                                          code == "not_found"       ? "/name"
                                          : code == "invalid_input" ? "/workspace_id"
                                                                    : std::string());
            }
            json routes = json::array();
            json names = json::array();
            for (const control::OciRepositoryRoute& route : *removed) {
                json item = route_json(route, customer->name);
                if (!to.empty()) {
                    item["moved_to"] = to;
                }
                routes.push_back(std::move(item));
                names.push_back(route.name);
            }
            context.audit_customer_id = customer_id;
            context.audit_kind = to.empty() ? "image.route.remove" : "image.route.move";
            context.audit_object = "image/" + customer->name + "/" + name;
            context.audit_details = {{"customer_id", customer_id},
                                     {"from_workspace_id", removed->front().workspace_id},
                                     {"names", names}};
            if (!to.empty()) {
                context.audit_details["to_workspace_id"] = to;
            }
            return ApiResult::success({{"routes", std::move(routes)}});
        }});
    registry.add(Capability{
        .name = "list_image_repositories",
        .title = "List image repositories",
        .description =
            "A space's tenant image repositories with the workspace each belongs to and its tags, "
            "read from that workspace. `workspace_id` narrows it to one workspace. A repository "
            "whose workspace cannot be read is listed with `tags_unavailable`.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            const auto object = customer_object(input, context);
            if (!object) {
                co_return ApiResult::failure(object.error().code, object.error().message,
                                             object.error().path);
            }
            if (context.acting_identity != "admin") {
                const auto decision = control::authorize(*context.store, context.acting_identity,
                                                         *object, "read",
                                                         context.authorization_cache);
                if (!decision) {
                    co_return ApiResult::failure(decision.error().code, decision.error().message);
                }
                if (!decision->allowed) {
                    co_return ApiResult::failure(
                        "unauthorized", "principal is not allowed to read this space's images");
                }
            }
            const std::string customer_id = input.at("customer_id");
            const auto customer = context.store->get_customer(customer_id);
            if (!customer) {
                co_return ApiResult::failure("invalid_input", "customer_id is unknown",
                                             "/customer_id");
            }
            const std::string workspace_id = input.value("workspace_id", std::string());
            if (input.contains("workspace_id")) {
                const auto workspace = context.store->get_workspace(workspace_id);
                if (!workspace || workspace->customer_id != customer_id) {
                    co_return ApiResult::failure(
                        "invalid_input", "workspace_id must name a workspace of this space",
                        "/workspace_id");
                }
            }
            const auto routes = context.store->list_oci_repository_routes(customer_id, workspace_id);
            if (!routes) {
                co_return ApiResult::failure(routes.error().code, routes.error().message);
            }
            json items = json::array();
            for (const control::OciRepositoryRoute& route : *routes) {
                json item = route_json(route, customer->name);
                item["tags"] = json::array();
                auto records = workspace_records(context, customer_id, route.workspace_id);
                if (!records) {
                    item["tags_unavailable"] = records.error().message;
                    items.push_back(std::move(item));
                    continue;
                }
                auto tags = control::settled(
                    co_await control::list_oci_tags(*records, customer_id, route.name));
                if (!tags) {
                    item["tags_unavailable"] = tags.error().message;
                } else {
                    for (const control::OciTag& tag : *tags) {
                        item["tags"].push_back(tag_json(tag));
                    }
                }
                items.push_back(std::move(item));
            }
            context.audit_kind = "image.route.list";
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
}

} // namespace lake::api
