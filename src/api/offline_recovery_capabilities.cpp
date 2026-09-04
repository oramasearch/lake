#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <vector>

#include "lake/audit/audit.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable", "no control-plane store is configured");
}

json object_schema() {
    return {{"type", "object"}};
}

json list_schema() {
    return {{"type", "object"},
            {"properties", {{"items", {{"type", "array"}, {"items", object_schema()}}}}}};
}

std::expected<std::string, ApiError> self_object(const json&, ApiContext& context) {
    return "principal/" + context.acting_identity;
}

CapabilityObjectResolver entity_object(std::string object_type, std::string input_field) {
    return [object_type = std::move(object_type), input_field = std::move(input_field)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        if (!context.store) {
            return std::unexpected(
                ApiError{"store_unavailable", "no control-plane store is configured", {}});
        }
        const auto object = context.store->find_policy_object(object_type, input.at(input_field));
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/" + input_field});
        }
        return object->id;
    };
}

std::expected<std::string, ApiError> recovery_organization_object(const json& input,
                                                                  ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto attempt = context.store->get_recovery_attempt(input.at("recovery_id"));
    if (!attempt) {
        return std::unexpected(
            ApiError{attempt.error().code, attempt.error().message, "/recovery_id"});
    }
    if (attempt->method != "admin" || attempt->organization_id.empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "recovery is not organization-assisted", "/recovery_id"});
    }
    const auto object = context.store->find_policy_object("organization", attempt->organization_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, {}});
    }
    return object->id;
}

std::expected<std::string, ApiError> global_object(const json&, ApiContext&) {
    return "recovery delivery queue";
}

std::expected<std::string, ApiError> repository_id_from_scope(std::string_view scope) {
    constexpr std::string_view prefix = "repository:";
    constexpr std::string_view suffix = ":read";
    if (!scope.starts_with(prefix) || !scope.ends_with(suffix) ||
        scope.size() <= prefix.size() + suffix.size()) {
        return std::unexpected(
            ApiError{"invalid_input", "offline scopes must be repository:<id>:read", "/scopes"});
    }
    return std::string(scope.substr(prefix.size(), scope.size() - prefix.size() - suffix.size()));
}

std::expected<void, ApiError> authorize_offline_scopes(const std::vector<std::string>& scopes,
                                                       ApiContext& context) {
    if (scopes.empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "at least one offline read scope is required", "/scopes"});
    }
    for (const std::string& scope : scopes) {
        const auto repository_id = repository_id_from_scope(scope);
        if (!repository_id) {
            return std::unexpected(repository_id.error());
        }
        const auto object = context.store->find_policy_object("repository", *repository_id);
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/scopes"});
        }
        const auto decision = control::authorize(*context.store,
                                                 context.acting_identity,
                                                 object->id,
                                                 "read",
                                                 context.authorization_cache);
        if (!decision) {
            return std::unexpected(
                ApiError{decision.error().code, decision.error().message, "/scopes"});
        }
        if (!decision->allowed) {
            return std::unexpected(
                ApiError{"unauthorized", "principal cannot read the requested repository", {}});
        }
    }
    return {};
}

audit::IdentityOptions identity_options(const ApiContext& context) {
    return {.configured_seed = context.identity_seed, .instance_kek = context.instance_kek};
}

ApiResult issued_result(control::Result<control::IssuedSession> issued) {
    if (!issued) {
        return from_store_error(issued.error());
    }
    return ApiResult::success(
        {{"session", control::to_json(issued->session)}, {"secret", issued->secret}});
}

void register_offline_grant_issue(Registry& registry) {
    registry.add(Capability{
        .name = "issue_offline_grant",
        .title = "Issue offline read grant",
        .description = "Issue a lake-signed, device-bound cached-read grant with explicit scopes.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"device_id", {{"type", "string"}}},
                           {"scopes", {{"type", "array"}}},
                           {"duration_seconds", {{"type", "integer"}}}}},
                         {"required", json::array({"device_id", "scopes", "duration_seconds"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "admin",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            if (context.acting_user_id.empty() || context.acting_identity.empty()) {
                return ApiResult::failure("unauthorized", "a user session is required");
            }
            const int duration = input.at("duration_seconds").get<int>();
            if (duration <= 0 || duration > 604800) {
                return ApiResult::failure(
                    "invalid_input", "offline grant duration must be between 1 and 604800 seconds");
            }
            const auto scopes = input.at("scopes").get<std::vector<std::string>>();
            const auto authorized = authorize_offline_scopes(scopes, context);
            if (!authorized) {
                return ApiResult::failure(
                    authorized.error().code, authorized.error().message, authorized.error().path);
            }
            const std::string id = control::new_uuid_v4();
            const std::int64_t expires = std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count() +
                                         duration;
            const auto credential = audit::sign_offline_grant(*context.store,
                                                              id,
                                                              context.acting_identity,
                                                              input.at("device_id"),
                                                              scopes,
                                                              expires,
                                                              identity_options(context));
            if (!credential) {
                return ApiResult::failure(credential.error().code, credential.error().message);
            }
            const auto grant = context.store->issue_offline_grant({id,
                                                                   context.acting_user_id,
                                                                   context.acting_identity,
                                                                   input.at("device_id"),
                                                                   scopes,
                                                                   duration,
                                                                   *credential});
            return grant ? ApiResult::success(control::to_json(*grant))
                         : from_store_error(grant.error());
        }});
}

void register_offline_grant_management(Registry& registry) {
    registry.add(Capability{
        .name = "list_offline_grants",
        .title = "List offline grants",
        .description = "List cached-read grants issued to the authenticated user.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = list_schema(),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .authorizes_candidates = true,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto decision = control::authorize(*context.store,
                                                     context.acting_identity,
                                                     "principal/" + context.acting_identity,
                                                     "read",
                                                     context.authorization_cache);
            if (!decision || !decision->allowed) {
                return ApiResult::failure(decision ? "unauthorized" : decision.error().code,
                                          decision ? "principal cannot list offline grants"
                                                   : decision.error().message);
            }
            const auto rows = context.store->list_offline_grants(context.acting_user_id);
            if (!rows) {
                return from_store_error(rows.error());
            }
            json items = json::array();
            for (const control::OfflineGrant& row : *rows) {
                items.push_back(control::to_json(row));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
}

void register_cached_read_check(Registry& registry) {
    registry.add(Capability{
        .name = "revoke_offline_grant",
        .title = "Revoke offline grant",
        .description = "Prevent future refresh or online use of an offline grant.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"id", {{"type", "string"}}}}},
                         {"required", json::array({"id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "admin",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto grant =
                context.store->revoke_offline_grant(context.acting_user_id, input.at("id"));
            return grant ? ApiResult::success(control::to_json(*grant))
                         : from_store_error(grant.error());
        }});
    registry.add(Capability{
        .name = "check_cached_read",
        .title = "Check cached offline read",
        .description = "Verify a signed offline grant for a cached read at the current time.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"credential", {{"type", "object"}}}, {"scope", {{"type", "string"}}}}},
                         {"required", json::array({"credential", "scope"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "offline grant",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
            const auto verified = audit::verify_offline_grant(
                *context.store, input.at("credential"), input.at("scope"), now);
            return verified ? ApiResult::success({{"allowed", true}, {"cached", true}})
                            : ApiResult::failure(verified.error().code, verified.error().message);
        }});
}

void register_recovery_entry_points(Registry& registry) {
    registry.add(
        Capability{.name = "recover_with_code",
                   .title = "Recover with MFA code",
                   .description = "Consume one MFA recovery code and issue a browser session.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"pool_id", {{"type", "string"}}},
                                      {"email", {{"type", "string"}}},
                                      {"code", {{"type", "string"}}},
                                      {"device_name", {{"type", "string"}}}}},
                                    {"required", json::array({"email", "code"})},
                                    {"additionalProperties", false}},
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Action,
                   .requires_auth = false,
                   .object = "authentication",
                   .action = "write",
                   .resolve_object = global_object,
                   .handler = [](const json& input, ApiContext& context) {
                       return context.store ? issued_result(context.store->recover_with_code(
                                                  input.value("pool_id", "lake"),
                                                  input.at("email"),
                                                  input.at("code"),
                                                  input.value("device_name", "recovered device")))
                                            : no_store();
                   }});
    registry.add(Capability{
        .name = "begin_email_recovery",
        .title = "Begin email recovery",
        .description = "Queue a one-use recovery link subject to the 24-hour cooling period.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}}, {"email", {{"type", "string"}}}}},
                         {"required", json::array({"email"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto attempt = context.store->begin_email_recovery(
                input.value("pool_id", "lake"), input.at("email"), 24 * 60 * 60);
            return attempt ? ApiResult::success(control::to_json(*attempt))
                           : from_store_error(attempt.error());
        }});
}

void register_admin_recovery(Registry& registry) {
    registry.add(Capability{
        .name = "begin_admin_recovery",
        .title = "Begin organization recovery",
        .description = "Open an organization recovery with the requesting admin's first approval.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"user_id", {{"type", "string"}}}}},
                         {"required", json::array({"organization_id", "user_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto attempt = context.store->begin_admin_recovery(
                input.at("organization_id"), input.at("user_id"), context.acting_identity);
            return attempt ? ApiResult::success(control::to_json(*attempt))
                           : from_store_error(attempt.error());
        }});
    registry.add(
        Capability{.name = "approve_admin_recovery",
                   .title = "Approve organization recovery",
                   .description = "Add the required approval from a distinct organization admin.",
                   .input_schema = {{"type", "object"},
                                    {"properties", {{"recovery_id", {{"type", "string"}}}}},
                                    {"required", json::array({"recovery_id"})},
                                    {"additionalProperties", false}},
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "organization recovery",
                   .action = "admin",
                   .resolve_object = recovery_organization_object,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.store) {
                           return no_store();
                       }
                       const auto attempt = context.store->approve_admin_recovery(
                           input.at("recovery_id"), context.acting_identity);
                       return attempt ? ApiResult::success(control::to_json(*attempt))
                                      : from_store_error(attempt.error());
                   }});
}

void register_recovery_completion_and_delivery(Registry& registry) {
    registry.add(Capability{
        .name = "complete_recovery",
        .title = "Complete account recovery",
        .description = "Consume an approved one-use recovery token and issue a browser session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"recovery_id", {{"type", "string"}}},
                           {"token", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}}}},
                         {"required", json::array({"recovery_id", "token"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? issued_result(context.store->complete_recovery(
                                       input.at("recovery_id"),
                                       input.at("token"),
                                       input.value("device_name", "recovered device")))
                                 : no_store();
        }});
}

void register_recovery_notifications(Registry& registry) {
    registry.add(Capability{
        .name = "claim_recovery_deliveries",
        .title = "Claim recovery email deliveries",
        .description = "Atomically claim queued one-use links for the configured mail worker.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"limit", {{"type", "integer"}}}}},
                         {"additionalProperties", false}},
        .output_schema = list_schema(),
        .kind = CapabilityKind::Action,
        .object = "recovery delivery queue",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            if (!context.operator_credential) {
                return ApiResult::failure("unauthorized", "operator credential is required");
            }
            const int limit = std::clamp(input.value("limit", 100), 1, 1000);
            const auto rows = context.store->claim_recovery_deliveries(limit);
            if (!rows) {
                return from_store_error(rows.error());
            }
            json items = json::array();
            for (const control::RecoveryDelivery& row : *rows) {
                items.push_back(control::to_json(row));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "list_recovery_notifications",
        .title = "List recovery notifications",
        .description = "List recovery notices addressed to every registered device.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = list_schema(),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .authorizes_candidates = true,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto decision = control::authorize(*context.store,
                                                     context.acting_identity,
                                                     "principal/" + context.acting_identity,
                                                     "read",
                                                     context.authorization_cache);
            if (!decision || !decision->allowed) {
                return ApiResult::failure(decision ? "unauthorized" : decision.error().code,
                                          decision ? "principal cannot list recovery notifications"
                                                   : decision.error().message);
            }
            const auto rows = context.store->list_recovery_notifications(context.acting_user_id);
            if (!rows) {
                return from_store_error(rows.error());
            }
            json items = json::array();
            for (const control::RecoveryNotification& row : *rows) {
                items.push_back(control::to_json(row));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
}

void register_recovery_contact_configuration(Registry& registry) {
    registry.add(Capability{
        .name = "configure_recovery_contact",
        .title = "Configure repository recovery contact",
        .description = "Store a trust-domain key wrapped to a designated recovery contact.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"contact_taap_id", {{"type", "string"}}},
                           {"wrapped_key", {{"type", "string"}}}}},
                         {"required",
                          json::array({"repository_id", "contact_taap_id", "wrapped_key"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "admin",
        .resolve_object = entity_object("repository", "repository_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto contact = context.store->configure_recovery_contact(
                {input.at("repository_id"), input.at("contact_taap_id"), input.at("wrapped_key")});
            return contact ? ApiResult::success(control::to_json(*contact))
                           : from_store_error(contact.error());
        }});
}

void register_repository_key_recovery(Registry& registry) {
    registry.add(Capability{
        .name = "repository_recovery_status",
        .title = "Repository recovery status",
        .description = "Report whether an encrypted repository has advance key custody.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"repository_id", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "repository_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto contact = context.store->get_recovery_contact(input.at("repository_id"));
            if (!contact && contact.error().code == "not_found") {
                return ApiResult::success(
                    {{"recoverable", false},
                     {"reason",
                      "account recovery does not restore repository keys; no recovery contact was "
                      "configured, so a key held only on the lost device is unrecoverable"}});
            }
            if (!contact) {
                return from_store_error(contact.error());
            }
            json value = control::to_json(*contact);
            value["recoverable"] = true;
            value["reason"] = "the designated contact holds the wrapped trust-domain key";
            return ApiResult::success(std::move(value));
        }});
    registry.add(Capability{
        .name = "recover_repository_key",
        .title = "Recover repository key",
        .description = "Return the stored wrapped key to the configured recovery contact.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"repository_id", {{"type", "string"}}}}},
                         {"required", json::array({"repository_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository recovery contact",
        .action = "read",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto contact = context.store->get_recovery_contact(input.at("repository_id"));
            if (!contact) {
                return ApiResult::failure(
                    "unrecoverable",
                    "repository key is unrecoverable because no recovery contact was configured");
            }
            if (context.acting_identity != contact->contact_taap_id &&
                !context.operator_credential) {
                return ApiResult::failure(
                    "unauthorized", "only the designated recovery contact may retrieve the key");
            }
            json value = control::to_json(*contact, true);
            value["recovered"] = true;
            return ApiResult::success(std::move(value));
        }});
}

} // namespace

void register_offline_recovery_capabilities(Registry& registry) {
    register_offline_grant_issue(registry);
    register_offline_grant_management(registry);
    register_cached_read_check(registry);
    register_recovery_entry_points(registry);
    register_admin_recovery(registry);
    register_recovery_completion_and_delivery(registry);
    register_recovery_notifications(registry);
    register_recovery_contact_configuration(registry);
    register_repository_key_recovery(registry);
}

} // namespace lake::api
