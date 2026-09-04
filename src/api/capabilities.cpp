#include "lake/api/capabilities.hpp"

#include <chrono>
#include <ctime>

#include "lake/audit/audit.hpp"

#include "lake/api/oauth.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/saml.hpp"

#include "lake/control/handles.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"

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

ApiResult no_search() {
    return ApiResult::failure("store_unavailable",
                              "no search store is configured (LAKE_STORE_ROOT)");
}

json search_schema() {
    return {{"type", "object"},
            {"properties",
             {{"customer", {{"type", "string"}}},
              {"repository", {{"type", "string"}}},
              {"path", {{"type", "string"}}},
              {"change", {{"type", "string"}}},
              {"snippet", {{"type", "string"}}},
              {"score", {{"type", "number"}}}}}};
}

ApiResult search_result(search::Result<std::vector<search::Hit>> hits) {
    if (!hits) {
        return ApiResult::failure(hits.error().code, hits.error().message);
    }
    json values = json::array();
    for (const search::Hit& hit : *hits) {
        values.push_back({{"customer", hit.customer},
                          {"repository", hit.repository},
                          {"path", hit.path},
                          {"change", hit.change},
                          {"snippet", hit.snippet},
                          {"score", hit.score}});
    }
    return ApiResult::success({{"items", std::move(values)}});
}

std::expected<std::string, ApiError> global_object(const json&, ApiContext&) {
    return std::string{};
}

std::expected<std::string, ApiError> self_object(const json&, ApiContext& context) {
    return "principal/" + context.acting_identity;
}

CapabilityObjectResolver entity_object(std::string object_type, std::string input_field) {
    return [object_type = std::move(object_type), input_field = std::move(input_field)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        auto object = context.store->find_policy_object(object_type, input.at(input_field));
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/" + input_field});
        }
        return object->id;
    };
}

CapabilityObjectResolver optional_entity_object(std::string object_type, std::string input_field) {
    return [object_type = std::move(object_type), input_field = std::move(input_field)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        const std::string external_id = input.value(input_field, "");
        if (external_id.empty()) {
            return std::string{};
        }
        auto object = context.store->find_policy_object(object_type, external_id);
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/" + input_field});
        }
        return object->id;
    };
}

CapabilityObjectResolver direct_object(std::string input_field) {
    return [input_field = std::move(input_field)](
               const json& input, ApiContext&) -> std::expected<std::string, ApiError> {
        return input.at(input_field).get<std::string>();
    };
}

std::expected<std::string, ApiError> scope_object(const json& input, ApiContext& context) {
    const std::string team_id = input.value("team_id", "");
    const std::string type = team_id.empty() ? "organization" : "team";
    const std::string external_id = team_id.empty() ? input.value("organization_id", "") : team_id;
    auto object = context.store->find_policy_object(type, external_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code,
                                        object.error().message,
                                        team_id.empty() ? "/organization_id" : "/team_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> invitation_object(const json& input, ApiContext& context) {
    auto invitation = context.store->get_invitation(input.at("invitation_id"));
    if (!invitation) {
        return std::unexpected(
            ApiError{invitation.error().code, invitation.error().message, "/invitation_id"});
    }
    const std::string type = invitation->team_id.empty() ? "organization" : "team";
    const std::string external_id =
        invitation->team_id.empty() ? invitation->organization_id : invitation->team_id;
    auto object = context.store->find_policy_object(type, external_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, {}});
    }
    return object->id;
}

std::expected<std::string, ApiError> sso_provider_object(const json& input, ApiContext& context) {
    const auto provider = context.store->get_sso_provider(input.at("provider_id"));
    if (!provider) {
        return std::unexpected(
            ApiError{provider.error().code, provider.error().message, "/provider_id"});
    }
    const auto object =
        context.store->find_policy_object("organization", provider->organization_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/provider_id"});
    }
    return object->id;
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

std::string timestamp_after(std::chrono::seconds offset) {
    const std::time_t value = std::time(nullptr) + offset.count();
    std::tm timestamp{};
    gmtime_r(&value, &timestamp);
    char result[sizeof "1970-01-01T00:00:00Z"];
    std::strftime(result, sizeof result, "%FT%TZ", &timestamp);
    return result;
}
json team_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"organization_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json membership_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"organization_id", {{"type", "string"}}},
              {"team_id", {{"type", json::array({"string", "null"})}}},
              {"principal_type", {{"type", "string"}}},
              {"principal_id", {{"type", "string"}}},
              {"principal_taap_id", {{"type", "string"}}},
              {"role", {{"type", "string"}}},
              {"attestation_digest", {{"type", "string"}}},
              {"granted_by_taap_id", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json invitation_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"organization_id", {{"type", "string"}}},
              {"team_id", {{"type", json::array({"string", "null"})}}},
              {"invitee_type", {{"type", "string"}}},
              {"invitee", {{"type", "string"}}},
              {"role", {{"type", "string"}}},
              {"expires_at", {{"type", "string"}}},
              {"status", {{"type", "string"}}},
              {"accepted_membership_id", {{"type", json::array({"string", "null"})}}},
              {"created_by_taap_id", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json object_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"object_type", {{"type", "string"}}},
              {"external_id", {{"type", "string"}}},
              {"parent_id", {{"type", json::array({"string", "null"})}}},
              {"inherit", {{"type", "boolean"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json grant_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"principal_taap_id", {{"type", "string"}}},
              {"object_id", {{"type", "string"}}},
              {"role", {{"type", "string"}}},
              {"attestation_digest", {{"type", "string"}}},
              {"granted_by_taap_id", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json list_schema(json item) {
    return {{"type", "object"},
            {"properties", {{"items", {{"type", "array"}, {"items", std::move(item)}}}}}};
}
json session_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"user_id", {{"type", {"string", "null"}}}},
              {"device_id", {{"type", {"string", "null"}}}},
              {"pool_id", {{"type", "string"}}},
              {"kind", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"scopes", {{"type", "array"}}},
              {"expires_at", {{"type", {"string", "null"}}}},
              {"revoked_at", {{"type", {"string", "null"}}}},
              {"created_at", {{"type", "string"}}}}}};
}
json device_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"user_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"passkey_credential_id", {{"type", {"string", "null"}}}},
              {"passkey_binding_attestation", {{"type", {"object", "null"}}}},
              {"revoked_at", {{"type", {"string", "null"}}}},
              {"created_at", {{"type", "string"}}}}}};
}

int limit_of(const json& input) {
    const int limit = input.value("limit", 100);
    return limit <= 0 ? 100 : (limit > 1000 ? 1000 : limit);
}

json audit_event_schema() {
    return {{"type", "object"}};
}

json audit_filter_input() {
    return {{"type", "object"},
            {"properties",
             {{"principal", {{"type", "string"}}},
              {"object", {{"type", "string"}}},
              {"outcome", {{"type", "string"}}},
              {"from", {{"type", "string"}}},
              {"to", {{"type", "string"}}},
              {"limit", {{"type", "integer"}}}}},
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

ApiResult issued_result(control::Result<control::IssuedSession> issued) {
    if (!issued)
        return from_store_error(issued.error());
    return ApiResult::success(
        {{"session", control::to_json(issued->session)}, {"secret", issued->secret}});
}

ApiResult authorized_object_list_result(control::Result<std::vector<control::PolicyObject>> rows,
                                        ApiContext& context) {
    if (!rows) {
        return from_store_error(rows.error());
    }
    std::vector<std::string> object_ids;
    object_ids.reserve(rows->size());
    for (const auto& row : *rows) {
        object_ids.push_back(row.id);
    }
    auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, object_ids, "read", context.authorization_cache);
    if (!decisions) {
        return from_store_error(decisions.error());
    }
    json items = json::array();
    for (const auto& row : *rows) {
        if (decisions->at(row.id).allowed) {
            items.push_back(control::to_json(row));
        }
    }
    return ApiResult::success({{"items", std::move(items)}});
}

} // namespace

namespace {

void register_server_info(Registry& registry, std::string_view version) {
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
        .object = "server",
        .action = "read",
        .resolve_object = global_object,
        .handler = [version_string](const json&, ApiContext& ctx) {
            return ApiResult::success({{"name", "lake"},
                                       {"node", ctx.node_name},
                                       {"version", version_string},
                                       {"api", "v1"},
                                       {"store", ctx.store ? ctx.store->kind() : "none"}});
        }});
}

void register_identity_capabilities(Registry& registry) {
    // ---- users ----
    registry.add(Capability{.name = "list_users",
                            .title = "List users",
                            .description = "The most recent users, newest first.",
                            .input_schema = list_input(),
                            .output_schema = list_schema(user_schema()),
                            .kind = CapabilityKind::Read,
                            .object = "user collection",
                            .action = "read",
                            .resolve_object = global_object,
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
                            .object = "user",
                            .action = "read",
                            .resolve_object = global_object,
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
        .object = "user collection",
        .action = "admin",
        .resolve_object = global_object,
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
                            .object = "organization collection",
                            .action = "read",
                            .resolve_object = global_object,
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
                            .object = "organization",
                            .action = "read",
                            .resolve_object = entity_object("organization", "id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                return ctx.store ? one_result(ctx.store->get_organization(in["id"]))
                                                 : no_store();
                            }});
    registry.add(Capability{.name = "create_organization",
                            .title = "Create organization",
                            .description =
                                "Create an organization (identity, owns nothing). taap_id "
                                "is optional and unique.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"taap_id", {{"type", "string"}}},
                                               {"display_name", {{"type", "string"}}},
                                               {"customer_id", {{"type", "string"}}}}},
                                             {"additionalProperties", false}},
                            .output_schema = organization_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "customer",
                            .action = "admin",
                            .resolve_object = optional_entity_object("customer", "customer_id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                if (!ctx.store)
                                    return no_store();
                                return one_result(ctx.store->create_organization(
                                    control::NewOrganization{in.value("taap_id", ""),
                                                             in.value("display_name", ""),
                                                             in.value("customer_id", "")}));
                            }});

    // ---- customers ----
    registry.add(Capability{.name = "list_customers",
                            .title = "List customers",
                            .description = "The most recent customers (tenants), newest first.",
                            .input_schema = list_input(),
                            .output_schema = list_schema(customer_schema()),
                            .kind = CapabilityKind::Read,
                            .object = "customer collection",
                            .action = "read",
                            .resolve_object = global_object,
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
                            .object = "customer",
                            .action = "read",
                            .resolve_object = entity_object("customer", "id"),
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
        .object = "customer collection",
        .action = "admin",
        .resolve_object = global_object,
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
    const json session_issue_schema = {
        {"type", "object"},
        {"properties", {{"session", session_schema()}, {"secret", {{"type", "string"}}}}}};
    registry.add(Capability{
        .name = "sign_up_password",
        .title = "Sign up with password",
        .description = "Create a lake-pool user with an Argon2id password "
                       "credential and issue a browser session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"password", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"identity_document", {{"type", "string"}}}}},
                         {"required", json::array({"email", "password", "identity_document"})},
                         {"additionalProperties", false}},
        .output_schema = session_issue_schema,
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            return issued_result(
                context.store->sign_up_password({input.value("pool_id", "lake"),
                                                 input.value("email", ""),
                                                 input.value("password", ""),
                                                 input.value("display_name", ""),
                                                 input.value("device_name", "browser"),
                                                 input.value("identity_document", "")}));
        }});
    registry.add(Capability{.name = "sign_in_password",
                            .title = "Sign in with password",
                            .description = "Verify a password credential. A user with enrolled MFA "
                                           "receives mfa_required until TOTP is verified.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"pool_id", {{"type", "string"}}},
                                               {"email", {{"type", "string"}}},
                                               {"password", {{"type", "string"}}},
                                               {"device_name", {{"type", "string"}}}}},
                                             {"required", json::array({"email", "password"})},
                                             {"additionalProperties", false}},
                            .output_schema = session_issue_schema,
                            .kind = CapabilityKind::Action,
                            .requires_auth = false,
                            .object = "authentication",
                            .action = "write",
                            .resolve_object = global_object,
                            .handler = [](const json& input, ApiContext& context) {
                                if (!context.store)
                                    return no_store();
                                return issued_result(context.store->sign_in_password(
                                    {input.value("pool_id", "lake"),
                                     input.value("email", ""),
                                     input.value("password", ""),
                                     "",
                                     input.value("device_name", "browser")}));
                            }});
    registry.add(
        Capability{.name = "verify_mfa",
                   .title = "Verify TOTP",
                   .description = "Verify an enrolled TOTP factor together with the "
                                  "password and issue a browser session.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"pool_id", {{"type", "string"}}},
                                      {"email", {{"type", "string"}}},
                                      {"password", {{"type", "string"}}},
                                      {"code", {{"type", "string"}}},
                                      {"device_name", {{"type", "string"}}}}},
                                    {"required", json::array({"email", "password", "code"})},
                                    {"additionalProperties", false}},
                   .output_schema = session_issue_schema,
                   .kind = CapabilityKind::Action,
                   .requires_auth = false,
                   .object = "authentication",
                   .action = "write",
                   .resolve_object = global_object,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.store)
                           return no_store();
                       return issued_result(
                           context.store->sign_in_totp({input.value("pool_id", "lake"),
                                                        input.value("email", ""),
                                                        input.value("password", ""),
                                                        input.value("code", ""),
                                                        input.value("device_name", "browser")}));
                   }});
    registry.add(Capability{
        .name = "enroll_mfa",
        .title = "Enroll TOTP",
        .description = "Enroll TOTP MFA and return its otpauth URI and recovery codes once.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"otpauth_uri", {{"type", "string"}}},
                            {"recovery_codes",
                             {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store)
                return no_store();
            auto result = context.store->enroll_mfa(context.acting_user_id, context.pool_id);
            if (!result)
                return from_store_error(result.error());
            return ApiResult::success({{"otpauth_uri",
                                        "otpauth://totp/lake:" + context.acting_user_id +
                                            "?secret=" + result->secret + "&issuer=lake"},
                                       {"recovery_codes", result->recovery_codes}});
        }});
    registry.add(Capability{
        .name = "disable_mfa",
        .title = "Disable MFA",
        .description = "Disable TOTP MFA after a fresh TOTP or recovery-code verification.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"code", {{"type", "string"}}}}},
                         {"required", json::array({"code"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto result = context.store->disable_mfa(
                context.acting_user_id, context.pool_id, input.value("code", ""));
            return result ? ApiResult::success({{"disabled", true}})
                          : from_store_error(result.error());
        }});
    registry.add(Capability{
        .name = "begin_passkey_registration",
        .title = "Begin passkey registration",
        .description = "Create five-minute WebAuthn registration options for "
                       "the authenticated user.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"pool_id", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            if (context.public_host.empty()) {
                return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for passkey registration");
            }
            const std::string pool_id =
                context.operator_credential ? input.value("pool_id", "lake") : context.pool_id;
            const auto user = context.store->get_user(context.acting_user_id);
            if (!user) {
                return from_store_error(user.error());
            }
            const std::string origin = "https://" + context.public_host;
            const auto challenge = context.store->begin_passkey_registration(
                context.acting_user_id, pool_id, context.public_host, origin);
            if (!challenge) {
                return from_store_error(challenge.error());
            }
            return ApiResult::success(
                {{"public_key",
                  {{"challenge", challenge->challenge},
                   {"rp", {{"id", challenge->rp_id}, {"name", "lake"}}},
                   {"user",
                    {{"id", control::identity_crypto::base64url_encode(user->id)},
                     {"name", user->email.empty() ? user->id : user->email},
                     {"displayName", user->display_name.empty() ? user->id : user->display_name}}},
                   {"pubKeyCredParams", json::array({{{"type", "public-key"}, {"alg", -7}}})},
                   {"timeout", 300000},
                   {"attestation", "none"}}}});
        }});
    registry.add(Capability{
        .name = "register_passkey",
        .title = "Register passkey",
        .description = "Record an attestation-less WebAuthn credential and a lake-signed TAAP "
                       "identity binding for the authenticated user.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"challenge", {{"type", "string"}}},
                           {"client_data_json", {{"type", "string"}}},
                           {"attestation_object", {{"type", "string"}}},
                           {"name", {{"type", "string"}}}}},
                         {"required",
                          json::array({"challenge", "client_data_json", "attestation_object"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            const std::string pool_id =
                context.operator_credential ? input.value("pool_id", "lake") : context.pool_id;
            auto result = context.store->register_passkey({context.acting_user_id,
                                                           pool_id,
                                                           input.value("challenge", ""),
                                                           input.value("client_data_json", ""),
                                                           input.value("attestation_object", ""),
                                                           input.value("name", "passkey"),
                                                           identity_options(context)});
            return result ? ApiResult::success({{"registered", true}})
                          : from_store_error(result.error());
        }});
    registry.add(Capability{
        .name = "begin_passkey_sign_in",
        .title = "Begin passkey sign-in",
        .description = "Create five-minute WebAuthn assertion options for a user pool.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"pool_id", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            if (context.public_host.empty()) {
                return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for passkey sign-in");
            }
            const std::string pool_id = input.value("pool_id", "lake");
            const std::string origin = "https://" + context.public_host;
            const auto challenge =
                context.store->begin_passkey_sign_in(pool_id, context.public_host, origin);
            if (!challenge) {
                return from_store_error(challenge.error());
            }
            return ApiResult::success({{"public_key",
                                        {{"challenge", challenge->challenge},
                                         {"rpId", challenge->rp_id},
                                         {"timeout", 300000},
                                         {"userVerification", "preferred"}}}});
        }});
    registry.add(Capability{
        .name = "sign_in_passkey",
        .title = "Sign in with passkey",
        .description = "Complete a verified WebAuthn assertion and issue a browser session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"challenge", {{"type", "string"}}},
                           {"credential_id", {{"type", "string"}}},
                           {"client_data_json", {{"type", "string"}}},
                           {"authenticator_data", {{"type", "string"}}},
                           {"signature", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}}}},
                         {"required",
                          json::array({"challenge",
                                       "credential_id",
                                       "client_data_json",
                                       "authenticator_data",
                                       "signature"})},
                         {"additionalProperties", false}},
        .output_schema = session_issue_schema,
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? issued_result(context.store->sign_in_passkey(
                                       {input.value("pool_id", "lake"),
                                        input.value("challenge", ""),
                                        input.value("credential_id", ""),
                                        input.value("client_data_json", ""),
                                        input.value("authenticator_data", ""),
                                        input.value("signature", ""),
                                        input.value("device_name", "browser")}))
                                 : no_store();
        }});
    registry.add(Capability{
        .name = "register_oauth_provider",
        .title = "Register OAuth provider",
        .description = "Register a pool-scoped OAuth or OpenID Connect provider.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"provider", {{"type", "string"}}},
                           {"issuer", {{"type", "string"}}},
                           {"client_id", {{"type", "string"}}},
                           {"client_secret", {{"type", "string"}}},
                           {"scopes", {{"type", "array"}}}}},
                         {"required",
                          json::array(
                              {"provider", "issuer", "client_id", "client_secret", "scopes"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"registered", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "OAuth provider registry",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto result = context.store->register_oauth_provider(
                {input.value("pool_id", "lake"),
                 input.value("provider", ""),
                 input.value("issuer", ""),
                 input.value("client_id", ""),
                 input.value("client_secret", ""),
                 input.value("scopes", std::vector<std::string>{})});
            return result ? ApiResult::success({{"registered", true}})
                          : from_store_error(result.error());
        }});
    registry.add(Capability{
        .name = "start_oauth",
        .title = "Start OAuth sign-in",
        .description = "Create a one-use OAuth authorization flow with PKCE and state.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"provider", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"identity_document", {{"type", "string"}}}}},
                         {"required", json::array({"provider"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"authorization_url", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            if (context.public_host.empty()) {
                return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for OAuth sign-in");
            }
            const std::string pool_id = input.value("pool_id", "lake");
            const std::string provider_name = input.value("provider", "");
            const auto provider = context.store->get_oauth_provider(pool_id, provider_name);
            if (!provider) {
                return from_store_error(provider.error());
            }
            if (!oauth_endpoints_allowed(oauth_endpoints(*provider))) {
                return ApiResult::failure("invalid_input",
                                          "OAuth endpoints must use HTTPS outside loopback tests");
            }
            control::OAuthFlow flow{pool_id,
                                    provider_name,
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    input.value("device_name", "browser"),
                                    input.value("identity_document", "")};
            if (flow.state.empty() || flow.code_verifier.empty() || flow.nonce.empty()) {
                return ApiResult::failure("store_error", "could not generate OAuth flow material");
            }
            const auto started = context.store->begin_oauth(flow);
            if (!started) {
                return from_store_error(started.error());
            }
            return ApiResult::success(
                {{"authorization_url",
                  oauth_authorization_url(*provider, flow, context.public_host)}});
        }});
    registry.add(Capability{
        .name = "start_sso_oidc",
        .title = "Start enterprise OIDC sign-in",
        .description = "Create a one-use enterprise OIDC authorization-code flow with PKCE.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}}}},
                         {"required", json::array({"provider_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (context.public_host.empty()) {
                return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for OIDC sign-in");
            }
            const std::string provider_id = input.at("provider_id");
            const auto provider = context.store->get_sso_oidc_provider(provider_id);
            if (!provider)
                return from_store_error(provider.error());
            if (!oauth_endpoints_allowed(oauth_endpoints(*provider))) {
                return ApiResult::failure("invalid_input",
                                          "OIDC endpoints must use HTTPS outside loopback tests");
            }
            control::OAuthFlow flow{provider->pool_id,
                                    provider->provider,
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    input.value("device_name", "browser"),
                                    input.value("taap_id", "")};
            if (flow.state.empty() || flow.code_verifier.empty() || flow.nonce.empty()) {
                return ApiResult::failure("store_error", "could not generate OIDC flow material");
            }
            const auto started = context.store->begin_oauth(flow);
            if (!started)
                return from_store_error(started.error());
            return ApiResult::success(
                {{"authorization_url",
                  oauth_authorization_url(*provider, flow, context.public_host)}});
        }});
    registry.add(Capability{.name = "create_token",
                            .title = "Create API token",
                            .description = "Issue a named, scoped API-token session. Static tokens "
                                           "require a reason and are audited.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"name", {{"type", "string"}}},
                                               {"scopes", {{"type", "array"}}},
                                               {"expires_in_seconds", {{"type", "integer"}}},
                                               {"static", {{"type", "boolean"}}},
                                               {"reason", {{"type", "string"}}}}},
                                             {"required", json::array({"name", "scopes"})},
                                             {"additionalProperties", false}},
                            .output_schema = session_issue_schema,
                            .kind = CapabilityKind::Action,
                            .object = "principal",
                            .action = "write",
                            .resolve_object = self_object,
                            .handler = [](const json& input, ApiContext& context) {
                                const bool is_static = input.value("static", false);
                                const int expires_in_seconds = input.value(
                                    "expires_in_seconds", is_static ? 0 : 60 * 60 * 24 * 30);
                                return context.store
                                           ? issued_result(context.store->create_token(
                                                 {context.acting_user_id,
                                                  context.pool_id,
                                                  input.value("name", ""),
                                                  input.value("reason", ""),
                                                  input.value("scopes", std::vector<std::string>{}),
                                                  expires_in_seconds,
                                                  is_static}))
                                           : no_store();
                            }});
    registry.add(Capability{.name = "list_sessions",
                            .title = "List sessions",
                            .description = "List the authenticated user's sessions and tokens.",
                            .input_schema = {{"type", "object"},
                                             {"properties", json::object()},
                                             {"additionalProperties", false}},
                            .output_schema = list_schema(session_schema()),
                            .kind = CapabilityKind::Read,
                            .object = "principal",
                            .action = "read",
                            .resolve_object = self_object,
                            .handler = [](const json&, ApiContext& context) {
                                return context.store ? list_result(context.store->list_sessions(
                                                           context.acting_user_id, context.pool_id))
                                                     : no_store();
                            }});
    registry.add(Capability{
        .name = "revoke_session",
        .title = "Revoke session",
        .description = "Revoke one of the authenticated user's sessions or tokens.",
        .input_schema = id_input(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            auto result = context.store->revoke_session(context.acting_user_id, input["id"]);
            return result ? ApiResult::success({{"revoked", true}})
                          : from_store_error(result.error());
        }});
    registry.add(Capability{.name = "list_devices",
                            .title = "List devices",
                            .description = "List the authenticated user's devices and signed "
                                           "passkey bindings.",
                            .input_schema = {{"type", "object"},
                                             {"properties", json::object()},
                                             {"additionalProperties", false}},
                            .output_schema = list_schema(device_schema()),
                            .kind = CapabilityKind::Read,
                            .object = "principal",
                            .action = "read",
                            .resolve_object = self_object,
                            .handler = [](const json&, ApiContext& context) {
                                return context.store ? list_result(context.store->list_devices(
                                                           context.acting_user_id))
                                                     : no_store();
                            }});
    registry.add(Capability{
        .name = "revoke_device",
        .title = "Revoke device",
        .description = "Revoke a device; attached sessions fail on their next request.",
        .input_schema = id_input(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            auto result = context.store->revoke_device(context.acting_user_id, input["id"]);
            return result ? ApiResult::success({{"revoked", true}})
                          : from_store_error(result.error());
        }});
}

void register_sso_capabilities(Registry& registry) {
    const json provider_input = {
        {"type", "object"},
        {"properties",
         {{"organization_id", {{"type", "string"}}},
          {"pool_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"protocol", {{"type", "string"}}},
          {"issuer", {{"type", "string"}}},
          {"metadata", {{"type", "string"}}},
          {"signing_certificate", {{"type", "string"}}},
          {"client_id", {{"type", "string"}}},
          {"client_secret", {{"type", "string"}}},
          {"scim_token", {{"type", "string"}}},
          {"attribute_mapping", {{"type", "object"}}},
          {"jit_provisioning", {{"type", "boolean"}}}}},
        {"required", json::array({"organization_id", "name", "protocol", "issuer", "scim_token"})},
        {"additionalProperties", false},
    };
    registry.add(Capability{
        .name = "register_sso_provider",
        .title = "Register SSO provider",
        .description = "Register or update an organization SAML or OpenID Connect provider.",
        .input_schema = provider_input,
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            return one_result(context.store->register_sso_provider(
                {input.value("organization_id", ""),
                 input.value("pool_id", "lake"),
                 input.value("name", ""),
                 input.value("protocol", ""),
                 input.value("issuer", ""),
                 input.value("metadata", ""),
                 input.value("signing_certificate", ""),
                 input.value("client_id", ""),
                 input.value("client_secret", ""),
                 input.value("scim_token", ""),
                 input.value("attribute_mapping", json::object()),
                 input.value("jit_provisioning", false)}));
        }});
    registry.add(Capability{
        .name = "list_sso_providers",
        .title = "List SSO providers",
        .description = "List an organization's SSO provider configurations without secrets.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"organization_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? list_result(context.store->list_sso_providers(
                                       input.value("organization_id", ""), limit_of(input)))
                                 : no_store();
        }});
    registry.add(Capability{
        .name = "test_sso_provider",
        .title = "Test SSO provider",
        .description = "Validate that a provider has the configuration required by its protocol.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"provider_id", {{"type", "string"}}}}},
                         {"required", json::array({"provider_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = sso_provider_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->test_sso_provider(input.at("provider_id")))
                       : no_store();
        }});
    registry.add(Capability{
        .name = "create_federation_issuer",
        .title = "Allow federation issuer claim",
        .description = "Allow an issuer to assert one named claim for an organization.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"issuer", {{"type", "string"}}},
                           {"claim", {{"type", "string"}}}}},
                         {"required", json::array({"organization_id", "issuer", "claim"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->create_federation_issuer(
                             {input.at("organization_id"), input.at("issuer"), input.at("claim")}))
                       : no_store();
        }});
    registry.add(Capability{
        .name = "list_federation_issuers",
        .title = "List federation issuer policy",
        .description = "List the issuer and claim pairs accepted by an organization.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"organization_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? list_result(context.store->list_federation_issuers(
                                       input.value("organization_id", ""), limit_of(input)))
                                 : no_store();
        }});
    registry.add(Capability{
        .name = "sign_in_sso",
        .title = "Sign in with SSO assertion",
        .description = "Issue a session after provider verification and federation issuer policy.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"issuer", {{"type", "string"}}},
                           {"subject", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"claims", {{"type", "object"}}}}},
                         {"required", json::array({"provider_id", "issuer", "subject", "claims"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? issued_result(context.store->sign_in_sso(
                                       {input.value("provider_id", ""),
                                        input.value("issuer", ""),
                                        input.value("subject", ""),
                                        input.value("email", ""),
                                        input.value("display_name", ""),
                                        input.value("taap_id", ""),
                                        input.value("device_name", "browser"),
                                        input.value("claims", json::object())}))
                                 : no_store();
        }});
    registry.add(Capability{
        .name = "sign_in_saml",
        .title = "Sign in with signed SAML assertion",
        .description = "Verify the configured SAML signing certificate before issuing a session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"saml_response", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}}}},
                         {"required", json::array({"provider_id", "saml_response"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto provider = context.store->get_sso_provider(input.at("provider_id"));
            if (!provider) {
                return from_store_error(provider.error());
            }
            if (provider->protocol != "saml") {
                return ApiResult::failure("invalid_input", "provider is not a SAML provider");
            }
            auto assertion =
                control::saml::verify_response(input.at("saml_response").get<std::string>(),
                                               provider->signing_certificate,
                                               provider->issuer);
            if (!assertion) {
                return ApiResult::failure("unauthorized", assertion.error());
            }
            return issued_result(context.store->sign_in_sso({provider->id,
                                                             assertion->issuer,
                                                             assertion->subject,
                                                             assertion->email,
                                                             assertion->display_name,
                                                             input.value("taap_id", ""),
                                                             input.value("device_name", "browser"),
                                                             assertion->claims}));
        }});
    registry.add(Capability{
        .name = "upsert_scim_user",
        .title = "Provision SCIM user",
        .description =
            "Create, update, or deactivate a SCIM user and revoke sessions on deactivation.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"active", {{"type", "boolean"}}},
                           {"attributes", {{"type", "object"}}}}},
                         {"required",
                          json::array({"provider_id", "external_id", "taap_id", "active"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = sso_provider_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? one_result(context.store->upsert_scim_user(
                                       {input.value("provider_id", ""),
                                        input.value("external_id", ""),
                                        input.value("email", ""),
                                        input.value("display_name", ""),
                                        input.value("taap_id", ""),
                                        input.value("active", true),
                                        input.value("attributes", json::object())}))
                                 : no_store();
        }});
    registry.add(Capability{
        .name = "scim_upsert_user",
        .title = "SCIM user resource",
        .description = "Authenticate a provider bearer token and apply a SCIM user resource.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"bearer_token", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"active", {{"type", "boolean"}}},
                           {"attributes", {{"type", "object"}}}}},
                         {"required",
                          json::array(
                              {"provider_id", "bearer_token", "external_id", "taap_id", "active"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            const auto provider =
                context.store->authenticate_scim(input.at("provider_id"), input.at("bearer_token"));
            if (!provider)
                return from_store_error(provider.error());
            return one_result(
                context.store->upsert_scim_user({input.at("provider_id"),
                                                 input.at("external_id"),
                                                 input.value("email", ""),
                                                 input.value("display_name", ""),
                                                 input.at("taap_id"),
                                                 input.at("active"),
                                                 input.value("attributes", json::object())}));
        }});
    registry.add(Capability{
        .name = "upsert_scim_group",
        .title = "Provision SCIM group",
        .description = "Create or update a SCIM group and map its users to a lake team.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"team_name", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"member_external_ids", {{"type", "array"}}},
                           {"attributes", {{"type", "object"}}}}},
                         {"required",
                          json::array({"provider_id",
                                       "external_id",
                                       "team_name",
                                       "display_name",
                                       "member_external_ids"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = sso_provider_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->upsert_scim_group(
                             {input.value("provider_id", ""),
                              input.value("external_id", ""),
                              input.value("team_name", ""),
                              input.value("display_name", ""),
                              input.value("member_external_ids", std::vector<std::string>{}),
                              input.value("attributes", json::object())}))
                       : no_store();
        }});
    registry.add(Capability{.name = "list_sso_provisioning_logs",
                            .title = "List SSO provisioning logs",
                            .description = "List SCIM provisioning outcomes for an SSO provider.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"provider_id", {{"type", "string"}}},
                                               {"limit", {{"type", "integer"}}}}},
                                             {"required", json::array({"provider_id"})},
                                             {"additionalProperties", false}},
                            .output_schema = {{"type", "object"}},
                            .kind = CapabilityKind::Read,
                            .object = "organization",
                            .action = "admin",
                            .resolve_object = sso_provider_object,
                            .handler = [](const json& input, ApiContext& context) {
                                return context.store
                                           ? list_result(context.store->list_sso_provisioning_logs(
                                                 input.value("provider_id", ""), limit_of(input)))
                                           : no_store();
                            }});
}

void register_repository_capabilities(Registry& registry) {
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
                            .object = "repository collection",
                            .action = "read",
                            .resolve_object = optional_entity_object("customer", "customer_id"),
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
                            .object = "repository",
                            .action = "read",
                            .resolve_object = entity_object("repository", "id"),
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
                       "private (default), organization, or public.",
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
        .object = "customer",
        .action = "write",
        .resolve_object = entity_object("customer", "customer_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store)
                return no_store();
            return one_result(
                ctx.store->create_repository(control::NewRepository{in.value("customer_id", ""),
                                                                    in.value("name", ""),
                                                                    in.value("description", ""),
                                                                    in.value("visibility", "")}));
        }});
    registry.add(Capability{
        .name = "rename_repository",
        .title = "Rename repository",
        .description = "Rename a live repository within its customer.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
                         {"required", json::array({"id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? one_result(ctx.store->rename_repository(in.at("id"), in.at("name")))
                             : no_store();
        }});
    registry.add(Capability{
        .name = "transfer_repository",
        .title = "Transfer repository",
        .description = "Transfer a live repository to another customer.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"id", "customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? one_result(ctx.store->transfer_repository(in.at("id"),
                                                                         in.at("customer_id")))
                             : no_store();
        }});
    registry.add(Capability{
        .name = "set_repository_visibility",
        .title = "Set repository visibility",
        .description = "Set a live repository to private, organization, or public.",
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
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? one_result(ctx.store->set_repository_visibility(in.at("id"),
                                                                               in.at("visibility")))
                             : no_store();
        }});
    registry.add(Capability{.name = "delete_repository",
                            .title = "Delete repository",
                            .description = "Tombstone a repository for 30 days before collection.",
                            .input_schema = id_input(),
                            .output_schema = repository_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "repository",
                            .action = "write",
                            .resolve_object = entity_object("repository", "id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                if (!ctx.store) {
                                    return no_store();
                                }
                                const control::RepositoryDeletion deletion{
                                    .deleted_at = timestamp_after(std::chrono::seconds::zero()),
                                    .retention_until = timestamp_after(std::chrono::days{30}),
                                };
                                return one_result(
                                    ctx.store->delete_repository(in.at("id"), deletion));
                            }});
}

void register_organization_capabilities(Registry& registry) {
    // ---- teams and membership ----
    registry.add(Capability{.name = "list_teams",
                            .title = "List teams",
                            .description = "The teams in an organization, newest first.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"organization_id", {{"type", "string"}}},
                                               {"limit", {{"type", "integer"}}}}},
                                             {"required", json::array({"organization_id"})},
                                             {"additionalProperties", false}},
                            .output_schema = list_schema(team_schema()),
                            .kind = CapabilityKind::Read,
                            .object = "organization",
                            .action = "read",
                            .resolve_object = entity_object("organization", "organization_id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                return list_result(
                                    ctx.store->list_teams(in.at("organization_id"), limit_of(in)));
                            }});
    registry.add(Capability{.name = "get_team",
                            .title = "Get team",
                            .description = "One team by id.",
                            .input_schema = id_input(),
                            .output_schema = team_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "team",
                            .action = "read",
                            .resolve_object = entity_object("team", "id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                return one_result(ctx.store->get_team(in.at("id")));
                            }});
    registry.add(Capability{
        .name = "create_team",
        .title = "Create team",
        .description = "Create a team under an organization. Organization admins "
                       "may delegate it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}}}},
                         {"required", json::array({"organization_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = team_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->create_team(control::NewTeam{in.at("organization_id"),
                                                                      in.at("name"),
                                                                      in.value("display_name", ""),
                                                                      ctx.acting_identity,
                                                                      ctx.request_id}));
        }});
    registry.add(
        Capability{.name = "list_memberships",
                   .title = "List memberships",
                   .description = "Organization or team memberships with their TAAP "
                                  "attestation digests.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"organization_id", {{"type", "string"}}},
                                      {"team_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"required", json::array({"organization_id"})},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(membership_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "organization or team",
                   .action = "read",
                   .resolve_object = scope_object,
                   .handler = [](const json& in, ApiContext& ctx) {
                       return list_result(ctx.store->list_memberships(
                           in.at("organization_id"), in.value("team_id", ""), limit_of(in)));
                   }});
    registry.add(Capability{
        .name = "create_membership",
        .title = "Create membership",
        .description = "Grant owner, admin, or member in an organization or team "
                       "using TAAP evidence.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"organization_id", {{"type", "string"}}},
               {"team_id", {{"type", "string"}}},
               {"principal_type",
                {{"type", "string"}, {"enum", json::array({"user", "workload"})}}},
               {"principal_id", {{"type", "string"}}},
               {"principal_taap_id", {{"type", "string"}}},
               {"role", {{"type", "string"}, {"enum", json::array({"owner", "admin", "member"})}}},
               {"attestation_digest", {{"type", "string"}}}}},
             {"required",
              json::array({"organization_id",
                           "principal_type",
                           "principal_id",
                           "principal_taap_id",
                           "role",
                           "attestation_digest"})},
             {"additionalProperties", false}},
        .output_schema = membership_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization or team",
        .action = "admin",
        .resolve_object = scope_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->create_membership(control::NewMembership{in.at("organization_id"),
                                                                    in.value("team_id", ""),
                                                                    in.at("principal_type"),
                                                                    in.at("principal_id"),
                                                                    in.at("principal_taap_id"),
                                                                    in.at("role"),
                                                                    in.at("attestation_digest"),
                                                                    ctx.acting_identity,
                                                                    ctx.request_id}));
        }});

    // ---- invitations ----
    registry.add(
        Capability{.name = "list_invitations",
                   .title = "List invitations",
                   .description = "Invitations in an organization or team, including expiry "
                                  "and acceptance.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"organization_id", {{"type", "string"}}},
                                      {"team_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"required", json::array({"organization_id"})},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(invitation_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "organization or team",
                   .action = "admin",
                   .resolve_object = scope_object,
                   .handler = [](const json& in, ApiContext& ctx) {
                       return list_result(ctx.store->list_invitations(
                           in.at("organization_id"), in.value("team_id", ""), limit_of(in)));
                   }});
    registry.add(Capability{
        .name = "create_invitation",
        .title = "Create invitation",
        .description = "Invite an email address or handle into an organization or team.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"organization_id", {{"type", "string"}}},
               {"team_id", {{"type", "string"}}},
               {"invitee_type", {{"type", "string"}, {"enum", json::array({"email", "handle"})}}},
               {"invitee", {{"type", "string"}}},
               {"role", {{"type", "string"}, {"enum", json::array({"owner", "admin", "member"})}}},
               {"expires_at", {{"type", "string"}}}}},
             {"required",
              json::array({"organization_id", "invitee_type", "invitee", "role", "expires_at"})},
             {"additionalProperties", false}},
        .output_schema = invitation_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization or team",
        .action = "admin",
        .resolve_object = scope_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->create_invitation(control::NewInvitation{in.at("organization_id"),
                                                                    in.value("team_id", ""),
                                                                    in.at("invitee_type"),
                                                                    in.at("invitee"),
                                                                    in.at("role"),
                                                                    in.at("expires_at"),
                                                                    ctx.acting_identity,
                                                                    ctx.request_id}));
        }});
    registry.add(Capability{
        .name = "accept_invitation",
        .title = "Accept invitation",
        .description = "Accept a pending invitation into an attested membership.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"invitation_id", {{"type", "string"}}},
                           {"principal_type",
                            {{"type", "string"}, {"enum", json::array({"user", "workload"})}}},
                           {"principal_id", {{"type", "string"}}},
                           {"principal_taap_id", {{"type", "string"}}},
                           {"attestation_digest", {{"type", "string"}}}}},
                         {"required",
                          json::array({"invitation_id",
                                       "principal_type",
                                       "principal_id",
                                       "principal_taap_id",
                                       "attestation_digest"})},
                         {"additionalProperties", false}},
        .output_schema = membership_schema(),
        .kind = CapabilityKind::Action,
        .object = "invitation scope",
        .action = "admin",
        .resolve_object = invitation_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->accept_invitation(control::AcceptInvitation{in.at("invitation_id"),
                                                                       in.at("principal_type"),
                                                                       in.at("principal_id"),
                                                                       in.at("principal_taap_id"),
                                                                       in.at("attestation_digest"),
                                                                       ctx.acting_identity,
                                                                       ctx.request_id}));
        }});
}

void register_policy_capabilities(Registry& registry) {
    // ---- object graph and grants ----
    registry.add(Capability{
        .name = "list_objects",
        .title = "List objects",
        .description = "Objects under a parent, optionally narrowed by object type.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"parent_id", {{"type", "string"}}},
                           {"object_type", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"additionalProperties", false}},
        .output_schema = list_schema(object_schema()),
        .kind = CapabilityKind::Read,
        .object = "policy object collection",
        .action = "read",
        .resolve_object = [](const json& in, ApiContext&) -> std::expected<std::string, ApiError> {
            return in.value("parent_id", "");
        },
        .handler =
            [](const json& in, ApiContext& ctx) {
                return authorized_object_list_result(
                    ctx.store->list_policy_objects(
                        in.value("parent_id", ""), in.value("object_type", ""), limit_of(in)),
                    ctx);
            }});
    registry.add(Capability{.name = "get_object",
                            .title = "Get object",
                            .description = "One policy object by id.",
                            .input_schema = id_input(),
                            .output_schema = object_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "policy object",
                            .action = "read",
                            .resolve_object = direct_object("id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                return one_result(ctx.store->get_policy_object(in.at("id")));
                            }});
    registry.add(Capability{
        .name = "create_object",
        .title = "Create object",
        .description = "Add a path, environment, secret mount, or secret to the "
                       "object graph.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"object_type", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"parent_id", {{"type", "string"}}},
                           {"inherit", {{"type", "boolean"}}}}},
                         {"required", json::array({"object_type", "external_id", "parent_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "policy object",
        .action = "admin",
        .resolve_object = direct_object("parent_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->create_policy_object(
                control::NewPolicyObject{in.at("object_type"),
                                         in.at("external_id"),
                                         in.at("parent_id"),
                                         ctx.acting_identity,
                                         ctx.request_id,
                                         in.value("inherit", true)}));
        }});
    registry.add(Capability{
        .name = "list_grants",
        .title = "List grants",
        .description = "Direct grants on one policy object.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"object_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"object_id"})},
                         {"additionalProperties", false}},
        .output_schema = list_schema(grant_schema()),
        .kind = CapabilityKind::Read,
        .object = "policy object grants",
        .action = "admin",
        .resolve_object = direct_object("object_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return list_result(ctx.store->list_grants(
                {in.at("object_id").get<std::string>()}, std::nullopt, limit_of(in)));
        }});
    registry.add(Capability{
        .name = "create_grant",
        .title = "Create grant",
        .description = "Grant a policy role on an object using a TAAP attestation digest.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"principal_taap_id", {{"type", "string"}}},
               {"object_id", {{"type", "string"}}},
               {"role",
                {{"type", "string"},
                 {"enum", json::array({"read", "write", "admin", "deploy", "review", "share"})}}},
               {"attestation_digest", {{"type", "string"}}}}},
             {"required",
              json::array({"principal_taap_id", "object_id", "role", "attestation_digest"})},
             {"additionalProperties", false}},
        .output_schema = grant_schema(),
        .kind = CapabilityKind::Action,
        .object = "policy object",
        .action = "admin",
        .resolve_object = direct_object("object_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->create_grant(control::NewGrant{in.at("principal_taap_id"),
                                                                        in.at("object_id"),
                                                                        in.at("role"),
                                                                        in.at("attestation_digest"),
                                                                        ctx.acting_identity,
                                                                        ctx.request_id}));
        }});
    registry.add(Capability{
        .name = "effective_access",
        .title = "Effective access",
        .description = "Who can access an object, which actions they have, and "
                       "whether each grant is inherited.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"object_id", {{"type", "string"}}}}},
                         {"required", json::array({"object_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {{"items", {{"type", "array"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "effective access",
        .action = "share",
        .resolve_object = direct_object("object_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            auto entries =
                control::effective_access(*ctx.store, in.at("object_id"), ctx.authorization_cache);
            if (!entries) {
                return from_store_error(entries.error());
            }
            json items = json::array();
            for (const auto& entry : *entries) {
                items.push_back(control::to_json(entry));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
}

void register_search_capability(Registry& registry) {
    registry.add(
        Capability{.name = "search",
                   .title = "Search repositories",
                   .description = "Ranked BM25F search; quoted text is a substring query and "
                                  "/expression/ is a regex query.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"query", {{"type", "string"}}},
                                      {"customer", {{"type", "string"}}},
                                      {"repository", {{"type", "string"}}},
                                      {"ref", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"required", json::array({"query"})},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(search_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "repository search results",
                   .action = "read",
                   .resolve_object = global_object,
                   .authorizes_candidates = true,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.search) {
                           return no_search();
                       }
                       if (!context.store) {
                           return no_store();
                       }
                       return search_result(
                           context.search->query(search::Query{input.at("query").get<std::string>(),
                                                               input.value("customer", ""),
                                                               input.value("repository", ""),
                                                               input.value("ref", ""),
                                                               limit_of(input)},
                                                 context.acting_identity,
                                                 *context.store));
                   }});
}

} // namespace

// ---- instance identity and audit ----
void register_audit_capabilities(Registry& registry) {
    registry.add(
        Capability{.name = "identity",
                   .title = "Lake identity",
                   .description = "The public TAAP/1 identity document for this lake instance.",
                   .input_schema = {{"type", "object"},
                                    {"properties", json::object()},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Read,
                   .requires_auth = false,
                   .object = "server",
                   .action = "read",
                   .resolve_object = global_object,
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
        .description = "Rotate the lake TAAP/1 Ed25519 identity with a "
                       "dual-signed rotation document.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "server",
        .action = "admin",
        .resolve_object = global_object,
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
        .object = "audit log",
        .action = "read",
        .resolve_object = global_object,
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
        .object = "audit log",
        .action = "read",
        .resolve_object = global_object,
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
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"verified", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "audit log",
        .action = "read",
        .resolve_object = global_object,
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
                         {"properties",
                          {{"customer_id", {{"type", "string"}}}, {"days", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "days"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"customer_id", {{"type", "string"}}},
                            {"days", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = entity_object("customer", "customer_id"),
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

void register_core_capabilities(Registry& registry, std::string_view version) {
    register_server_info(registry, version);
    register_identity_capabilities(registry);
    register_sso_capabilities(registry);
    register_repository_capabilities(registry);
    register_organization_capabilities(registry);
    register_policy_capabilities(registry);
    register_search_capability(registry);
    register_audit_capabilities(registry);
}

} // namespace lake::api
