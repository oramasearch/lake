#include "lake/api/capabilities.hpp"

#include "lake/control/handles.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"

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
  for (const search::Hit &hit : *hits) {
    values.push_back({{"customer", hit.customer},
                      {"repository", hit.repository},
                      {"path", hit.path},
                      {"change", hit.change},
                      {"snippet", hit.snippet},
                      {"score", hit.score}});
  }
  return ApiResult::success({{"items", std::move(values)}});
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
  return {
      {"type", "object"},
      {"properties",
       {{"id", {{"type", "string"}}},
        {"customer_id", {{"type", "string"}}},
        {"name", {{"type", "string"}}},
        {"description", {{"type", "string"}}},
        {"visibility",
         {{"type", "string"}, {"enum", json::array({"private", "public"})}}},
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

} // namespace

void register_core_capabilities(Registry &registry, std::string_view version) {
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
      .handler = [version_string](const json &, ApiContext &ctx) {
        return ApiResult::success(
            {{"name", "lake"},
             {"node", ctx.node_name},
             {"version", version_string},
             {"api", "v1"},
             {"store", ctx.store ? ctx.store->kind() : "none"}});
      }});

  // ---- users ----
  registry.add(Capability{
      .name = "list_users",
      .title = "List users",
      .description = "The most recent users, newest first.",
      .input_schema = list_input(),
      .output_schema = list_schema(user_schema()),
      .kind = CapabilityKind::Read,
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
      .handler = [](const json &in, ApiContext &ctx) {
        return ctx.store ? one_result(ctx.store->get_organization(in["id"]))
                         : no_store();
      }});
  registry.add(
      Capability{.name = "create_organization",
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
                 .handler = [](const json &in, ApiContext &ctx) {
                   if (!ctx.store)
                     return no_store();
                   return one_result(ctx.store->create_organization(
                       control::NewOrganization{in.value("taap_id", ""),
                                                in.value("display_name", "")}));
                 }});

  // ---- customers ----
  registry.add(Capability{
      .name = "list_customers",
      .title = "List customers",
      .description = "The most recent customers (tenants), newest first.",
      .input_schema = list_input(),
      .output_schema = list_schema(customer_schema()),
      .kind = CapabilityKind::Read,
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
                     "private (default) or public.",
      .input_schema = {{"type", "object"},
                       {"properties",
                        {{"customer_id", {{"type", "string"}}},
                         {"name", {{"type", "string"}}},
                         {"description", {{"type", "string"}}},
                         {"visibility",
                          {{"type", "string"},
                           {"enum", json::array({"private", "public"})}}}}},
                       {"required", json::array({"customer_id", "name"})},
                       {"additionalProperties", false}},
      .output_schema = repository_schema(),
      .kind = CapabilityKind::Action,
      .handler = [](const json &in, ApiContext &ctx) {
        if (!ctx.store)
          return no_store();
        return one_result(ctx.store->create_repository(control::NewRepository{
            in.value("customer_id", ""), in.value("name", ""),
            in.value("description", ""), in.value("visibility", "")}));
      }});

  // ---- search ----
  registry.add(Capability{
      .name = "search",
      .title = "Search repositories",
      .description = "Ranked BM25F search; quoted text is a substring query "
                     "and /expression/ is a regex query.",
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
      .handler = [](const json &in, ApiContext &ctx) {
        if (!ctx.search) {
          return no_search();
        }
        return search_result(ctx.search->query(
            search::Query{in.at("query").get<std::string>(),
                          in.value("customer", ""), in.value("repository", ""),
                          in.value("ref", ""), limit_of(in)},
            ctx.acting_identity));
      }});
}

} // namespace lake::api
