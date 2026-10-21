#include "lake/api/capabilities.hpp"

#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lake/control/identity_crypto.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "secret_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable",
                              "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

// A mount's policy object, which counts only inside the account that owns the mount: under its
// customer object, directly or through the owning organization. A mount made under an
// environment another account created first to claim this account's identifiers sits outside
// it (lake#913) and is refused.
control::Result<control::PolicyObject> owned_mount_object(control::Store& store,
                                                          const control::SecretMountRoute& route) {
    auto object = store.find_policy_object("secret_mount", route.mount_id);
    if (!object) {
        return object;
    }
    const auto customer = store.find_policy_object("customer", route.customer_id);
    if (!customer) {
        return std::unexpected(customer.error());
    }
    const auto within = control::object_within(store, object->id, customer->id);
    if (!within) {
        return std::unexpected(within.error());
    }
    if (!*within) {
        return std::unexpected(control::StoreError{
            "conflict",
            "secret mount " + route.mount_id + " sits under another account's policy object"});
    }
    return object;
}

json object_schema() {
    return {{"type", "object"}};
}

json items_schema() {
    return {{"type", "object"}, {"properties", {{"items", {{"type", "array"}}}}}};
}

json input_schema(json properties, json required = json::array()) {
    json schema = {
        {"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

// The organization a new mount names, or empty when its account (the customer) owns it.
std::string mount_organization(const json& input) {
    const auto found = input.find("organization_id");
    return found != input.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// The policy object that decides who may create a mount: its owner's. That is the organization
// named by `organization_id` (`admin` on the organization, as before), or, without one, the
// customer that owns the workspace (`admin` on the customer, the same object an account's
// environments hang under).
std::expected<std::string, ApiError> mount_owner_object(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string organization_id = mount_organization(input);
    context.audit_customer_id = customer_id;
    context.audit_kind = "secret_mount";
    context.audit_object = "secret_mount/" + customer_id + "/" +
                           (organization_id.empty() ? "" : organization_id + "/") +
                           input.at("name").get<std::string>();
    context.audit_details = {{"mount", input.at("name")},
                             {"workspace_id", input.value("workspace_id", "")},
                             {"owner_type", organization_id.empty() ? "customer" : "organization"}};
    const bool account = organization_id.empty();
    auto object = context.store->find_policy_object(account ? "customer" : "organization",
                                                    account ? customer_id : organization_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code,
                                        object.error().message,
                                        account ? "/customer_id" : "/organization_id"});
    }
    return object->id;
}

// The mount's route (control store) names its space for the audit and its policy object for the
// decision; the workspace is not reached before the caller is authorized.
bool uuid_shaped(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (dash ? c != '-' : std::isxdigit(c) == 0) {
            return false;
        }
    }
    return true;
}

std::expected<std::string, ApiError> resolve_mount(const std::string& mount_id,
                                                   const std::string& audit_kind,
                                                   ApiContext& context) {
    if (!uuid_shaped(mount_id)) {
        return std::unexpected(ApiError{"not_found", "no secret mount " + mount_id, "/mount_id"});
    }
    auto route = context.store->get_secret_mount_route(mount_id);
    if (!route) {
        return std::unexpected(ApiError{route.error().code, route.error().message, "/mount_id"});
    }
    context.audit_customer_id = route->customer_id;
    context.audit_kind = audit_kind;
    context.audit_object = "secret_mount/" + mount_id;
    context.audit_details = {{"mount_id", mount_id}, {"workspace_id", route->workspace_id}};
    auto object = owned_mount_object(*context.store, *route);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/mount_id"});
    }
    return object->id;
}

CapabilityObjectResolver mount_object(std::string audit_kind) {
    return [audit_kind = std::move(audit_kind)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        auto object = resolve_mount(input.at("mount_id"), audit_kind, context);
        if (object && input.contains("name")) {
            context.audit_details["key"] = input.at("name");
        }
        if (object && input.contains("version")) {
            context.audit_details["version"] = input.at("version");
        }
        return object;
    };
}

// A lease's route (control store) names its mount, whose route names the policy object; the
// lease's workspace is not reached before the caller is authorized.
std::expected<std::string, ApiError> dynamic_lease_object(const json& input, ApiContext& context) {
    const std::string lease_id = input.at("lease_id");
    if (!uuid_shaped(lease_id)) {
        return std::unexpected(ApiError{
            "not_found", "dynamic-secret lease " + lease_id + " does not exist", "/lease_id"});
    }
    auto lease = context.store->get_secret_lease_route(lease_id);
    if (!lease) {
        return std::unexpected(ApiError{lease.error().code, lease.error().message, "/lease_id"});
    }
    auto object = resolve_mount(lease->mount_id, "dynamic_secret", context);
    context.audit_object = "dynamic_secret/" + lease->lease_id;
    context.audit_details["lease_id"] = lease->lease_id;
    return object;
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_kind = "encryption";
    context.audit_object = "customer/" + customer_id;
    context.audit_details = {{"customer_id", customer_id}};
    auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> candidate_list_object(const json&, ApiContext&) {
    return std::string{};
}

int limit_of(const json& input) {
    const auto limit = input.value("limit", 100);
    return limit > 0 ? limit : 100;
}

std::expected<std::uint64_t, ApiError> optional_version(const json& input,
                                                        const std::string& field = "version") {
    if (!input.contains(field)) {
        return 0;
    }
    const auto value = input.at(field).get<std::int64_t>();
    if (value < 0) {
        return std::unexpected(
            ApiError{"invalid_input", field + " must not be negative", "/" + field});
    }
    return static_cast<std::uint64_t>(value);
}

std::expected<std::vector<std::uint64_t>, ApiError> versions_of(const json& input) {
    std::vector<std::uint64_t> versions;
    for (std::size_t index = 0; index < input.at("versions").size(); ++index) {
        const json& value = input.at("versions").at(index);
        if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
            return std::unexpected(ApiError{"invalid_input",
                                            "versions must contain positive integers",
                                            "/versions/" + std::to_string(index)});
        }
        versions.push_back(value.get<std::uint64_t>());
    }
    if (versions.empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "at least one version is required", "/versions"});
    }
    return versions;
}

ApiResult refused(const ApiError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

template <class T> ApiResult one_result(const control::ZettaControlResult<T>& row) {
    if (!row) {
        return secret_records_failure(row.error());
    }
    return ApiResult::success(control::to_json(*row));
}

using SecretHandler = std::function<liquid::runtime::task<ApiResult>(const json&, ApiContext&,
                                                                     MountSecrets&)>;

// A list on one mount authorizes that mount itself (every list is candidate-authorizing): the
// mount's route and policy object, read from PostgreSQL, decide before its workspace is reached.
// Answers the refusal, or nullopt when the caller may read the mount.
std::optional<ApiResult>
refuse_unreadable_mount(const json& input, const std::string& audit_kind, ApiContext& context) {
    const auto object = resolve_mount(input.at("mount_id"), audit_kind, context);
    if (!object) {
        return refused(object.error());
    }
    if (context.acting_identity != "admin") {
        const auto decisions = control::authorize_batch(*context.store,
                                                        context.acting_identity,
                                                        {*object},
                                                        "read",
                                                        context.authorization_cache);
        if (!decisions) {
            return from_store_error(decisions.error());
        }
        if (!decisions->at(*object).allowed) {
            return ApiResult::failure("unauthorized",
                                      "principal is not allowed to read secret_mount");
        }
    }
    return std::nullopt;
}

// A capability on one mount: the mount's records are opened in the workspace its route names.
AsyncCapabilityHandler on_mount(SecretHandler handler) {
    return [handler = std::move(handler)](const json& input,
                                          ApiContext& context) -> liquid::runtime::task<ApiResult> {
        if (!context.store) {
            co_return no_store();
        }
        auto mount = MountSecrets::open(context, input.at("mount_id"));
        if (!mount) {
            co_return refused(mount.error());
        }
        auto completed = co_await handler(input, context, *mount);
        co_return completed ? std::move(*completed)
                            : ApiResult::failure("store_error", completed.error().message);
    };
}

// A capability on one dynamic lease: the records of the workspace its route names.
AsyncCapabilityHandler on_lease(SecretHandler handler) {
    return [handler = std::move(handler)](const json& input,
                                          ApiContext& context) -> liquid::runtime::task<ApiResult> {
        if (!context.store) {
            co_return no_store();
        }
        auto mount = MountSecrets::open_lease(context, input.at("lease_id"));
        if (!mount) {
            co_return refused(mount.error());
        }
        auto completed = co_await handler(input, context, *mount);
        co_return completed ? std::move(*completed)
                            : ApiResult::failure("store_error", completed.error().message);
    };
}

json mount_input(json properties, json required) {
    properties["mount_id"] = {{"type", "string"}};
    required.insert(required.begin(), "mount_id");
    return input_schema(std::move(properties), std::move(required));
}

void register_mount_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_secret_mount",
        .title = "Create secret mount",
        .description = "Create a secret mount in one workspace of a customer. Without "
                       "organization_id the customer (the account that owns the workspace) owns "
                       "it; with one, that organization does. The mount, its secrets and its keys "
                       "live in that workspace's Zetta.",
        .input_schema = input_schema({{"customer_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"organization_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"description", {{"type", "string"}}}},
                                     json::array({"customer_id", "workspace_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret mount owner",
        .action = "admin",
        .resolve_object = mount_owner_object,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            control::SecretMount draft;
            draft.customer_id = input.at("customer_id");
            draft.workspace_id = input.at("workspace_id");
            draft.organization_id = mount_organization(input);
            draft.name = input.at("name");
            draft.description = input.value("description", "");
            auto created = control::settled(co_await create_secret_mount(context, draft));
            if (created) {
                context.audit_object = "secret_mount/" + created->id;
                context.audit_details["mount_id"] = created->id;
            }
            co_return one_result(created);
        }});
    registry.add(Capability{
        .name = "list_secret_mounts",
        .title = "List secret mounts",
        .description = "List the secret mounts you may read in one workspace, to find where a "
                       "secret lives.",
        .input_schema = input_schema({{"customer_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"organization_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}},
                                     json::array({"customer_id", "workspace_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = candidate_list_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const std::string customer_id = input.at("customer_id");
            const std::string workspace_id = input.at("workspace_id");
            context.audit_customer_id = customer_id;
            context.audit_kind = "secret_mount";
            context.audit_object = "secret_mounts";
            context.audit_details = {{"workspace_id", workspace_id}};
            // The routes are read and authorized first, so a caller who may read no mount here
            // causes no workspace call at all.
            auto routes = context.store->list_secret_mount_routes(customer_id, workspace_id);
            if (!routes) {
                co_return from_store_error(routes.error());
            }
            std::vector<std::string> object_ids;
            std::map<std::string, std::string> object_of;
            for (const auto& route : *routes) {
                auto object = owned_mount_object(*context.store, route);
                if (!object && object.error().code == "conflict") {
                    // Governed from another account's part of the graph (lake#913): listed only
                    // to the operator, and refused by every mount capability.
                    continue;
                }
                if (!object) {
                    co_return from_store_error(object.error());
                }
                object_ids.push_back(object->id);
                object_of[route.mount_id] = object->id;
            }
            std::set<std::string> readable;
            if (context.acting_identity == "admin") {
                for (const auto& route : *routes) {
                    readable.insert(route.mount_id);
                }
            } else if (!object_ids.empty()) {
                auto authorized = control::authorize_batch(*context.store,
                                                           context.acting_identity,
                                                           object_ids,
                                                           "read",
                                                           context.authorization_cache);
                if (!authorized) {
                    co_return from_store_error(authorized.error());
                }
                for (const auto& [mount_id, object_id] : object_of) {
                    if (authorized->at(object_id).allowed) {
                        readable.insert(mount_id);
                    }
                }
            }
            json items = json::array();
            if (readable.empty()) {
                co_return ApiResult::success({{"items", std::move(items)}});
            }
            auto mounts = control::settled(
                co_await list_secret_mounts(context, customer_id, workspace_id,
                                            input.value("organization_id", ""), 0));
            if (!mounts) {
                co_return secret_records_failure(mounts.error());
            }
            const auto limit = static_cast<std::size_t>(limit_of(input));
            for (const auto& mount : *mounts) {
                if (readable.contains(mount.id) && items.size() < limit) {
                    items.push_back(control::to_json(mount));
                }
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "get_secret_mount",
        .title = "Get secret mount",
        .description = "Read one secret mount's settings, never the secrets stored in it.",
        .input_schema = mount_input(json::object(), json::array()),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("secret_mount"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json&, ApiContext&,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            co_return one_result(control::settled(co_await mount.mount()));
        })});
    // lake#645, docs/api/secrets.md#deleting-a-mount.
    registry.add(Capability{
        .name = "delete_secret_mount",
        .title = "Delete a secret mount",
        .description =
            "Delete a secret mount and everything in it, which cannot be recovered: every secret "
            "with all its versions, its named keys and their data keys, its dynamic leases, and "
            "its access grants. They are removed from the mount's workspace Zetta. `confirm` must "
            "be the mount's name. `dry_run: true` deletes nothing and lists what would go, with "
            "counts and the secrets' names.",
        .input_schema = mount_input(
            {{"confirm", {{"type", "string"}}}, {"dry_run", {{"type", "boolean"}}}}, json::array()),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "admin",
        .resolve_object = mount_object("secret_mount"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context, MountSecrets& mount)
                                      -> liquid::runtime::task<ApiResult> {
            const bool dry_run = input.value("dry_run", false);
            // The route goes with the mount, so the event is routed to its workspace here.
            context.audit_workspace_id = mount.route().workspace_id;
            context.audit_kind = dry_run ? "secret_mount.delete.plan" : "secret_mount.delete";
            auto settings = control::settled(co_await mount.mount());
            const std::string name = settings ? settings->name : std::string();
            if (!settings && settings.error().code != "not_found") {
                co_return secret_records_failure(settings.error());
            }
            // A mount whose record is already gone (a deletion stopped part way) is confirmed by
            // its id.
            const std::string expected = name.empty() ? mount.route().mount_id : name;
            const bool confirmed = input.contains("confirm") && input.at("confirm") == expected;
            if (!confirmed && (!dry_run || input.contains("confirm"))) {
                co_return ApiResult::failure(
                    "invalid_input", "confirm must match the mount's name", "/confirm");
            }
            auto removed = control::settled(co_await mount.remove_all(dry_run));
            if (!removed) {
                co_return secret_records_failure(removed.error());
            }
            if (!dry_run) {
                if (auto route = context.store->delete_secret_mount_route(mount.route().mount_id);
                    !route) {
                    co_return from_store_error(route.error());
                }
            }
            json value{{"mount_id", mount.route().mount_id},
                       {"name", name},
                       {"workspace_id", mount.route().workspace_id},
                       {"dry_run", dry_run},
                       {"deleted", !dry_run},
                       {"counts", removed->at("counts")},
                       {"secrets", removed->at("secrets")}};
            context.audit_details["counts"] = value["counts"];
            co_return ApiResult::success(std::move(value));
        })});
}

void register_static_secret_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "put_secret",
        .title = "Put secret",
        .description = "Create a new immutable version of a key-value secret. Answers the new "
                       "version's metadata (version, times, custom_metadata), never the value: "
                       "read_secret is the one call that returns a value.",
        .input_schema = mount_input({{"name", {{"type", "string"}}},
                                     {"value", {{"type", "object"}}},
                                     {"custom_metadata", {{"type", "object"}}},
                                     {"check_and_set", {{"type", "integer"}}}},
                                    json::array({"name", "value"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("secret"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context, MountSecrets& mount)
                                      -> liquid::runtime::task<ApiResult> {
            auto check = optional_version(input, "check_and_set");
            if (!check) {
                co_return refused(check.error());
            }
            auto result = control::settled(
                co_await mount.put({input.at("mount_id"), input.at("name"), input.at("value"),
                                    input.value("custom_metadata", json::object()), *check}));
            if (!result) {
                co_return secret_records_failure(result.error());
            }
            context.audit_details["version"] = result->version;
            // Never the value (lake#590): a tool's answer lands in agent transcripts and logs.
            json written = control::to_json(*result);
            written.erase("value");
            co_return ApiResult::success(std::move(written));
        })});
    registry.add(Capability{
        .name = "read_secret",
        .title = "Read secret",
        .description = "Read the current or an explicit live secret version.",
        .input_schema =
            mount_input({{"name", {{"type", "string"}}}, {"version", {{"type", "integer"}}}},
                        json::array({"name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("secret"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto version = optional_version(input);
            if (!version) {
                co_return refused(version.error());
            }
            auto result =
                control::settled(co_await mount.read(input.at("name"), *version));
            if (result) {
                context.audit_details["version"] = result->version;
            }
            co_return one_result(result);
        })});
    registry.add(Capability{
        .name = "secret_metadata",
        .title = "Read secret metadata",
        .description = "Read custom metadata and the complete version history without values.",
        .input_schema = mount_input({{"name", {{"type", "string"}}}}, json::array({"name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("secret"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto result = control::settled(co_await mount.metadata(input.at("name")));
            if (result) {
                context.audit_details["version"] = result->current_version;
            }
            co_return one_result(result);
        })});
    registry.add(Capability{
        .name = "list_secrets",
        .title = "List secrets",
        .description = "List a mount's secrets by name, with each one's current version, times and "
                       "deleted state. Values are never returned.",
        .input_schema =
            mount_input({{"prefix", {{"type", "string"}}},
                         {"cursor", {{"type", "string"}}},
                         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}}},
                        json::array()),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"items", {{"type", "array"}}},
                            {"next_cursor", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("secret"),
        // Like every list, it authorizes what it returns itself: here the one mount, before the
        // mount's workspace is reached.
        .authorizes_candidates = true,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (auto refusal = refuse_unreadable_mount(input, "secret", context)) {
                co_return std::move(*refusal);
            }
            auto mount = MountSecrets::open(context, input.at("mount_id"));
            if (!mount) {
                co_return refused(mount.error());
            }
            const std::string prefix = input.value("prefix", "");
            if (!prefix.empty()) {
                context.audit_details["prefix"] = prefix;
            }
            auto page = control::settled(co_await mount->list(
                prefix, input.value("cursor", ""), static_cast<std::size_t>(limit_of(input))));
            if (!page) {
                co_return secret_records_failure(page.error());
            }
            json items = json::array();
            for (const auto& item : page->items) {
                items.push_back(
                    {{"name", item.name},
                     {"current_version", item.current_version},
                     {"created_at", item.created_at},
                     {"updated_at", item.updated_at},
                     {"deleted", !item.deleted_at.empty()},
                     {"deleted_at",
                      item.deleted_at.empty() ? json(nullptr) : json(item.deleted_at)},
                     {"destroyed_at",
                      item.destroyed_at.empty() ? json(nullptr) : json(item.destroyed_at)}});
            }
            json value = {{"items", std::move(items)}};
            if (!page->next_cursor.empty()) {
                value["next_cursor"] = page->next_cursor;
            }
            co_return ApiResult::success(std::move(value));
        }});
    const auto add_version_action = [&registry](std::string name,
                                                std::string title,
                                                std::string description,
                                                control::SecretVersionAction action) {
        registry.add(Capability{
            .name = std::move(name),
            .title = std::move(title),
            .description = std::move(description),
            .input_schema =
                mount_input({{"name", {{"type", "string"}}}, {"versions", {{"type", "array"}}}},
                            json::array({"name", "versions"})),
            .output_schema = object_schema(),
            .kind = CapabilityKind::Action,
            .object = "secret_mount",
            .action = "write",
            .resolve_object = mount_object("secret"),
            .resolves_operator_audit_target = true,
            .async_handler =
                on_mount([action](const json& input, ApiContext& context,
                                  MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
                    auto versions = versions_of(input);
                    if (!versions) {
                        co_return refused(versions.error());
                    }
                    context.audit_details["versions"] = *versions;
                    co_return one_result(control::settled(
                        co_await mount.change(input.at("name"), *versions, action)));
                })});
    };
    add_version_action("delete_secret_versions",
                       "Delete secret versions",
                       "Soft-delete secret versions while retaining ciphertext and history.",
                       control::SecretVersionAction::Delete);
    add_version_action("undelete_secret_versions",
                       "Undelete secret versions",
                       "Restore soft-deleted secret versions that were not destroyed.",
                       control::SecretVersionAction::Undelete);
    add_version_action("destroy_secret_versions",
                       "Destroy secret versions",
                       "Permanently erase selected version ciphertext while retaining metadata.",
                       control::SecretVersionAction::Destroy);
}

void register_dynamic_secret_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "lease_dynamic_secret",
        .title = "Lease dynamic secret",
        .description = "Generate and lease a short-lived credential from a secret mount.",
        .input_schema = mount_input(
            {{"role", {{"type", "string"}}}, {"lifetime_seconds", {{"type", "integer"}}}},
            json::array({"role"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("dynamic_secret"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto issued = control::settled(co_await mount.lease(
                input.at("role"), input.value("lifetime_seconds", 300)));
            if (!issued) {
                co_return secret_records_failure(issued.error());
            }
            context.audit_object = "dynamic_secret/" + issued->lease.id;
            context.audit_details["lease_id"] = issued->lease.id;
            context.audit_details["operation"] = "lease";
            co_return ApiResult::success(
                {{"lease", control::to_json(issued->lease)}, {"value", issued->value}});
        })});
    registry.add(Capability{
        .name = "renew_dynamic_secret",
        .title = "Renew dynamic secret",
        .description =
            "Keep a generated credential working longer by renewing its lease before it expires.",
        .input_schema = input_schema(
            {{"lease_id", {{"type", "string"}}}, {"lifetime_seconds", {{"type", "integer"}}}},
            json::array({"lease_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = dynamic_lease_object,
        .resolves_operator_audit_target = true,
        .async_handler = on_lease([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            context.audit_details["operation"] = "renew";
            co_return one_result(control::settled(co_await mount.renew_lease(
                input.at("lease_id"), input.value("lifetime_seconds", 300))));
        })});
    registry.add(Capability{
        .name = "revoke_dynamic_secret",
        .title = "Revoke dynamic secret",
        .description =
            "Revoke a generated credential now, so it stops working everywhere it was used.",
        .input_schema =
            input_schema({{"lease_id", {{"type", "string"}}}}, json::array({"lease_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = dynamic_lease_object,
        .resolves_operator_audit_target = true,
        .async_handler = on_lease([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            context.audit_details["operation"] = "revoke";
            co_return one_result(
                control::settled(co_await mount.revoke_lease(input.at("lease_id"))));
        })});
}

void register_encryption_key_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_encryption_key",
        .title = "Create encryption key",
        .description = "Create a named versioned key whose material remains inside Lake.",
        .input_schema = mount_input({{"name", {{"type", "string"}}}}, json::array({"name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context, MountSecrets& mount)
                                      -> liquid::runtime::task<ApiResult> {
            auto created = control::settled(co_await mount.create_key(input.at("name")));
            if (created) {
                context.audit_details["version"] = created->current_version;
            }
            co_return one_result(created);
        })});
    registry.add(Capability{
        .name = "list_encryption_keys",
        .title = "List encryption keys",
        .description = "List named keys in a readable mount without exposing key material.",
        .input_schema = mount_input({{"limit", {{"type", "integer"}}}}, json::array()),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("encryption"),
        // Like every list, it authorizes what it returns itself: here the one mount, before the
        // mount's workspace is reached.
        .authorizes_candidates = true,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (auto refusal = refuse_unreadable_mount(input, "encryption", context)) {
                co_return std::move(*refusal);
            }
            auto mount = MountSecrets::open(context, input.at("mount_id"));
            if (!mount) {
                co_return refused(mount.error());
            }
            auto rows = control::settled(co_await mount->list_keys(limit_of(input)));
            if (!rows) {
                co_return secret_records_failure(rows.error());
            }
            json items = json::array();
            for (const auto& row : *rows) {
                items.push_back(control::to_json(row));
            }
            co_return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "get_encryption_key",
        .title = "Get encryption key",
        .description = "Read named-key metadata without exposing key material.",
        .input_schema = mount_input({{"name", {{"type", "string"}}}}, json::array({"name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto key = control::settled(co_await mount.get_key(input.at("name")));
            if (key) {
                context.audit_details["version"] = key->current_version;
            }
            co_return one_result(key);
        })});
    registry.add(Capability{
        .name = "rotate_encryption_key",
        .title = "Rotate encryption key",
        .description = "Add a new current version while retaining old versions for decryption.",
        .input_schema = mount_input({{"name", {{"type", "string"}}}}, json::array({"name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto result = control::settled(co_await mount.rotate_key(input.at("name")));
            if (result) {
                context.audit_details["version"] = result->current_version;
            }
            co_return one_result(result);
        })});
}

using ValueOperation = std::function<SecretTask<control::TransitValue>(
    MountSecrets&, std::string, std::string)>;

void register_encryption_value_capabilities(Registry& registry) {
    const auto add_value_action = [&registry](std::string name,
                                              std::string title,
                                              std::string description,
                                              std::string input_field,
                                              ValueOperation operation) {
        registry.add(Capability{
            .name = std::move(name),
            .title = std::move(title),
            .description = std::move(description),
            .input_schema = mount_input(
                {{"name", {{"type", "string"}}}, {input_field, {{"type", "string"}}}},
                json::array({"name", input_field})),
            .output_schema = object_schema(),
            .kind = CapabilityKind::Action,
            .object = "secret_mount",
            .action = "write",
            .resolve_object = mount_object("encryption"),
            .resolves_operator_audit_target = true,
            .async_handler = on_mount(
                [input_field, operation](const json& input, ApiContext& context,
                                         MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
                    auto result = control::settled(co_await operation(
                        mount, input.at("name").get<std::string>(),
                        input.at(input_field).get<std::string>()));
                    if (result) {
                        context.audit_details["version"] = result->key_version;
                    }
                    co_return one_result(result);
                })});
    };
    add_value_action("encrypt",
                     "Encrypt",
                     "Encrypt base64url plaintext with the current named-key version.",
                     "plaintext",
                     [](MountSecrets& mount, std::string name, std::string value) {
                         return mount.encrypt(std::move(name), std::move(value));
                     });
    add_value_action("decrypt",
                     "Decrypt",
                     "Decrypt Lake ciphertext with the version embedded in its envelope.",
                     "ciphertext",
                     [](MountSecrets& mount, std::string name, std::string value) {
                         return mount.decrypt(std::move(name), std::move(value));
                     });
    add_value_action("rewrap",
                     "Rewrap",
                     "Decrypt and re-encrypt ciphertext under the current named-key version.",
                     "ciphertext",
                     [](MountSecrets& mount, std::string name, std::string value) {
                         return mount.rewrap(std::move(name), std::move(value));
                     });
}

void register_encryption_integrity_capabilities(Registry& registry) {
    const auto message_schema = mount_input({{"name", {{"type", "string"}}},
                                             {"message", {{"type", "string"}}},
                                             {"version", {{"type", "integer"}}}},
                                            json::array({"name", "message"}));
    registry.add(Capability{
        .name = "sign",
        .title = "Sign",
        .description = "Sign a base64url message with an explicit or current key version.",
        .input_schema = message_schema,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto version = optional_version(input);
            if (!version) {
                co_return refused(version.error());
            }
            auto result = control::settled(
                co_await mount.sign(input.at("name"), input.at("message"), *version));
            if (result) {
                context.audit_details["version"] = result->key_version;
            }
            co_return one_result(result);
        })});
    registry.add(Capability{
        .name = "verify",
        .title = "Verify",
        .description = "Verify an Ed25519 signature without exposing key material.",
        .input_schema = mount_input({{"name", {{"type", "string"}}},
                                     {"message", {{"type", "string"}}},
                                     {"signature", {{"type", "string"}}},
                                     {"version", {{"type", "integer"}}}},
                                    json::array({"name", "message", "signature"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto version = optional_version(input);
            if (!version) {
                co_return refused(version.error());
            }
            auto result = control::settled(co_await mount.verify(
                input.at("name"), input.at("message"), input.at("signature"), *version));
            if (result) {
                context.audit_details["version"] = result->key_version;
            }
            co_return one_result(result);
        })});
    registry.add(Capability{
        .name = "hmac",
        .title = "HMAC",
        .description = "Compute HMAC-SHA-256 with an explicit or current key version.",
        .input_schema = message_schema,
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            auto version = optional_version(input);
            if (!version) {
                co_return refused(version.error());
            }
            auto result = control::settled(
                co_await mount.hmac(input.at("name"), input.at("message"), *version));
            if (result) {
                context.audit_details["version"] = result->key_version;
            }
            co_return one_result(result);
        })});
    registry.add(Capability{
        .name = "derive_encryption_key",
        .title = "Derive encryption key",
        .description = "Derive and store a named child key from a parent and base64url context.",
        .input_schema = mount_input({{"parent_name", {{"type", "string"}}},
                                     {"name", {{"type", "string"}}},
                                     {"context", {{"type", "string"}}}},
                                    json::array({"parent_name", "name", "context"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .async_handler = on_mount([](const json& input, ApiContext& context,
                                     MountSecrets& mount) -> liquid::runtime::task<ApiResult> {
            context.audit_details["parent_key"] = input.at("parent_name");
            auto result = control::settled(co_await mount.derive(
                input.at("parent_name"), input.at("name"), input.at("context")));
            if (result) {
                context.audit_details["version"] = result->parent_version;
            }
            co_return one_result(result);
        })});
}

void register_customer_kek_capability(Registry& registry) {
    registry.add(Capability{
        .name = "rotate_customer_kek",
        .title = "Rotate customer KEK",
        .description = "Rotate a customer's KEK and rewrap each data key, in every workspace that "
                       "holds one, without touching payloads.",
        .input_schema =
            input_schema({{"customer_id", {{"type", "string"}}}}, json::array({"customer_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            auto result =
                control::settled(co_await rotate_customer_kek(context, input.at("customer_id")));
            if (result) {
                context.audit_details["version"] = result->version;
                context.audit_details["data_keys_rewrapped"] = result->data_keys_rewrapped;
            }
            co_return one_result(result);
        }});
}

} // namespace

void register_secret_capabilities(Registry& registry) {
    register_mount_capabilities(registry);
    register_static_secret_capabilities(registry);
    register_dynamic_secret_capabilities(registry);
    register_encryption_key_capabilities(registry);
    register_encryption_value_capabilities(registry);
    register_encryption_integrity_capabilities(registry);
    register_customer_kek_capability(registry);
}

} // namespace lake::api
