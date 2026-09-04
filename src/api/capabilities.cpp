#include "lake/api/capabilities.hpp"

#include "lake/audit/audit.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError& error) { return ApiResult::failure(error.code, error.message); }

ApiResult no_store() {
    return ApiResult::failure("store_unavailable", "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

json id_input() {
    return {{"type", "object"},
            {"properties", {{"id", {{"type", "string"}, {"description", "uuid"}}}}},
            {"required", json::array({"id"})},
            {"additionalProperties", false}};
}
json list_input() {
    return {{"type", "object"},
            {"properties", {{"limit", {{"type", "integer"}, {"description", "max rows, default 100, newest first"}}}}},
            {"additionalProperties", false}};
}
json user_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}}, {"taap_id", {{"type", json::array({"string", "null"})}}},
              {"email", {{"type", json::array({"string", "null"})}}}, {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}}, {"updated_at", {{"type", "string"}}}}}};
}
json organization_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}}, {"taap_id", {{"type", json::array({"string", "null"})}}},
              {"display_name", {{"type", "string"}}}, {"created_at", {{"type", "string"}}}, {"updated_at", {{"type", "string"}}}}}};
}
json customer_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}}, {"customer_type", {{"type", "string"}, {"enum", json::array({"individual", "organization"})}}},
              {"user_id", {{"type", json::array({"string", "null"})}}}, {"organization_id", {{"type", json::array({"string", "null"})}}},
              {"name", {{"type", "string"}}}, {"created_at", {{"type", "string"}}}, {"updated_at", {{"type", "string"}}}}}};
}
json repository_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}}, {"customer_id", {{"type", "string"}}}, {"name", {{"type", "string"}}},
              {"description", {{"type", "string"}}}, {"visibility", {{"type", "string"}, {"enum", json::array({"private", "public"})}}},
              {"created_at", {{"type", "string"}}}, {"updated_at", {{"type", "string"}}}}}};
}
json audit_event_schema() {
    return {{"type", "object"}};
}
json list_schema(json item) { return {{"type", "object"}, {"properties", {{"items", {{"type", "array"}, {"items", std::move(item)}}}}}}; }

int limit_of(const json& input) {
    const int limit = input.value("limit", 100);
    return limit <= 0 ? 100 : (limit > 1000 ? 1000 : limit);
}

json audit_filter_input() {
    return {{"type", "object"},
            {"properties",
             {{"principal", {{"type", "string"}}}, {"object", {{"type", "string"}}},
              {"outcome", {{"type", "string"}}}, {"from", {{"type", "string"}}},
              {"to", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}},
            {"additionalProperties", false}};
}

audit::Filter audit_filter_of(const json& input) {
    return audit::Filter{
        .principal = input.value("principal", ""),
        .object = input.value("object", ""),
        .outcome = input.value("outcome", ""),
        .from = input.value("from", ""),
        .to = input.value("to", ""),
        .limit = limit_of(input),
    };
}

audit::IdentityOptions identity_options(const ApiContext& context) {
    return audit::IdentityOptions{
        .configured_seed = context.identity_seed,
        .instance_kek = context.instance_kek,
    };
}

template <class T>
ApiResult list_result(control::Result<std::vector<T>> rows) {
    if (!rows) return from_store_error(rows.error());
    json items = json::array();
    for (const auto& row : *rows) items.push_back(control::to_json(row));
    return ApiResult::success({{"items", std::move(items)}});
}

template <class T>
ApiResult one_result(control::Result<T> row) {
    if (!row) return from_store_error(row.error());
    return ApiResult::success(control::to_json(*row));
}

}  // namespace

void register_core_capabilities(Registry& registry, std::string_view version) {
    const std::string version_string(version);
    registry.add(Capability{
        .name = "server_info",
        .title = "Server info",
        .description = "Name, node, version and API level of this lake. No authentication.",
        .input_schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"name", {{"type", "string"}}}, {"node", {{"type", "string"}}}, {"version", {{"type", "string"}}},
                                          {"api", {{"type", "string"}}}, {"store", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .handler = [version_string](const json&, ApiContext& ctx) {
            return ApiResult::success({{"name", "lake"}, {"node", ctx.node_name}, {"version", version_string}, {"api", "v1"},
                                       {"store", ctx.store ? ctx.store->kind() : "none"}});
        }});

    // ---- users ----
    registry.add(Capability{
        .name = "list_users", .title = "List users", .description = "The most recent users, newest first.",
        .input_schema = list_input(), .output_schema = list_schema(user_schema()), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? list_result(ctx.store->list_users(limit_of(in))) : no_store(); }});
    registry.add(Capability{
        .name = "get_user", .title = "Get user", .description = "One user by id.",
        .input_schema = id_input(), .output_schema = user_schema(), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? one_result(ctx.store->get_user(in["id"])) : no_store(); }});
    registry.add(Capability{
        .name = "create_user", .title = "Create user",
        .description = "Create a user (identity, owns nothing). taap_id and email are optional and unique.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"taap_id", {{"type", "string"}}}, {"email", {{"type", "string"}}}, {"display_name", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = user_schema(), .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) return no_store();
            return one_result(ctx.store->create_user(control::NewUser{in.value("taap_id", ""), in.value("email", ""), in.value("display_name", "")}));
        }});

    // ---- organizations ----
    registry.add(Capability{
        .name = "list_organizations", .title = "List organizations", .description = "The most recent organizations, newest first.",
        .input_schema = list_input(), .output_schema = list_schema(organization_schema()), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? list_result(ctx.store->list_organizations(limit_of(in))) : no_store(); }});
    registry.add(Capability{
        .name = "get_organization", .title = "Get organization", .description = "One organization by id.",
        .input_schema = id_input(), .output_schema = organization_schema(), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? one_result(ctx.store->get_organization(in["id"])) : no_store(); }});
    registry.add(Capability{
        .name = "create_organization", .title = "Create organization",
        .description = "Create an organization (identity, owns nothing). taap_id is optional and unique.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"taap_id", {{"type", "string"}}}, {"display_name", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = organization_schema(), .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) return no_store();
            return one_result(ctx.store->create_organization(control::NewOrganization{in.value("taap_id", ""), in.value("display_name", "")}));
        }});

    // ---- customers ----
    registry.add(Capability{
        .name = "list_customers", .title = "List customers", .description = "The most recent customers (tenants), newest first.",
        .input_schema = list_input(), .output_schema = list_schema(customer_schema()), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? list_result(ctx.store->list_customers(limit_of(in))) : no_store(); }});
    registry.add(Capability{
        .name = "get_customer", .title = "Get customer", .description = "One customer by id.",
        .input_schema = id_input(), .output_schema = customer_schema(), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? one_result(ctx.store->get_customer(in["id"])) : no_store(); }});
    registry.add(Capability{
        .name = "create_customer", .title = "Create customer",
        .description = "Create the tenant for a user (customer_type individual + user_id) or an organization "
                       "(customer_type organization + organization_id). name is the handle — one global lowercase "
                       "namespace; omitted, a {adjective}-{noun}-{number} handle is generated. Conflicts answer 409.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_type", {{"type", "string"}, {"enum", json::array({"individual", "organization"})}}},
                           {"user_id", {{"type", "string"}}}, {"organization_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}, {"description", "handle; generated when omitted"}}}}},
                         {"required", json::array({"customer_type"})},
                         {"additionalProperties", false}},
        .output_schema = customer_schema(), .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) return no_store();
            control::NewCustomer draft{in.value("customer_type", ""), in.value("user_id", ""), in.value("organization_id", ""), in.value("name", "")};
            if (!draft.name.empty()) return one_result(ctx.store->create_customer(draft));
            // Generated handle: retry on a collision, then give up honestly.
            for (int attempt = 0; attempt < 8; ++attempt) {
                draft.name = control::generate_handle();
                auto created = ctx.store->create_customer(draft);
                if (created || created.error().code != "conflict" || created.error().message.find("name") == std::string::npos) {
                    return one_result(std::move(created));
                }
            }
            return ApiResult::failure("conflict", "could not find a free generated handle; pass name");
        }});

    // ---- repositories ----
    registry.add(Capability{
        .name = "list_repositories", .title = "List repositories",
        .description = "The most recent repositories, newest first; customer_id narrows to one tenant.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}},
                         {"additionalProperties", false}},
        .output_schema = list_schema(repository_schema()), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? list_result(ctx.store->list_repositories(in.value("customer_id", ""), limit_of(in))) : no_store();
        }});
    registry.add(Capability{
        .name = "get_repository", .title = "Get repository", .description = "One repository by id.",
        .input_schema = id_input(), .output_schema = repository_schema(), .kind = CapabilityKind::Read,
        .handler = [](const json& in, ApiContext& ctx) { return ctx.store ? one_result(ctx.store->get_repository(in["id"])) : no_store(); }});
    registry.add(Capability{
        .name = "create_repository", .title = "Create repository",
        .description = "Create a repository owned by a customer: name is a slug unique within that customer "
                       "(the address is <customer handle>/<name>); visibility private (default) or public.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}}, {"name", {{"type", "string"}}},
                           {"description", {{"type", "string"}}},
                           {"visibility", {{"type", "string"}, {"enum", json::array({"private", "public"})}}}}},
                         {"required", json::array({"customer_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(), .kind = CapabilityKind::Action,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) return no_store();
            return one_result(ctx.store->create_repository(control::NewRepository{
                in.value("customer_id", ""), in.value("name", ""), in.value("description", ""), in.value("visibility", "")}));
        }});

    // ---- instance identity and audit ----
    registry.add(Capability{
        .name = "identity",
        .title = "Lake identity",
        .description = "The public TAAP/1 identity document for this lake instance.",
        .input_schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto value = audit::identity(*context.store, identity_options(context));
            if (!value) {
                return ApiResult::failure(value.error().code, value.error().message);
            }
            return ApiResult::success(audit::identity_document(*value));
        }});
    registry.add(Capability{
        .name = "rotate_identity",
        .title = "Rotate lake identity",
        .description = "Rotate the lake TAAP/1 Ed25519 identity with a dual-signed rotation document.",
        .input_schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto value = audit::rotate_identity(*context.store, identity_options(context));
            if (!value) {
                return ApiResult::failure(value.error().code, value.error().message);
            }
            return ApiResult::success(audit::identity_document(*value));
        }});
    registry.add(Capability{
        .name = "audit",
        .title = "Query audit events",
        .description = "Query signed audit events by principal, object, outcome and time.",
        .input_schema = audit_filter_input(),
        .output_schema = list_schema(audit_event_schema()),
        .kind = CapabilityKind::Read,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto events = context.store->list_audit_events(audit_filter_of(input));
            if (!events) {
                return from_store_error(events.error());
            }
            json items = json::array();
            for (const audit::Event& event : *events) {
                items.push_back(audit::to_json(event));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "export_audit",
        .title = "Export audit events",
        .description = "Export matching signed audit events as JSON lines.",
        .input_schema = audit_filter_input(),
        .output_schema = {{"type", "object"}, {"properties", {{"jsonl", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto events = context.store->list_audit_events(audit_filter_of(input));
            if (!events) {
                return from_store_error(events.error());
            }
            return ApiResult::success({{"jsonl", audit::json_lines(*events)}});
        }});
    registry.add(Capability{
        .name = "verify_audit",
        .title = "Verify audit chain",
        .description = "Verify the signed audit event digest chain.",
        .input_schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {{"verified", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Read,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto events = context.store->list_audit_events(audit::Filter{.limit = 1000000});
            if (!events) {
                return from_store_error(events.error());
            }
            auto verified = audit::verify(*context.store, *events);
            if (!verified) {
                return ApiResult::failure(verified.error().code, verified.error().message);
            }
            return ApiResult::success({{"verified", true}});
        }});
    registry.add(Capability{
        .name = "set_audit_retention",
        .title = "Set audit retention",
        .description = "Set customer audit retention at or above the 400-day floor.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", {{"type", "string"}}}, {"days", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "days"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {{"customer_id", {{"type", "string"}}}, {"days", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Action,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const int days = input["days"];
            auto valid = audit::validate_retention_days(days);
            if (!valid) {
                return ApiResult::failure(valid.error().code, valid.error().message);
            }
            auto updated = context.store->set_audit_retention_days(input["customer_id"], days);
            if (!updated) {
                return from_store_error(updated.error());
            }
            return ApiResult::success({{"customer_id", input["customer_id"]}, {"days", days}});
        }});
}

}  // namespace lake::api
