// Secure-networking capabilities (lake#728, docs/api/network.md). The desired rows and operator
// observations are the 0084_network control-plane contract. Tunnel sessions and activity remain
// workspace records; until lake#730 and zetta#1281 provide them, their registered reads answer
// not_supported after the ordinary account permission check.

#include "lake/api/capabilities.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lake/control/network.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

json string_schema() {
    return {{"type", "string"}};
}

json nullable_schema(std::string_view type) {
    return {{"type", json::array({std::string(type), "null"})}};
}

json workspace_input(json properties, json required) {
    properties["customer_id"] = string_schema();
    properties["workspace_id"] = string_schema();
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json status_schema(std::string_view extra = {}) {
    json properties = {{"phase", string_schema()},
                       {"message", string_schema()},
                       {"observed_generation", {{"type", "integer"}}},
                       {"updated_at", string_schema()}};
    json required = json::array({"phase", "message", "observed_generation", "updated_at"});
    if (!extra.empty()) {
        properties[std::string(extra)] = string_schema();
        required.push_back(std::string(extra));
    }
    return {{"type", json::array({"object", "null"})},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json record_schema(json properties, json required, std::string_view status_extra = {}) {
    properties["id"] = string_schema();
    properties["customer_id"] = string_schema();
    properties["workspace_id"] = string_schema();
    properties["generation"] = {{"type", "integer"}};
    properties["created_by"] = nullable_schema("string");
    properties["created_at"] = string_schema();
    properties["updated_at"] = string_schema();
    properties["status"] = status_schema(status_extra);
    for (const char* name : {"id",
                             "customer_id",
                             "workspace_id",
                             "generation",
                             "created_by",
                             "created_at",
                             "updated_at",
                             "status"}) {
        required.push_back(name);
    }
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json credential_scope_schema() {
    return {{"type", json::array({"object", "null"})},
            {"properties",
             {{"access", {{"type", "string"}, {"enum", {"read", "write"}}}},
              {"schema", string_schema()}}},
            {"required", json::array({"access"})},
            {"additionalProperties", false}};
}

json connection_schema() {
    return record_schema(
        {{"source_kind", {{"type", "string"}, {"enum", {"app", "service", "agent", "tunnel"}}}},
         {"source_id", string_schema()},
         {"target_kind", {{"type", "string"}, {"enum", {"service", "app"}}}},
         {"target_id", string_schema()},
         {"ports",
          {{"type", "array"},
           {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}},
         {"credential_scope", credential_scope_schema()},
         {"desired", {{"type", "string"}, {"enum", {"present", "absent"}}}}},
        json::array({"source_kind",
                     "source_id",
                     "target_kind",
                     "target_id",
                     "ports",
                     "credential_scope",
                     "desired"}),
        "credential_secret");
}

json egress_schema() {
    return record_schema(
        {{"source_kind", nullable_schema("string")},
         {"source_id", nullable_schema("string")},
         {"cidr", nullable_schema("string")},
         {"dns_name", nullable_schema("string")},
         {"ports",
          {{"type", "array"},
           {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}},
         {"desired", {{"type", "string"}, {"enum", {"present", "absent"}}}}},
        json::array({"source_kind", "source_id", "cidr", "dns_name", "ports", "desired"}));
}

json tunnel_schema() {
    return record_schema(
        {{"user_id", string_schema()},
         {"target_kind", {{"type", "string"}, {"enum", {"service", "app"}}}},
         {"target_id", string_schema()},
         {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
         {"expires_at", string_schema()},
         {"revoked_at", nullable_schema("string")},
         {"active", {{"type", "boolean"}}}},
        json::array(
            {"user_id", "target_kind", "target_id", "port", "expires_at", "revoked_at", "active"}),
        "endpoint");
}

json peer_export_schema() {
    return {{"type", "object"},
            {"properties",
             {{"target_kind", {{"type", "string"}, {"enum", {"service", "app"}}}},
              {"target_id", string_schema()},
              {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}},
            {"required", json::array({"target_kind", "target_id", "port"})},
            {"additionalProperties", false}};
}

json peer_import_schema() {
    return {{"type", "object"},
            {"properties",
             {{"name", string_schema()},
              {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}},
            {"required", json::array({"name", "port"})},
            {"additionalProperties", false}};
}

json peer_schema() {
    return record_schema({{"name", string_schema()},
                          {"remote", string_schema()},
                          {"exports", {{"type", "array"}, {"items", peer_export_schema()}}},
                          {"imports", {{"type", "array"}, {"items", peer_import_schema()}}},
                          {"desired", {{"type", "string"}, {"enum", {"present", "absent"}}}}},
                         json::array({"name", "remote", "exports", "imports", "desired"}));
}

json optional_text(const std::optional<std::string>& value) {
    return value ? json(*value) : json(nullptr);
}

json status_json(const std::optional<control::NetworkStatus>& status, std::string_view extra = {}) {
    if (!status) {
        return nullptr;
    }
    json value = {{"phase", status->phase},
                  {"message", status->message},
                  {"observed_generation", status->observed_generation},
                  {"updated_at", status->updated_at}};
    if (extra == "credential_secret") {
        value["credential_secret"] = status->credential_secret;
    } else if (extra == "endpoint") {
        value["endpoint"] = status->endpoint;
    }
    return value;
}

json metadata_json(const control::NetworkMetadata& record, std::string_view status_extra = {}) {
    return {{"id", record.id},
            {"customer_id", record.customer_id},
            {"workspace_id", record.workspace_id},
            {"generation", record.generation},
            {"created_by", optional_text(record.created_by)},
            {"created_at", record.created_at},
            {"updated_at", record.updated_at},
            {"status", status_json(record.status, status_extra)}};
}

json connection_json(const control::NetworkConnection& record) {
    json value = metadata_json(record, "credential_secret");
    value.update({{"source_kind", record.spec.source_kind},
                  {"source_id", record.spec.source_id},
                  {"target_kind", record.spec.target_kind},
                  {"target_id", record.spec.target_id},
                  {"ports", record.spec.ports},
                  {"credential_scope", record.spec.credential_scope},
                  {"desired", record.desired}});
    return value;
}

json egress_json(const control::NetworkEgressRule& record) {
    json value = metadata_json(record);
    value.update({{"source_kind", optional_text(record.spec.source_kind)},
                  {"source_id", optional_text(record.spec.source_id)},
                  {"cidr", optional_text(record.spec.cidr)},
                  {"dns_name", optional_text(record.spec.dns_name)},
                  {"ports", record.spec.ports},
                  {"desired", record.desired}});
    return value;
}

json tunnel_json(const control::NetworkTunnelGrant& record) {
    json value = metadata_json(record, "endpoint");
    value.update({{"user_id", record.spec.user_id},
                  {"target_kind", record.spec.target_kind},
                  {"target_id", record.spec.target_id},
                  {"port", record.spec.port},
                  {"expires_at", record.spec.expires_at},
                  {"revoked_at", optional_text(record.revoked_at)},
                  {"active", control::network_tunnel_grant_active(record)}});
    return value;
}

json peer_json(const control::NetworkPeer& record) {
    json value = metadata_json(record);
    value.update({{"name", record.spec.name},
                  {"remote", record.spec.remote},
                  {"exports", record.spec.exports},
                  {"imports", record.spec.imports},
                  {"desired", record.desired}});
    return value;
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

ApiResult store_failure(const control::StoreError& error, std::string path = {}) {
    if (error.code == "store_error") {
        return ApiResult::failure("store_unavailable", "the control-plane store did not answer");
    }
    return ApiResult::failure(error.code, error.message, std::move(path));
}

std::optional<ApiResult> authorize_list(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read customer");
    }
    return std::nullopt;
}

control::NetworkScope network_scope(const json& input) {
    return {input.at("customer_id"), input.at("workspace_id")};
}

control::NetworkActor network_actor(const ApiContext& context) {
    const bool system = context.operator_credential || context.acting_identity == "admin";
    return {.user_id = system ? std::string() : context.acting_user_id,
            .principal = context.acting_identity.empty() ? "admin" : context.acting_identity,
            .request_id = context.request_id,
            .identity_options = {.configured_seed = context.identity_seed,
                                 .instance_kek = context.instance_kek}};
}

void set_audit_target(ApiContext& context, const control::NetworkScope& scope, std::string object) {
    context.audit_customer_id = scope.customer_id;
    context.audit_object = std::move(object);
    context.audit_details = {{"workspace_id", scope.workspace_id}};
}

template <class Record, class Encode>
ApiResult list_result(const control::Result<std::vector<Record>>& records,
                      std::string_view field,
                      Encode encode) {
    if (!records) {
        return store_failure(records.error());
    }
    json values = json::array();
    for (const Record& record : *records) {
        values.push_back(encode(record));
    }
    return ApiResult::success({{std::string(field), std::move(values)}});
}

ApiResult create_connection(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_connection/new");
    control::NetworkConnectionSpec spec{.source_kind = input.at("source_kind"),
                                        .source_id = input.at("source_id"),
                                        .target_kind = input.at("target_kind"),
                                        .target_id = input.at("target_id"),
                                        .ports = input.value("ports", std::vector<int>{}),
                                        .credential_scope =
                                            input.value("credential_scope", json(nullptr))};
    const auto created =
        context.store->create_network_connection(scope, spec, network_actor(context));
    if (!created) {
        return store_failure(created.error());
    }
    context.audit_object = "network_connection/" + created->id;
    return ApiResult::success(connection_json(*created));
}

ApiResult list_connections(const json& input, ApiContext& context) {
    if (const auto refusal = authorize_list(input, context)) {
        return *refusal;
    }
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_connections/" + scope.workspace_id);
    return list_result(
        context.store->list_network_connections(scope), "connections", connection_json);
}

ApiResult remove_connection(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    const std::string id = input.at("connection_id");
    set_audit_target(context, scope, "network_connection/" + id);
    const auto removed =
        context.store->withdraw_network_connection(scope, id, network_actor(context));
    if (!removed) {
        return store_failure(removed.error(), "/connection_id");
    }
    return ApiResult::success(connection_json(*removed));
}

ApiResult create_egress_rule(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_egress_rule/new");
    control::NetworkEgressRuleSpec spec;
    if (input.contains("source_kind")) {
        spec.source_kind = input.at("source_kind").get<std::string>();
    }
    if (input.contains("source_id")) {
        spec.source_id = input.at("source_id").get<std::string>();
    }
    if (input.contains("cidr")) {
        spec.cidr = input.at("cidr").get<std::string>();
    }
    if (input.contains("dns_name")) {
        spec.dns_name = input.at("dns_name").get<std::string>();
    }
    spec.ports = input.at("ports").get<std::vector<int>>();
    const auto created =
        context.store->create_network_egress_rule(scope, spec, network_actor(context));
    if (!created) {
        return store_failure(created.error());
    }
    context.audit_object = "network_egress_rule/" + created->id;
    return ApiResult::success(egress_json(*created));
}

ApiResult list_egress_rules(const json& input, ApiContext& context) {
    if (const auto refusal = authorize_list(input, context)) {
        return *refusal;
    }
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_egress_rules/" + scope.workspace_id);
    return list_result(
        context.store->list_network_egress_rules(scope), "egress_rules", egress_json);
}

std::optional<ApiResult>
tunnel_user_problem(const std::string& user_id, const json& input, ApiContext& context) {
    const auto user = context.store->get_user(user_id);
    if (!user) {
        return store_failure(user.error(), "/user_id");
    }
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    const auto decision = control::authorize(
        *context.store, user->taap_id, *object, "read", context.authorization_cache);
    if (!decision) {
        return store_failure(decision.error());
    }
    if (!decision->allowed) {
        return ApiResult::failure(
            "invalid_input", "user_id must be a member of the workspace's account", "/user_id");
    }
    return std::nullopt;
}

ApiResult grant_tunnel(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_tunnel_grant/new");
    const std::string user_id = input.at("user_id");
    if (auto problem = tunnel_user_problem(user_id, input, context)) {
        return *problem;
    }
    control::NetworkTunnelGrantSpec spec{.user_id = user_id,
                                         .target_kind = input.at("target_kind"),
                                         .target_id = input.at("target_id"),
                                         .port = input.at("port"),
                                         .expires_at = input.at("expires_at")};
    const auto granted =
        context.store->create_network_tunnel_grant(scope, spec, network_actor(context));
    if (!granted) {
        return store_failure(granted.error());
    }
    context.audit_object = "network_tunnel_grant/" + granted->id;
    return ApiResult::success(tunnel_json(*granted));
}

ApiResult list_tunnel_grants(const json& input, ApiContext& context) {
    if (const auto refusal = authorize_list(input, context)) {
        return *refusal;
    }
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_tunnel_grants/" + scope.workspace_id);
    return list_result(
        context.store->list_network_tunnel_grants(scope), "tunnel_grants", tunnel_json);
}

ApiResult revoke_tunnel(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    const std::string id = input.at("tunnel_grant_id");
    set_audit_target(context, scope, "network_tunnel_grant/" + id);
    const auto revoked =
        context.store->revoke_network_tunnel_grant(scope, id, network_actor(context));
    if (!revoked) {
        return store_failure(revoked.error(), "/tunnel_grant_id");
    }
    return ApiResult::success(tunnel_json(*revoked));
}

ApiResult list_peers(const json& input, ApiContext& context) {
    if (const auto refusal = authorize_list(input, context)) {
        return *refusal;
    }
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_peers/" + scope.workspace_id);
    return list_result(context.store->list_network_peers(scope), "peers", peer_json);
}

ApiResult effective_rules(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_rules/" + scope.workspace_id);
    const auto connections = context.store->list_network_connections(scope);
    const auto egress = context.store->list_network_egress_rules(scope);
    const auto tunnels = context.store->list_network_tunnel_grants(scope);
    const auto peers = context.store->list_network_peers(scope);
    if (!connections) {
        return store_failure(connections.error());
    }
    if (!egress) {
        return store_failure(egress.error());
    }
    if (!tunnels) {
        return store_failure(tunnels.error());
    }
    if (!peers) {
        return store_failure(peers.error());
    }
    json connection_values = json::array();
    json egress_values = json::array();
    json tunnel_values = json::array();
    json peer_values = json::array();
    for (const auto& record : *connections) {
        if (record.desired == "present") {
            connection_values.push_back(connection_json(record));
        }
    }
    for (const auto& record : *egress) {
        if (record.desired == "present") {
            egress_values.push_back(egress_json(record));
        }
    }
    for (const auto& record : *tunnels) {
        if (control::network_tunnel_grant_active(record)) {
            tunnel_values.push_back(tunnel_json(record));
        }
    }
    for (const auto& record : *peers) {
        if (record.desired == "present") {
            peer_values.push_back(peer_json(record));
        }
    }
    return ApiResult::success({{"connections", std::move(connection_values)},
                               {"egress_rules", std::move(egress_values)},
                               {"tunnel_grants", std::move(tunnel_values)},
                               {"peers", std::move(peer_values)}});
}

ApiResult unsupported_records(std::string kind) {
    return ApiResult::failure(
        "not_supported", "workspace network " + kind + " records are not available on this Lake");
}

ApiResult list_tunnel_sessions(const json& input, ApiContext& context) {
    if (const auto refusal = authorize_list(input, context)) {
        return *refusal;
    }
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_tunnel_sessions/" + scope.workspace_id);
    return unsupported_records("tunnel session");
}

ApiResult recent_activity(const json& input, ApiContext& context) {
    const control::NetworkScope scope = network_scope(input);
    set_audit_target(context, scope, "network_activity/" + scope.workspace_id);
    return unsupported_records("activity");
}

json array_output(std::string_view field, json item) {
    const std::string name(field);
    return {{"type", "object"},
            {"properties", {{name, {{"type", "array"}, {"items", std::move(item)}}}}},
            {"required", json::array({name})},
            {"additionalProperties", false}};
}

json unavailable_array_output(std::string_view field) {
    json schema = array_output(field, {{"type", "object"}});
    schema.erase("required");
    return schema;
}

void register_connection_capabilities(Registry& registry) {
    const json source_kind{{"type", "string"}, {"enum", {"app", "service", "agent", "tunnel"}}};
    const json target_kind{{"type", "string"}, {"enum", {"service", "app"}}};
    const json ports{{"type", "array"},
                     {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}};
    const json scope_input = workspace_input(json::object(), json::array());

    registry.add(Capability{
        .name = "create_network_connection",
        .title = "Create a Network Connection",
        .description =
            "Declare one path from an app, service, agent or tunnel to one app or service in the "
            "same workspace. Empty ports use the target's service ports. credential_scope is "
            "null or read/write with an optional schema; it never contains credential bytes.",
        .input_schema =
            workspace_input({{"source_kind", source_kind},
                             {"source_id", string_schema()},
                             {"target_kind", target_kind},
                             {"target_id", string_schema()},
                             {"ports", ports},
                             {"credential_scope", credential_scope_schema()}},
                            json::array({"source_kind", "source_id", "target_kind", "target_id"})),
        .output_schema = connection_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = create_connection});
    registry.add(Capability{
        .name = "list_network_connections",
        .title = "List Network Connections",
        .description = "List the workspace's declared network connections, including withdrawn "
                       "history and the operator's latest observation.",
        .input_schema = scope_input,
        .output_schema = array_output("connections", connection_schema()),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = list_connections});
    registry.add(Capability{
        .name = "remove_network_connection",
        .title = "Remove a Network Connection",
        .description = "Withdraw a declared connection. The operator removes what it made and "
                       "reports Deleted; the retained row remains in list_network_connections.",
        .input_schema =
            workspace_input({{"connection_id", string_schema()}}, json::array({"connection_id"})),
        .output_schema = connection_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = remove_connection});
}

void register_egress_capabilities(Registry& registry) {
    const json egress_source_kind{{"type", "string"}, {"enum", {"app", "service", "agent"}}};
    const json ports{{"type", "array"},
                     {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}};
    const json scope_input = workspace_input(json::object(), json::array());

    registry.add(Capability{
        .name = "create_network_egress_rule",
        .title = "Create a Network Egress Rule",
        .description =
            "Declare outbound access by one CIDR or DNS name and ports. Omit source_kind and "
            "source_id together for every workload in the workspace; otherwise send both.",
        .input_schema = workspace_input({{"source_kind", egress_source_kind},
                                         {"source_id", string_schema()},
                                         {"cidr", string_schema()},
                                         {"dns_name", string_schema()},
                                         {"ports", ports}},
                                        json::array({"ports"})),
        .output_schema = egress_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = create_egress_rule});
    registry.add(Capability{
        .name = "list_network_egress_rules",
        .title = "List Network Egress Rules",
        .description = "List the workspace's declared egress rules, including withdrawn history "
                       "and the operator's latest observation.",
        .input_schema = scope_input,
        .output_schema = array_output("egress_rules", egress_schema()),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = list_egress_rules});
}

void register_tunnel_capabilities(Registry& registry) {
    const json target_kind{{"type", "string"}, {"enum", {"service", "app"}}};
    const json scope_input = workspace_input(json::object(), json::array());

    registry.add(Capability{
        .name = "grant_network_tunnel",
        .title = "Grant a Network Tunnel",
        .description = "Grant one account member access to one app or service port until a future "
                       "UTC RFC 3339 instant. The grant names no tunnel provider.",
        .input_schema = workspace_input(
            {{"user_id", string_schema()},
             {"target_kind", target_kind},
             {"target_id", string_schema()},
             {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
             {"expires_at", string_schema()}},
            json::array({"user_id", "target_kind", "target_id", "port", "expires_at"})),
        .output_schema = tunnel_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = grant_tunnel});
    registry.add(Capability{
        .name = "list_network_tunnel_grants",
        .title = "List Network Tunnel Grants",
        .description = "List the workspace's tunnel grants, including expired and revoked history, "
                       "whether each is active, and the operator's latest observation.",
        .input_schema = scope_input,
        .output_schema = array_output("tunnel_grants", tunnel_schema()),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = list_tunnel_grants});
    registry.add(Capability{
        .name = "revoke_network_tunnel",
        .title = "Revoke a Network Tunnel",
        .description = "Revoke a tunnel grant. A repeated revocation or an already expired grant "
                       "is a conflict; ending live sessions belongs to the tunnel flow.",
        .input_schema = workspace_input({{"tunnel_grant_id", string_schema()}},
                                        json::array({"tunnel_grant_id"})),
        .output_schema = tunnel_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .handler = revoke_tunnel});
    registry.add(Capability{
        .name = "list_network_tunnel_sessions",
        .title = "List Network Tunnel Sessions",
        .description = "List the workspace's current and past tunnel sessions. Until the "
                       "workspace network-record contract is available, this read answers "
                       "not_supported after permission checks.",
        .input_schema = workspace_input(
            {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}}, json::array()),
        .output_schema = unavailable_array_output("sessions"),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = list_tunnel_sessions});
}

void register_network_read_capabilities(Registry& registry) {
    const json scope_input = workspace_input(json::object(), json::array());

    registry.add(Capability{
        .name = "list_network_peers",
        .title = "List Network Peers",
        .description = "List the workspace's peer connections, named exported and imported "
                       "services, retained withdrawals, and the operator's latest observation.",
        .input_schema = scope_input,
        .output_schema = array_output("peers", peer_schema()),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .handler = list_peers});
    registry.add(Capability{
        .name = "network_rules",
        .title = "Network Rules",
        .description = "The workspace's effective declared rules: present connections, egress "
                       "rules and peers, plus unexpired and unrevoked tunnel grants. Each carries "
                       "the operator's latest observation when one exists.",
        .input_schema = scope_input,
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"connections", {{"type", "array"}, {"items", connection_schema()}}},
                            {"egress_rules", {{"type", "array"}, {"items", egress_schema()}}},
                            {"tunnel_grants", {{"type", "array"}, {"items", tunnel_schema()}}},
                            {"peers", {{"type", "array"}, {"items", peer_schema()}}}}},
                          {"required",
                           json::array({"connections", "egress_rules", "tunnel_grants", "peers"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .handler = effective_rules});
    registry.add(Capability{
        .name = "network_activity",
        .title = "Network Activity",
        .description = "The workspace's recent network decisions. Until the workspace "
                       "network-record contract is available, this read answers not_supported "
                       "after permission checks.",
        .input_schema = workspace_input(
            {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}}, json::array()),
        .output_schema = unavailable_array_output("events"),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .handler = recent_activity});
}

} // namespace

void register_network_capabilities(Registry& registry) {
    register_connection_capabilities(registry);
    register_egress_capabilities(registry);
    register_tunnel_capabilities(registry);
    register_network_read_capabilities(registry);
}

} // namespace lake::api
