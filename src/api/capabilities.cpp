#include "lake/api/capabilities.hpp"

#include <chrono>
#include <ctime>

#include "lake/audit/audit.hpp"

#include "lake/control/handles.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError &error) {
  return ApiResult::failure(error.code, error.message);
}

ApiResult no_store() {
  return ApiResult::failure(
      "store_unavailable",
      "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

std::expected<std::string, ApiError> global_object(const json &, ApiContext &) {
  return std::string{};
}

CapabilityObjectResolver entity_object(std::string object_type,
                                       std::string input_field) {
  return [object_type = std::move(object_type),
          input_field = std::move(input_field)](
             const json &input,
             ApiContext &context) -> std::expected<std::string, ApiError> {
    auto object =
        context.store->find_policy_object(object_type, input.at(input_field));
    if (!object) {
      return std::unexpected(ApiError{
          object.error().code, object.error().message, "/" + input_field});
    }
    return object->id;
  };
}

CapabilityObjectResolver optional_entity_object(std::string object_type,
                                                std::string input_field) {
  return [object_type = std::move(object_type),
          input_field = std::move(input_field)](
             const json &input,
             ApiContext &context) -> std::expected<std::string, ApiError> {
    const std::string external_id = input.value(input_field, "");
    if (external_id.empty()) {
      return std::string{};
    }
    auto object = context.store->find_policy_object(object_type, external_id);
    if (!object) {
      return std::unexpected(ApiError{
          object.error().code, object.error().message, "/" + input_field});
    }
    return object->id;
  };
}

CapabilityObjectResolver direct_object(std::string input_field) {
  return [input_field = std::move(input_field)](
             const json &input,
             ApiContext &) -> std::expected<std::string, ApiError> {
    return input.at(input_field).get<std::string>();
  };
}

std::expected<std::string, ApiError> scope_object(const json &input,
                                                  ApiContext &context) {
  const std::string team_id = input.value("team_id", "");
  const std::string type = team_id.empty() ? "organization" : "team";
  const std::string external_id =
      team_id.empty() ? input.value("organization_id", "") : team_id;
  auto object = context.store->find_policy_object(type, external_id);
  if (!object) {
    return std::unexpected(
        ApiError{object.error().code, object.error().message,
                 team_id.empty() ? "/organization_id" : "/team_id"});
  }
  return object->id;
}

std::expected<std::string, ApiError> invitation_object(const json &input,
                                                       ApiContext &context) {
  auto invitation = context.store->get_invitation(input.at("invitation_id"));
  if (!invitation) {
    return std::unexpected(ApiError{
        invitation.error().code, invitation.error().message, "/invitation_id"});
  }
  const std::string type =
      invitation->team_id.empty() ? "organization" : "team";
  const std::string external_id = invitation->team_id.empty()
                                      ? invitation->organization_id
                                      : invitation->team_id;
  auto object = context.store->find_policy_object(type, external_id);
  if (!object) {
    return std::unexpected(
        ApiError{object.error().code, object.error().message, {}});
  }
  return object->id;
}

json id_input() {
  return {
      {"type", "object"},
      {"properties", {{"id", {{"type", "string"}, {"description", "uuid"}}}}},
      {"required", json::array({"id"})},
      {"additionalProperties", false}};
}
json list_input() {
  return {{"type", "object"},
          {"properties",
           {{"limit",
             {{"type", "integer"},
              {"description", "max rows, default 100, newest first"}}}}},
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
             {{"type", "string"},
              {"enum", json::array({"individual", "organization"})}}},
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
             {{"type", "string"},
              {"enum", json::array({"private", "organization", "public"})}}},
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
  return {
      {"type", "object"},
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
          {"properties",
           {{"items", {{"type", "array"}, {"items", std::move(item)}}}}}};
}

int limit_of(const json &input) {
  const int limit = input.value("limit", 100);
  return limit <= 0 ? 100 : (limit > 1000 ? 1000 : limit);
}

json audit_event_schema() { return {{"type", "object"}}; }

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

audit::Filter audit_filter_of(const json &input) {
  return audit::Filter{
      .principal = input.value("principal", ""),
      .object = input.value("object", ""),
      .outcome = input.value("outcome", ""),
      .from = input.value("from", ""),
      .to = input.value("to", ""),
      .limit = limit_of(input),
  };
}

audit::IdentityOptions identity_options(const ApiContext &context) {
  return audit::IdentityOptions{
      .configured_seed = context.identity_seed,
      .instance_kek = context.instance_kek,
  };
}

template <class T> ApiResult list_result(control::Result<std::vector<T>> rows) {
  if (!rows)
    return from_store_error(rows.error());
  json items = json::array();
  for (const auto &row : *rows)
    items.push_back(control::to_json(row));
  return ApiResult::success({{"items", std::move(items)}});
}

template <class T> ApiResult one_result(control::Result<T> row) {
  if (!row)
    return from_store_error(row.error());
  return ApiResult::success(control::to_json(*row));
}

ApiResult authorized_object_list_result(
    control::Result<std::vector<control::PolicyObject>> rows,
    ApiContext &context) {
  if (!rows) {
    return from_store_error(rows.error());
  }
  std::vector<std::string> object_ids;
  object_ids.reserve(rows->size());
  for (const auto &row : *rows) {
    object_ids.push_back(row.id);
  }
  auto decisions =
      control::authorize_batch(*context.store, context.acting_identity,
                               object_ids, "read", context.authorization_cache);
  if (!decisions) {
    return from_store_error(decisions.error());
  }
  json items = json::array();
  for (const auto &row : *rows) {
    if (decisions->at(row.id).allowed) {
      items.push_back(control::to_json(row));
    }
  }
  return ApiResult::success({{"items", std::move(items)}});
}

} // namespace

namespace {

void register_server_info(Registry &registry, std::string_view version) {
  const std::string version_string(version);
  registry.add(Capability{
      .name = "server_info",
      .title = "Server info",
      .description =
          "Name, node, version and API level of this lake. No authentication.",
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
      .handler = [version_string](const json &, ApiContext &ctx) {
        return ApiResult::success(
            {{"name", "lake"},
             {"node", ctx.node_name},
             {"version", version_string},
             {"api", "v1"},
             {"store", ctx.store ? ctx.store->kind() : "none"}});
      }});
}

void register_identity_capabilities(Registry &registry) {
  // ---- users ----
  registry.add(Capability{
      .name = "list_users",
      .title = "List users",
      .description = "The most recent users, newest first.",
      .input_schema = list_input(),
      .output_schema = list_schema(user_schema()),
      .kind = CapabilityKind::Read,
      .object = "user collection",
      .action = "read",
      .resolve_object = global_object,
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? list_result(ctx.store->list_users(limit_of(in)))
                         : no_store();
      }});
  registry.add(
      Capability{.name = "get_user",
                 .title = "Get user",
                 .description = "One user by id.",
                 .input_schema = id_input(),
                 .output_schema = user_schema(),
                 .kind = CapabilityKind::Read,
                 .object = "user",
                 .action = "read",
                 .resolve_object = global_object,
                 .handler = [](const json &in, ApiContext &ctx) {
                   return ctx.store ? one_result(ctx.store->get_user(in["id"]))
                                    : no_store();
                 }});
  registry.add(
      Capability{.name = "create_user",
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
                 .handler = [](const json &in, ApiContext &ctx) {
                   if (!ctx.store)
                     return no_store();
                   return one_result(ctx.store->create_user(control::NewUser{
                       in.value("taap_id", ""), in.value("email", ""),
                       in.value("display_name", "")}));
                 }});

  // ---- organizations ----
  registry.add(Capability{
      .name = "list_organizations",
      .title = "List organizations",
      .description = "The most recent organizations, newest first.",
      .input_schema = list_input(),
      .output_schema = list_schema(organization_schema()),
      .kind = CapabilityKind::Read,
      .object = "organization collection",
      .action = "read",
      .resolve_object = global_object,
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store
                   ? list_result(ctx.store->list_organizations(limit_of(in)))
                   : no_store();
      }});
  registry.add(Capability{
      .name = "get_organization",
      .title = "Get organization",
      .description = "One organization by id.",
      .input_schema = id_input(),
      .output_schema = organization_schema(),
      .kind = CapabilityKind::Read,
      .object = "organization",
      .action = "read",
      .resolve_object = entity_object("organization", "id"),
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? one_result(ctx.store->get_organization(in["id"]))
                         : no_store();
      }});
  registry.add(Capability{
      .name = "create_organization",
      .title = "Create organization",
      .description = "Create an organization (identity, owns nothing). taap_id "
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
      .handler = [](const json &in, ApiContext &ctx) {
        if (!ctx.store)
          return no_store();
        return one_result(ctx.store->create_organization(
            control::NewOrganization{in.value("taap_id", ""),
                                     in.value("display_name", ""),
                                     in.value("customer_id", "")}));
      }});

  // ---- customers ----
  registry.add(Capability{
      .name = "list_customers",
      .title = "List customers",
      .description = "The most recent customers (tenants), newest first.",
      .input_schema = list_input(),
      .output_schema = list_schema(customer_schema()),
      .kind = CapabilityKind::Read,
      .object = "customer collection",
      .action = "read",
      .resolve_object = global_object,
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? list_result(ctx.store->list_customers(limit_of(in)))
                         : no_store();
      }});
  registry.add(Capability{
      .name = "get_customer",
      .title = "Get customer",
      .description = "One customer by id.",
      .input_schema = id_input(),
      .output_schema = customer_schema(),
      .kind = CapabilityKind::Read,
      .object = "customer",
      .action = "read",
      .resolve_object = entity_object("customer", "id"),
      .handler = [](const json &in, ApiContext &ctx) {
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
              {{"type", "string"},
               {"enum", json::array({"individual", "organization"})}}},
             {"user_id", {{"type", "string"}}},
             {"organization_id", {{"type", "string"}}},
             {"name",
              {{"type", "string"},
               {"description", "handle; generated when omitted"}}}}},
           {"required", json::array({"customer_type"})},
           {"additionalProperties", false}},
      .output_schema = customer_schema(),
      .kind = CapabilityKind::Action,
      .object = "customer collection",
      .action = "admin",
      .resolve_object = global_object,
      .handler = [](const json &in, ApiContext &ctx) {
        if (!ctx.store)
          return no_store();
        control::NewCustomer draft{
            in.value("customer_type", ""), in.value("user_id", ""),
            in.value("organization_id", ""), in.value("name", "")};
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
        return ApiResult::failure(
            "conflict", "could not find a free generated handle; pass name");
      }});
}

void register_repository_capabilities(Registry &registry) {
  // ---- repositories ----
  registry.add(Capability{
      .name = "list_repositories",
      .title = "List repositories",
      .description = "The most recent repositories, newest first; customer_id "
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
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? list_result(ctx.store->list_repositories(
                               in.value("customer_id", ""), limit_of(in)))
                         : no_store();
      }});
  registry.add(Capability{
      .name = "get_repository",
      .title = "Get repository",
      .description = "One repository by id.",
      .input_schema = id_input(),
      .output_schema = repository_schema(),
      .kind = CapabilityKind::Read,
      .object = "repository",
      .action = "read",
      .resolve_object = entity_object("repository", "id"),
      .handler = [](const json &in, ApiContext &ctx) {
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
      .input_schema =
          {{"type", "object"},
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
      .handler = [](const json &in, ApiContext &ctx) {
        if (!ctx.store)
          return no_store();
        return one_result(ctx.store->create_repository(control::NewRepository{
            in.value("customer_id", ""), in.value("name", ""),
            in.value("description", ""), in.value("visibility", "")}));
      }});
  registry.add(
      Capability{.name = "rename_repository",
                 .title = "Rename repository",
                 .description = "Rename a live repository within its customer.",
                 .input_schema = {{"type", "object"},
                                  {"properties",
                                   {{"id", {{"type", "string"}}},
                                    {"name", {{"type", "string"}}}}},
                                  {"required", json::array({"id", "name"})},
                                  {"additionalProperties", false}},
                 .output_schema = repository_schema(),
                 .kind = CapabilityKind::Action,
                 .object = "repository",
                 .action = "write",
                 .resolve_object = entity_object("repository", "id"),
                 .handler = [](const json &in, ApiContext &ctx) {
                   return ctx.store ? one_result(ctx.store->rename_repository(
                                          in.at("id"), in.at("name")))
                                    : no_store();
                 }});
  registry.add(Capability{
      .name = "transfer_repository",
      .title = "Transfer repository",
      .description = "Transfer a live repository to another customer.",
      .input_schema = {{"type", "object"},
                       {"properties",
                        {{"id", {{"type", "string"}}},
                         {"customer_id", {{"type", "string"}}}}},
                       {"required", json::array({"id", "customer_id"})},
                       {"additionalProperties", false}},
      .output_schema = repository_schema(),
      .kind = CapabilityKind::Action,
      .object = "repository",
      .action = "write",
      .resolve_object = entity_object("repository", "id"),
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? one_result(ctx.store->transfer_repository(
                               in.at("id"), in.at("customer_id")))
                         : no_store();
      }});
  registry.add(Capability{
      .name = "set_repository_visibility",
      .title = "Set repository visibility",
      .description =
          "Set a live repository to private, organization, or public.",
      .input_schema =
          {{"type", "object"},
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
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? one_result(ctx.store->set_repository_visibility(
                               in.at("id"), in.at("visibility")))
                         : no_store();
      }});
  registry.add(Capability{
      .name = "delete_repository",
      .title = "Delete repository",
      .description = "Tombstone a repository for 30 days before collection.",
      .input_schema = id_input(),
      .output_schema = repository_schema(),
      .kind = CapabilityKind::Action,
      .object = "repository",
      .action = "write",
      .resolve_object = entity_object("repository", "id"),
      .handler = [](const json &in, ApiContext &ctx) {
        if (!ctx.store) {
          return no_store();
        }
        const control::RepositoryDeletion deletion{
            .deleted_at = timestamp_after(std::chrono::seconds::zero()),
            .retention_until = timestamp_after(std::chrono::days{30}),
        };
        return one_result(ctx.store->delete_repository(in.at("id"), deletion));
      }});
}

void register_organization_capabilities(Registry &registry) {
  // ---- teams and membership ----
  registry.add(Capability{
      .name = "list_teams",
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
      .handler = [](const json &in, ApiContext &ctx) {
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
                          .handler = [](const json &in, ApiContext &ctx) {
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
      .handler = [](const json &in, ApiContext &ctx) {
        return one_result(ctx.store->create_team(
            control::NewTeam{in.at("organization_id"), in.at("name"),
                             in.value("display_name", ""), ctx.acting_identity,
                             ctx.request_id}));
      }});
  registry.add(Capability{
      .name = "list_memberships",
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
      .handler = [](const json &in, ApiContext &ctx) {
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
              {{"type", "string"},
               {"enum", json::array({"user", "workload"})}}},
             {"principal_id", {{"type", "string"}}},
             {"principal_taap_id", {{"type", "string"}}},
             {"role",
              {{"type", "string"},
               {"enum", json::array({"owner", "admin", "member"})}}},
             {"attestation_digest", {{"type", "string"}}}}},
           {"required",
            json::array({"organization_id", "principal_type", "principal_id",
                         "principal_taap_id", "role", "attestation_digest"})},
           {"additionalProperties", false}},
      .output_schema = membership_schema(),
      .kind = CapabilityKind::Action,
      .object = "organization or team",
      .action = "admin",
      .resolve_object = scope_object,
      .handler = [](const json &in, ApiContext &ctx) {
        return one_result(ctx.store->create_membership(control::NewMembership{
            in.at("organization_id"), in.value("team_id", ""),
            in.at("principal_type"), in.at("principal_id"),
            in.at("principal_taap_id"), in.at("role"),
            in.at("attestation_digest"), ctx.acting_identity, ctx.request_id}));
      }});

  // ---- invitations ----
  registry.add(Capability{
      .name = "list_invitations",
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
      .handler = [](const json &in, ApiContext &ctx) {
        return list_result(ctx.store->list_invitations(
            in.at("organization_id"), in.value("team_id", ""), limit_of(in)));
      }});
  registry.add(Capability{
      .name = "create_invitation",
      .title = "Create invitation",
      .description =
          "Invite an email address or handle into an organization or team.",
      .input_schema =
          {{"type", "object"},
           {"properties",
            {{"organization_id", {{"type", "string"}}},
             {"team_id", {{"type", "string"}}},
             {"invitee_type",
              {{"type", "string"}, {"enum", json::array({"email", "handle"})}}},
             {"invitee", {{"type", "string"}}},
             {"role",
              {{"type", "string"},
               {"enum", json::array({"owner", "admin", "member"})}}},
             {"expires_at", {{"type", "string"}}}}},
           {"required", json::array({"organization_id", "invitee_type",
                                     "invitee", "role", "expires_at"})},
           {"additionalProperties", false}},
      .output_schema = invitation_schema(),
      .kind = CapabilityKind::Action,
      .object = "organization or team",
      .action = "admin",
      .resolve_object = scope_object,
      .handler = [](const json &in, ApiContext &ctx) {
        return one_result(ctx.store->create_invitation(control::NewInvitation{
            in.at("organization_id"), in.value("team_id", ""),
            in.at("invitee_type"), in.at("invitee"), in.at("role"),
            in.at("expires_at"), ctx.acting_identity, ctx.request_id}));
      }});
  registry.add(Capability{
      .name = "accept_invitation",
      .title = "Accept invitation",
      .description = "Accept a pending invitation into an attested membership.",
      .input_schema = {{"type", "object"},
                       {"properties",
                        {{"invitation_id", {{"type", "string"}}},
                         {"principal_type",
                          {{"type", "string"},
                           {"enum", json::array({"user", "workload"})}}},
                         {"principal_id", {{"type", "string"}}},
                         {"principal_taap_id", {{"type", "string"}}},
                         {"attestation_digest", {{"type", "string"}}}}},
                       {"required",
                        json::array({"invitation_id", "principal_type",
                                     "principal_id", "principal_taap_id",
                                     "attestation_digest"})},
                       {"additionalProperties", false}},
      .output_schema = membership_schema(),
      .kind = CapabilityKind::Action,
      .object = "invitation scope",
      .action = "admin",
      .resolve_object = invitation_object,
      .handler = [](const json &in, ApiContext &ctx) {
        return one_result(
            ctx.store->accept_invitation(control::AcceptInvitation{
                in.at("invitation_id"), in.at("principal_type"),
                in.at("principal_id"), in.at("principal_taap_id"),
                in.at("attestation_digest"), ctx.acting_identity,
                ctx.request_id}));
      }});
}

void register_policy_capabilities(Registry &registry) {
  // ---- object graph and grants ----
  registry.add(Capability{
      .name = "list_objects",
      .title = "List objects",
      .description =
          "Objects under a parent, optionally narrowed by object type.",
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
      .resolve_object = [](const json &in, ApiContext &)
          -> std::expected<std::string, ApiError> {
        return in.value("parent_id", "");
      },
      .handler =
          [](const json &in, ApiContext &ctx) {
            return authorized_object_list_result(
                ctx.store->list_policy_objects(in.value("parent_id", ""),
                                               in.value("object_type", ""),
                                               limit_of(in)),
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
                          .handler = [](const json &in, ApiContext &ctx) {
                            return one_result(
                                ctx.store->get_policy_object(in.at("id")));
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
                       {"required", json::array({"object_type", "external_id",
                                                 "parent_id"})},
                       {"additionalProperties", false}},
      .output_schema = object_schema(),
      .kind = CapabilityKind::Action,
      .object = "policy object",
      .action = "admin",
      .resolve_object = direct_object("parent_id"),
      .handler = [](const json &in, ApiContext &ctx) {
        return one_result(ctx.store->create_policy_object(
            control::NewPolicyObject{in.at("object_type"), in.at("external_id"),
                                     in.at("parent_id"), ctx.acting_identity,
                                     ctx.request_id,
                                     in.value("inherit", true)}));
      }});
  registry.add(
      Capability{.name = "list_grants",
                 .title = "List grants",
                 .description = "Direct grants on one policy object.",
                 .input_schema = {{"type", "object"},
                                  {"properties",
                                   {{"object_id", {{"type", "string"}}},
                                    {"limit", {{"type", "integer"}}}}},
                                  {"required", json::array({"object_id"})},
                                  {"additionalProperties", false}},
                 .output_schema = list_schema(grant_schema()),
                 .kind = CapabilityKind::Read,
                 .object = "policy object grants",
                 .action = "admin",
                 .resolve_object = direct_object("object_id"),
                 .handler = [](const json &in, ApiContext &ctx) {
                   return list_result(ctx.store->list_grants(
                       {in.at("object_id").get<std::string>()}, std::nullopt,
                       limit_of(in)));
                 }});
  registry.add(Capability{
      .name = "create_grant",
      .title = "Create grant",
      .description =
          "Grant a policy role on an object using a TAAP attestation digest.",
      .input_schema =
          {{"type", "object"},
           {"properties",
            {{"principal_taap_id", {{"type", "string"}}},
             {"object_id", {{"type", "string"}}},
             {"role",
              {{"type", "string"},
               {"enum", json::array({"read", "write", "admin", "deploy",
                                     "review", "share"})}}},
             {"attestation_digest", {{"type", "string"}}}}},
           {"required", json::array({"principal_taap_id", "object_id", "role",
                                     "attestation_digest"})},
           {"additionalProperties", false}},
      .output_schema = grant_schema(),
      .kind = CapabilityKind::Action,
      .object = "policy object",
      .action = "admin",
      .resolve_object = direct_object("object_id"),
      .handler = [](const json &in, ApiContext &ctx) {
        return one_result(ctx.store->create_grant(control::NewGrant{
            in.at("principal_taap_id"), in.at("object_id"), in.at("role"),
            in.at("attestation_digest"), ctx.acting_identity, ctx.request_id}));
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
      .output_schema = {{"type", "object"},
                        {"properties", {{"items", {{"type", "array"}}}}}},
      .kind = CapabilityKind::Read,
      .object = "effective access",
      .action = "share",
      .resolve_object = direct_object("object_id"),
      .handler = [](const json &in, ApiContext &ctx) {
        auto entries = control::effective_access(*ctx.store, in.at("object_id"),
                                                 ctx.authorization_cache);
        if (!entries) {
          return from_store_error(entries.error());
        }
        json items = json::array();
        for (const auto &entry : *entries) {
          items.push_back(control::to_json(entry));
        }
        return ApiResult::success({{"items", std::move(items)}});
      }});
}

} // namespace

// ---- instance identity and audit ----
void register_audit_capabilities(Registry &registry) {
  registry.add(Capability{
      .name = "identity",
      .title = "Lake identity",
      .description =
          "The public TAAP/1 identity document for this lake instance.",
      .input_schema = {{"type", "object"},
                       {"properties", json::object()},
                       {"additionalProperties", false}},
      .output_schema = {{"type", "object"}},
      .kind = CapabilityKind::Read,
      .requires_auth = false,
      .object = "server",
      .action = "read",
      .resolve_object = global_object,
      .handler = [](const json &, ApiContext &context) {
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
      .handler = [](const json &, ApiContext &context) {
        if (!context.store) {
          return no_store();
        }
        auto value =
            audit::rotate_identity(*context.store, identity_options(context));
        if (!value) {
          return ApiResult::failure(value.error().code, value.error().message);
        }
        return ApiResult::success(audit::identity_document(*value));
      }});
  registry.add(Capability{
      .name = "audit",
      .title = "Query audit events",
      .description =
          "Query signed audit events by principal, object, outcome and time.",
      .input_schema = audit_filter_input(),
      .output_schema = list_schema(audit_event_schema()),
      .kind = CapabilityKind::Read,
      .object = "audit log",
      .action = "read",
      .resolve_object = global_object,
      .handler = [](const json &input, ApiContext &context) {
        if (!context.store) {
          return no_store();
        }
        auto events = context.store->list_audit_events(audit_filter_of(input));
        if (!events) {
          return from_store_error(events.error());
        }
        json items = json::array();
        for (const audit::Event &event : *events) {
          items.push_back(audit::to_json(event));
        }
        return ApiResult::success({{"items", std::move(items)}});
      }});
  registry.add(Capability{
      .name = "export_audit",
      .title = "Export audit events",
      .description = "Export matching signed audit events as JSON lines.",
      .input_schema = audit_filter_input(),
      .output_schema = {{"type", "object"},
                        {"properties", {{"jsonl", {{"type", "string"}}}}}},
      .kind = CapabilityKind::Read,
      .object = "audit log",
      .action = "read",
      .resolve_object = global_object,
      .handler = [](const json &input, ApiContext &context) {
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
      .handler = [](const json &, ApiContext &context) {
        if (!context.store) {
          return no_store();
        }
        auto events =
            context.store->list_audit_events(audit::Filter{.limit = 1000000});
        if (!events) {
          return from_store_error(events.error());
        }
        auto verified = audit::verify(*context.store, *events);
        if (!verified) {
          return ApiResult::failure(verified.error().code,
                                    verified.error().message);
        }
        return ApiResult::success({{"verified", true}});
      }});
  registry.add(Capability{
      .name = "set_audit_retention",
      .title = "Set audit retention",
      .description =
          "Set customer audit retention at or above the 400-day floor.",
      .input_schema = {{"type", "object"},
                       {"properties",
                        {{"customer_id", {{"type", "string"}}},
                         {"days", {{"type", "integer"}}}}},
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
      .handler = [](const json &input, ApiContext &context) {
        if (!context.store) {
          return no_store();
        }
        const int days = input["days"];
        auto valid = audit::validate_retention_days(days);
        if (!valid) {
          return ApiResult::failure(valid.error().code, valid.error().message);
        }
        auto updated =
            context.store->set_audit_retention_days(input["customer_id"], days);
        if (!updated) {
          return from_store_error(updated.error());
        }
        return ApiResult::success(
            {{"customer_id", input["customer_id"]}, {"days", days}});
      }});
}

void register_core_capabilities(Registry &registry, std::string_view version) {
  register_server_info(registry, version);
  register_identity_capabilities(registry);
  register_repository_capabilities(registry);
  register_organization_capabilities(registry);
  register_policy_capabilities(registry);
  register_audit_capabilities(registry);
}

} // namespace lake::api
