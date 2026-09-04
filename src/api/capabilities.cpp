#include "lake/api/capabilities.hpp"

#include <chrono>
#include <format>

#include "lake/api/sync_store.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/store.hpp"
#include "lake/policy/authorization.hpp"

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

json id_input() {
    return {{"type", "object"},
            {"properties", {{"id", {{"type", "string"}, {"description", "uuid"}}}}},
            {"required", json::array({"id"})},
            {"additionalProperties", false}};
}
json list_input() {
    return {{"type", "object"},
            {"properties",
             {{"limit",
               {{"type", "integer"}, {"description", "max rows, default 100, newest first"}}}}},
            {"additionalProperties", false}};
}
json user_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"taap_id", {{"type", json::array({"string", "null"})}}},
              {"email", {{"type", json::array({"string", "null"})}}},
              {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json organization_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"taap_id", {{"type", json::array({"string", "null"})}}},
              {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json customer_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"customer_type",
               {{"type", "string"}, {"enum", json::array({"individual", "organization"})}}},
              {"user_id", {{"type", json::array({"string", "null"})}}},
              {"organization_id", {{"type", json::array({"string", "null"})}}},
              {"name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json repository_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"customer_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"description", {{"type", "string"}}},
              {"visibility",
               {{"type", "string"}, {"enum", json::array({"private", "organization", "public"})}}},
              {"deleted_at", {{"type", json::array({"string", "null"})}}},
              {"retention_until", {{"type", json::array({"string", "null"})}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json audit_event_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"repository_id", {{"type", "string"}}},
              {"actor", {{"type", "string"}}},
              {"action", {{"type", "string"}}},
              {"outcome", {{"type", "string"}}},
              {"detail", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}}}}};
}
json repository_ref_schema() {
    return {{"type", "object"},
            {"properties",
             {{"repository_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"head", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json list_schema(json item) {
    return {{"type", "object"},
            {"properties", {{"items", {{"type", "array"}, {"items", std::move(item)}}}}}};
}

int limit_of(const json& input) {
    const int limit = input.value("limit", 100);
    return limit <= 0 ? 100 : (limit > 1000 ? 1000 : limit);
}

template <class T> ApiResult list_result(control::Result<std::vector<T>> rows) {
    if (!rows)
        return from_store_error(rows.error());
    json items = json::array();
    for (const auto& row : *rows)
        items.push_back(control::to_json(row));
    return ApiResult::success({{"items", std::move(items)}});
}

template <class T> ApiResult one_result(control::Result<T> row) {
    if (!row)
        return from_store_error(row.error());
    return ApiResult::success(control::to_json(*row));
}

std::string timestamp_after(std::chrono::seconds offset) {
    const auto value =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) + offset;
    return std::format("{:%FT%T}Z", value);
}

ApiResult authorize_repository(ApiContext& context,
                               const std::string& repository_id,
                               std::string_view action) {
    if (!context.authorizer || !context.authorizer->authorize(context.acting_identity,
                                                              "repository:" + repository_id,
                                                              action)) {
        return ApiResult::failure(
            "unauthorized", "the principal cannot " + std::string(action) + " this repository");
    }
    return ApiResult::success(nullptr);
}

ApiResult authorize_new_repository(ApiContext& context,
                                   const std::string& customer_id,
                                   const std::string& name) {
    const std::string object = "repository:" + customer_id + "/" + name;
    if (!context.authorizer ||
        !context.authorizer->authorize(context.acting_identity, object, "create")) {
        return ApiResult::failure("unauthorized", "the principal cannot create this repository");
    }
    return ApiResult::success(nullptr);
}

ApiResult from_sync_store_error(const SyncStoreError& error) {
    const std::string code = error.code == "invalid_argument" ? "invalid_input" : error.code;
    return ApiResult::failure(code, error.message);
}

control::Result<control::Repository> repository_for_sync(ApiContext& context,
                                                         const std::string& repository_id,
                                                         std::string_view action) {
    if (!context.store) {
        return std::unexpected(
            control::StoreError{"store_unavailable", "no control-plane store is configured"});
    }
    auto repository = context.store->get_repository(repository_id);
    if (!repository) {
        return std::unexpected(repository.error());
    }
    if (ApiResult allowed = authorize_repository(context, repository_id, action); !allowed.ok) {
        return std::unexpected(control::StoreError{allowed.error.code, allowed.error.message});
    }
    return repository;
}

} // namespace

void register_core_capabilities(Registry& registry, std::string_view version) {
    const std::string version_string(version);
    registry.add(Capability{
        .name = "server_info",
        .title = "Server info",
        .description = "Name, node, version and API level of this lake. No authentication.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"name", {{"type", "string"}}},
                            {"node", {{"type", "string"}}},
                            {"version", {{"type", "string"}}},
                            {"api", {{"type", "string"}}},
                            {"store", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .handler = [version_string](const json&, ApiContext& ctx) {
            return ApiResult::success({{"name", "lake"},
                                       {"node", ctx.node_name},
                                       {"version", version_string},
                                       {"api", "v1"},
                                       {"store", ctx.store ? ctx.store->kind() : "none"}});
        }});

    // ---- users ----
    registry.add(Capability{.name = "list_users",
                            .title = "List users",
                            .description = "The most recent users, newest first.",
                            .input_schema = list_input(),
                            .output_schema = list_schema(user_schema()),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? list_result(ctx.store->list_users(limit_of(in)))
                                                 : no_store();
                            }});
    registry.add(Capability{.name = "get_user",
                            .title = "Get user",
                            .description = "One user by id.",
                            .input_schema = id_input(),
                            .output_schema = user_schema(),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? one_result(ctx.store->get_user(in["id"]))
                                                 : no_store();
                            }});
    registry.add(Capability{
        .name = "create_user",
        .title = "Create user",
        .description = "Create a user (identity, owns nothing). "
                       "taap_id and email are optional and unique.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"taap_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = user_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store)
                return no_store();
            return one_result(ctx.store->create_user(control::NewUser{
                in.value("taap_id", ""), in.value("email", ""), in.value("display_name", "")}));
        }});

    // ---- organizations ----
    registry.add(Capability{.name = "list_organizations",
                            .title = "List organizations",
                            .description = "The most recent organizations, newest first.",
                            .input_schema = list_input(),
                            .output_schema = list_schema(organization_schema()),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? list_result(
                                                       ctx.store->list_organizations(limit_of(in)))
                                                 : no_store();
                            }});
    registry.add(Capability{.name = "get_organization",
                            .title = "Get organization",
                            .description = "One organization by id.",
                            .input_schema = id_input(),
                            .output_schema = organization_schema(),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? one_result(ctx.store->get_organization(in["id"]))
                                                 : no_store();
                            }});
    registry.add(Capability{.name = "create_organization",
                            .title = "Create organization",
                            .description = "Create an organization (identity, owns "
                                           "nothing). taap_id is optional and unique.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"taap_id", {{"type", "string"}}},
                                               {"display_name", {{"type", "string"}}}}},
                                             {"additionalProperties", false}},
                            .output_schema = organization_schema(),
                            .kind = CapabilityKind::Action,
                            .handler = [](const json& in, ApiContext& ctx) {
                                if (!ctx.store)
                                    return no_store();
                                return one_result(
                                    ctx.store->create_organization(control::NewOrganization{
                                        in.value("taap_id", ""), in.value("display_name", "")}));
                            }});

    // ---- customers ----
    registry.add(Capability{.name = "list_customers",
                            .title = "List customers",
                            .description = "The most recent customers (tenants), newest first.",
                            .input_schema = list_input(),
                            .output_schema = list_schema(customer_schema()),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store
                                           ? list_result(ctx.store->list_customers(limit_of(in)))
                                           : no_store();
                            }});
    registry.add(Capability{.name = "get_customer",
                            .title = "Get customer",
                            .description = "One customer by id.",
                            .input_schema = id_input(),
                            .output_schema = customer_schema(),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? one_result(ctx.store->get_customer(in["id"]))
                                                 : no_store();
                            }});
    registry.add(Capability{
        .name = "create_customer",
        .title = "Create customer",
        .description = "Create the tenant for a user (customer_type individual + "
                       "user_id) or an organization "
                       "(customer_type organization + organization_id). name is "
                       "the handle — one global lowercase "
                       "namespace; omitted, a {adjective}-{noun}-{number} handle "
                       "is generated. Conflicts answer 409.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_type",
                {{"type", "string"}, {"enum", json::array({"individual", "organization"})}}},
               {"user_id", {{"type", "string"}}},
               {"organization_id", {{"type", "string"}}},
               {"name", {{"type", "string"}, {"description", "handle; generated when omitted"}}}}},
             {"required", json::array({"customer_type"})},
             {"additionalProperties", false}},
        .output_schema = customer_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store)
                return no_store();
            control::NewCustomer draft{in.value("customer_type", ""),
                                       in.value("user_id", ""),
                                       in.value("organization_id", ""),
                                       in.value("name", "")};
            if (!draft.name.empty())
                return one_result(ctx.store->create_customer(draft));
            // Generated handle: retry on a collision, then give up honestly.
            for (int attempt = 0; attempt < 8; ++attempt) {
                draft.name = control::generate_handle();
                auto created = ctx.store->create_customer(draft);
                if (created || created.error().code != "conflict" ||
                    created.error().message.find("name") == std::string::npos) {
                    return one_result(std::move(created));
                }
            }
            return ApiResult::failure("conflict",
                                      "could not find a free generated handle; pass name");
        }});

    // ---- repositories ----
    registry.add(Capability{.name = "list_repositories",
                            .title = "List repositories",
                            .description =
                                "The most recent repositories, newest first; customer_id "
                                "narrows to one tenant.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"customer_id", {{"type", "string"}}},
                                               {"limit", {{"type", "integer"}}}}},
                                             {"additionalProperties", false}},
                            .output_schema = list_schema(repository_schema()),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? list_result(ctx.store->list_repositories(
                                                       in.value("customer_id", ""), limit_of(in)))
                                                 : no_store();
                            }});
    registry.add(Capability{.name = "get_repository",
                            .title = "Get repository",
                            .description = "One repository by id.",
                            .input_schema = id_input(),
                            .output_schema = repository_schema(),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? one_result(ctx.store->get_repository(in["id"]))
                                                 : no_store();
                            }});
    registry.add(Capability{
        .name = "create_repository",
        .title = "Create repository",
        .description = "Create a repository owned by a customer: name is a slug "
                       "unique within that customer "
                       "(the address is <customer handle>/<name>); visibility "
                       "private (default) or public.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}},
                           {"description", {{"type", "string"}}},
                           {"visibility",
                            {{"type", "string"},
                             {"enum", json::array({"private", "organization", "public"})}}}}},
                         {"required", json::array({"customer_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (ApiResult allowed = authorize_new_repository(
                    context, in.value("customer_id", ""), in.value("name", ""));
                !allowed.ok) {
                return allowed;
            }
            return one_result(
                context.store->create_repository(control::NewRepository{in.value("customer_id", ""),
                                                                        in.value("name", ""),
                                                                        in.value("description", ""),
                                                                        in.value("visibility", "")},
                                                 context.acting_identity));
        }});
    registry.add(Capability{
        .name = "rename_repository",
        .title = "Rename repository",
        .description = "Rename a live repository within its customer. "
                       "The name remains unique for that customer.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
                         {"required", json::array({"id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (ApiResult allowed = authorize_repository(context, in["id"], "rename"); !allowed.ok)
                return allowed;
            return one_result(
                context.store->rename_repository(in["id"], in["name"], context.acting_identity));
        }});
    registry.add(Capability{
        .name = "transfer_repository",
        .title = "Transfer repository",
        .description = "Transfer a live repository to another customer. The "
                       "destination must not already use its name.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"id", "customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (ApiResult allowed = authorize_repository(context, in["id"], "transfer");
                !allowed.ok)
                return allowed;
            return one_result(context.store->transfer_repository(
                in["id"], in["customer_id"], context.acting_identity));
        }});
    registry.add(Capability{
        .name = "set_repository_visibility",
        .title = "Set repository visibility",
        .description = "Set a live repository to private, organization, or "
                       "public visibility.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}},
                           {"visibility",
                            {{"type", "string"},
                             {"enum", json::array({"private", "organization", "public"})}}}}},
                         {"required", json::array({"id", "visibility"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (ApiResult allowed = authorize_repository(context, in["id"], "set visibility");
                !allowed.ok)
                return allowed;
            return one_result(context.store->set_repository_visibility(
                in["id"], in["visibility"], context.acting_identity));
        }});
    registry.add(Capability{
        .name = "delete_repository",
        .title = "Delete repository",
        .description = "Tombstone a repository. Its customer store is eligible "
                       "for collection after the 30-day retention window.",
        .input_schema = id_input(),
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (ApiResult allowed = authorize_repository(context, in["id"], "delete"); !allowed.ok)
                return allowed;
            const std::string deleted_at = timestamp_after(std::chrono::seconds::zero());
            const std::string retention_until = timestamp_after(std::chrono::days{30});
            return one_result(context.store->delete_repository(
                in["id"],
                control::RepositoryDeletion{deleted_at, retention_until},
                context.acting_identity));
        }});
    registry.add(Capability{.name = "list_repository_audit_events",
                            .title = "List repository audit events",
                            .description = "The repository lifecycle audit events, newest first.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"repository_id", {{"type", "string"}}},
                                               {"limit", {{"type", "integer"}}}}},
                                             {"required", json::array({"repository_id"})},
                                             {"additionalProperties", false}},
                            .output_schema = list_schema(audit_event_schema()),
                            .kind = CapabilityKind::Read,
                            .handler = [](const json& in, ApiContext& context) {
                                if (!context.store)
                                    return no_store();
                                if (ApiResult allowed = authorize_repository(
                                        context, in["repository_id"], "read audit events");
                                    !allowed.ok)
                                    return allowed;
                                return list_result(context.store->list_repository_audit_events(
                                    in["repository_id"], limit_of(in)));
                            }});
    registry.add(Capability{
        .name = "refs",
        .title = "Repository heads",
        .description = "The advertised liquid heads, one per repository ref.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"repository_id", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id"})},
                         {"additionalProperties", false}},
        .output_schema = list_schema(repository_ref_schema()),
        .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& context) {
            auto repository = repository_for_sync(context, in["repository_id"], "read refs");
            if (!repository)
                return from_store_error(repository.error());
            return list_result(context.store->list_repository_refs(repository->id));
        }});
    registry.add(Capability{
        .name = "find_missing",
        .title = "Find missing chunks",
        .description = "Return the requested liquid chunk digests absent from "
                       "the repository customer store.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"digests", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                         {"required", json::array({"repository_id", "digests"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"digests", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
        .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& context) {
            auto repository = repository_for_sync(context, in["repository_id"], "read chunks");
            if (!repository)
                return from_store_error(repository.error());
            if (!context.sync_store)
                return ApiResult::failure("store_unavailable",
                                          "no liquid customer store is configured");
            const auto missing = context.sync_store->find_missing(
                repository->customer_id, in["digests"].get<std::vector<std::string>>());
            if (!missing)
                return from_sync_store_error(missing.error());
            return ApiResult::success({{"digests", *missing}});
        }});
}

} // namespace lake::api
