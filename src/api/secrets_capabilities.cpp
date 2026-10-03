#include "lake/api/capabilities.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "lake/control/identity_crypto.hpp"
#include "lake/control/store.hpp"

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

std::expected<std::string, ApiError> organization_object(const json& input, ApiContext& context) {
    context.audit_customer_id = input.at("customer_id");
    context.audit_kind = "secret_mount";
    context.audit_object = "secret_mount/" + input.at("customer_id").get<std::string>() + "/" +
                           input.at("organization_id").get<std::string>() + "/" +
                           input.at("name").get<std::string>();
    context.audit_details = {{"mount", input.at("name")}};
    auto object = context.store->find_policy_object("organization", input.at("organization_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/organization_id"});
    }
    return object->id;
}

CapabilityObjectResolver mount_object(std::string audit_kind) {
    return [audit_kind = std::move(audit_kind)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        const std::string mount_id = input.at("mount_id");
        auto mount = context.store->get_secret_mount(mount_id);
        if (!mount) {
            return std::unexpected(
                ApiError{mount.error().code, mount.error().message, "/mount_id"});
        }
        context.audit_customer_id = mount->customer_id;
        context.audit_kind = audit_kind;
        context.audit_object = "secret_mount/" + mount_id;
        context.audit_details = {{"mount_id", mount_id}};
        if (input.contains("name")) {
            context.audit_details["key"] = input.at("name");
        }
        if (input.contains("version")) {
            context.audit_details["version"] = input.at("version");
        } else if (audit_kind == "secret" && input.contains("name")) {
            auto metadata = context.store->secret_metadata(mount_id, input.at("name"));
            if (metadata) {
                context.audit_details["version"] = metadata->current_version;
            }
        } else if (audit_kind == "encryption") {
            const std::string key_name = input.contains("parent_name")
                                             ? input.at("parent_name").get<std::string>()
                                             : input.value("name", "");
            if (!key_name.empty()) {
                auto key = context.store->get_encryption_key(mount_id, key_name);
                if (key) {
                    context.audit_details["version"] = key->current_version;
                }
            }
        }
        auto object = context.store->find_policy_object("secret_mount", mount_id);
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/mount_id"});
        }
        return object->id;
    };
}

std::expected<std::string, ApiError> dynamic_lease_object(const json& input, ApiContext& context) {
    auto lease = context.store->get_dynamic_secret_lease(input.at("lease_id"));
    if (!lease) {
        return std::unexpected(ApiError{lease.error().code, lease.error().message, "/lease_id"});
    }
    json normalized = {{"mount_id", lease->mount_id}};
    auto object = mount_object("dynamic_secret")(normalized, context);
    context.audit_object = "dynamic_secret/" + lease->id;
    context.audit_details["lease_id"] = lease->id;
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

template <class T> ApiResult one_result(control::Result<T> row) {
    if (!row) {
        return from_store_error(row.error());
    }
    return ApiResult::success(control::to_json(*row));
}

void register_mount_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_secret_mount",
        .title = "Create secret mount",
        .description = "Create an organization secret mount inside a customer namespace.",
        .input_schema = input_schema({{"customer_id", {{"type", "string"}}},
                                      {"organization_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"description", {{"type", "string"}}}},
                                     json::array({"customer_id", "organization_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = organization_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto result = context.store->create_secret_mount({input.at("customer_id"),
                                                              input.at("organization_id"),
                                                              input.at("name"),
                                                              input.value("description", ""),
                                                              context.acting_identity,
                                                              context.request_id},
                                                             context.instance_kek);
            if (result) {
                context.audit_object = "secret_mount/" + result->id;
                context.audit_details["mount_id"] = result->id;
            }
            return one_result(std::move(result));
        }});
    registry.add(Capability{
        .name = "list_secret_mounts",
        .title = "List secret mounts",
        .description =
            "List the secret mounts you may read in a space, to find where a secret lives.",
        .input_schema = input_schema({{"customer_id", {{"type", "string"}}},
                                      {"organization_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = candidate_list_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            context.audit_customer_id = input.value("customer_id", "");
            context.audit_kind = "secret_mount";
            context.audit_object = "secret_mounts";
            auto rows = context.store->list_secret_mounts(input.value("customer_id", ""),
                                                          input.value("organization_id", ""),
                                                          limit_of(input));
            if (!rows) {
                return from_store_error(rows.error());
            }
            std::vector<std::string> object_ids;
            std::vector<control::PolicyObject> objects;
            for (const auto& row : *rows) {
                auto object = context.store->find_policy_object("secret_mount", row.id);
                if (!object) {
                    return from_store_error(object.error());
                }
                object_ids.push_back(object->id);
                objects.push_back(*object);
            }
            std::map<std::string, control::AuthorizationDecision> decisions;
            if (context.acting_identity != "admin") {
                auto authorized = control::authorize_batch(*context.store,
                                                           context.acting_identity,
                                                           object_ids,
                                                           "read",
                                                           context.authorization_cache);
                if (!authorized) {
                    return from_store_error(authorized.error());
                }
                decisions = std::move(*authorized);
            }
            json items = json::array();
            for (std::size_t index = 0; index < rows->size(); ++index) {
                if (context.acting_identity == "admin" || decisions.at(objects[index].id).allowed) {
                    items.push_back(control::to_json(rows->at(index)));
                }
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "get_secret_mount",
        .title = "Get secret mount",
        .description = "Read one secret mount's settings, never the secrets stored in it.",
        .input_schema =
            input_schema({{"mount_id", {{"type", "string"}}}}, json::array({"mount_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("secret_mount"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? one_result(context.store->get_secret_mount(input.at("mount_id")))
                                 : no_store();
        }});
}

void register_static_secret_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "put_secret",
        .title = "Put secret",
        .description = "Create a new immutable version of a key-value secret.",
        .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"value", {{"type", "object"}}},
                                      {"custom_metadata", {{"type", "object"}}},
                                      {"check_and_set", {{"type", "integer"}}}},
                                     json::array({"mount_id", "name", "value"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("secret"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto check = optional_version(input, "check_and_set");
            if (!check) {
                return ApiResult::failure(
                    check.error().code, check.error().message, check.error().path);
            }
            auto result = context.store->put_secret({input.at("mount_id"),
                                                     input.at("name"),
                                                     input.at("value"),
                                                     input.value("custom_metadata", json::object()),
                                                     *check},
                                                    context.instance_kek);
            if (result) {
                context.audit_details["version"] = result->version;
            }
            return one_result(std::move(result));
        }});
    registry.add(
        Capability{.name = "read_secret",
                   .title = "Read secret",
                   .description = "Read the current or an explicit live secret version.",
                   .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                                 {"name", {{"type", "string"}}},
                                                 {"version", {{"type", "integer"}}}},
                                                json::array({"mount_id", "name"})),
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "secret_mount",
                   .action = "read",
                   .resolve_object = mount_object("secret"),
                   .resolves_operator_audit_target = true,
                   .handler = [](const json& input, ApiContext& context) {
                       if (!context.store) {
                           return no_store();
                       }
                       auto version = optional_version(input);
                       if (!version) {
                           return ApiResult::failure(
                               version.error().code, version.error().message, version.error().path);
                       }
                       auto result = context.store->read_secret(
                           input.at("mount_id"), input.at("name"), *version, context.instance_kek);
                       if (result) {
                           context.audit_details["version"] = result->version;
                       }
                       return one_result(std::move(result));
                   }});
    registry.add(Capability{
        .name = "secret_metadata",
        .title = "Read secret metadata",
        .description = "Read custom metadata and the complete version history without values.",
        .input_schema =
            input_schema({{"mount_id", {{"type", "string"}}}, {"name", {{"type", "string"}}}},
                         json::array({"mount_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("secret"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? one_result(context.store->secret_metadata(input.at("mount_id"),
                                                                             input.at("name")))
                                 : no_store();
        }});
    const auto add_version_action = [&registry](std::string name,
                                                std::string title,
                                                std::string description,
                                                control::SecretVersionAction action) {
        registry.add(Capability{
            .name = std::move(name),
            .title = std::move(title),
            .description = std::move(description),
            .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                          {"name", {{"type", "string"}}},
                                          {"versions", {{"type", "array"}}}},
                                         json::array({"mount_id", "name", "versions"})),
            .output_schema = object_schema(),
            .kind = CapabilityKind::Action,
            .object = "secret_mount",
            .action = "write",
            .resolve_object = mount_object("secret"),
            .resolves_operator_audit_target = true,
            .handler = [action](const json& input, ApiContext& context) {
                if (!context.store) {
                    return no_store();
                }
                auto versions = versions_of(input);
                if (!versions) {
                    return ApiResult::failure(
                        versions.error().code, versions.error().message, versions.error().path);
                }
                context.audit_details["versions"] = *versions;
                return one_result(context.store->change_secret_versions(
                    input.at("mount_id"), input.at("name"), *versions, action));
            }});
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
        .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                      {"role", {{"type", "string"}}},
                                      {"lifetime_seconds", {{"type", "integer"}}}},
                                     json::array({"mount_id", "role"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("dynamic_secret"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            const std::string suffix = control::identity_crypto::random_secret();
            json credential = {{"username", "lake-" + suffix.substr(0, 12)},
                               {"password", control::identity_crypto::random_secret()}};
            auto issued =
                context.store->lease_dynamic_secret({input.at("mount_id"),
                                                     input.at("role"),
                                                     credential,
                                                     input.value("lifetime_seconds", 300)},
                                                    context.instance_kek);
            if (!issued) {
                return from_store_error(issued.error());
            }
            context.audit_object = "dynamic_secret/" + issued->lease.id;
            context.audit_details["lease_id"] = issued->lease.id;
            context.audit_details["operation"] = "lease";
            return ApiResult::success(
                {{"lease", control::to_json(issued->lease)}, {"value", issued->value}});
        }});
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
        .handler = [](const json& input, ApiContext& context) {
            context.audit_details["operation"] = "renew";
            return one_result(context.store->renew_dynamic_secret_lease(
                input.at("lease_id"), input.value("lifetime_seconds", 300)));
        }});
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
        .handler = [](const json& input, ApiContext& context) {
            context.audit_details["operation"] = "revoke";
            return one_result(context.store->revoke_dynamic_secret_lease(input.at("lease_id")));
        }});
}

void register_encryption_key_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_encryption_key",
        .title = "Create encryption key",
        .description = "Create a named versioned key whose material remains inside lake.",
        .input_schema =
            input_schema({{"mount_id", {{"type", "string"}}}, {"name", {{"type", "string"}}}},
                         json::array({"mount_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->create_encryption_key(
                             {input.at("mount_id"), input.at("name")}, context.instance_kek))
                       : no_store();
        }});
    registry.add(Capability{
        .name = "list_encryption_keys",
        .title = "List encryption keys",
        .description = "List named keys in a readable mount without exposing key material.",
        .input_schema =
            input_schema({{"mount_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}},
                         json::array({"mount_id"})),
        .output_schema = items_schema(),
        .kind = CapabilityKind::Read,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = mount_object("encryption"),
        .authorizes_candidates = true,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto object = mount_object("encryption")(input, context);
            if (!object) {
                return ApiResult::failure(
                    object.error().code, object.error().message, object.error().path);
            }
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
            auto rows = context.store->list_encryption_keys(input.at("mount_id"), limit_of(input));
            if (!rows) {
                return from_store_error(rows.error());
            }
            json items = json::array();
            for (const auto& row : *rows) {
                items.push_back(control::to_json(row));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{.name = "get_encryption_key",
                            .title = "Get encryption key",
                            .description = "Read named-key metadata without exposing key material.",
                            .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                                          {"name", {{"type", "string"}}}},
                                                         json::array({"mount_id", "name"})),
                            .output_schema = object_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "secret_mount",
                            .action = "read",
                            .resolve_object = mount_object("encryption"),
                            .resolves_operator_audit_target = true,
                            .handler = [](const json& input, ApiContext& context) {
                                return context.store ? one_result(context.store->get_encryption_key(
                                                           input.at("mount_id"), input.at("name")))
                                                     : no_store();
                            }});
    registry.add(Capability{
        .name = "rotate_encryption_key",
        .title = "Rotate encryption key",
        .description = "Add a new current version while retaining old versions for decryption.",
        .input_schema =
            input_schema({{"mount_id", {{"type", "string"}}}, {"name", {{"type", "string"}}}},
                         json::array({"mount_id", "name"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto result = context.store->rotate_encryption_key(
                input.at("mount_id"), input.at("name"), context.instance_kek);
            if (result) {
                context.audit_details["version"] = result->current_version;
            }
            return one_result(std::move(result));
        }});
}

void register_encryption_value_capabilities(Registry& registry) {
    const auto add_value_action = [&registry](std::string name,
                                              std::string title,
                                              std::string description,
                                              std::string input_field,
                                              auto operation) {
        registry.add(
            Capability{.name = std::move(name),
                       .title = std::move(title),
                       .description = std::move(description),
                       .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                                     {"name", {{"type", "string"}}},
                                                     {input_field, {{"type", "string"}}}},
                                                    json::array({"mount_id", "name", input_field})),
                       .output_schema = object_schema(),
                       .kind = CapabilityKind::Action,
                       .object = "secret_mount",
                       .action = "write",
                       .resolve_object = mount_object("encryption"),
                       .resolves_operator_audit_target = true,
                       .handler = [input_field = std::move(input_field),
                                   operation](const json& input, ApiContext& context) {
                           if (!context.store) {
                               return no_store();
                           }
                           auto result = operation(*context.store,
                                                   input.at("mount_id").get<std::string>(),
                                                   input.at("name").get<std::string>(),
                                                   input.at(input_field).get<std::string>(),
                                                   context.instance_kek);
                           if (result) {
                               context.audit_details["version"] = result->key_version;
                           }
                           return one_result(std::move(result));
                       }});
    };
    add_value_action("encrypt",
                     "Encrypt",
                     "Encrypt base64url plaintext with the current named-key version.",
                     "plaintext",
                     [](control::Store& store,
                        const std::string& mount_id,
                        const std::string& name,
                        const std::string& value,
                        const std::string& instance_kek) {
                         return store.encrypt(mount_id, name, value, instance_kek);
                     });
    add_value_action("decrypt",
                     "Decrypt",
                     "Decrypt lake ciphertext with the version embedded in its envelope.",
                     "ciphertext",
                     [](control::Store& store,
                        const std::string& mount_id,
                        const std::string& name,
                        const std::string& value,
                        const std::string& instance_kek) {
                         return store.decrypt(mount_id, name, value, instance_kek);
                     });
    add_value_action("rewrap",
                     "Rewrap",
                     "Decrypt and re-encrypt ciphertext under the current named-key version.",
                     "ciphertext",
                     [](control::Store& store,
                        const std::string& mount_id,
                        const std::string& name,
                        const std::string& value,
                        const std::string& instance_kek) {
                         return store.rewrap(mount_id, name, value, instance_kek);
                     });
}

void register_encryption_integrity_capabilities(Registry& registry) {
    const auto message_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                              {"name", {{"type", "string"}}},
                                              {"message", {{"type", "string"}}},
                                              {"version", {{"type", "integer"}}}},
                                             json::array({"mount_id", "name", "message"}));
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
        .handler = [](const json& input, ApiContext& context) {
            auto version = optional_version(input);
            if (!version) {
                return ApiResult::failure(
                    version.error().code, version.error().message, version.error().path);
            }
            auto result = context.store->sign(input.at("mount_id"),
                                              input.at("name"),
                                              input.at("message"),
                                              *version,
                                              context.instance_kek);
            if (result) {
                context.audit_details["version"] = result->key_version;
            }
            return one_result(std::move(result));
        }});
    registry.add(Capability{
        .name = "verify",
        .title = "Verify",
        .description = "Verify an Ed25519 signature without exposing key material.",
        .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"message", {{"type", "string"}}},
                                      {"signature", {{"type", "string"}}},
                                      {"version", {{"type", "integer"}}}},
                                     json::array({"mount_id", "name", "message", "signature"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            auto version = optional_version(input);
            if (!version) {
                return ApiResult::failure(
                    version.error().code, version.error().message, version.error().path);
            }
            auto result = context.store->verify(input.at("mount_id"),
                                                input.at("name"),
                                                input.at("message"),
                                                input.at("signature"),
                                                *version,
                                                context.instance_kek);
            if (result) {
                context.audit_details["version"] = result->key_version;
            }
            return one_result(std::move(result));
        }});
    registry.add(
        Capability{.name = "hmac",
                   .title = "HMAC",
                   .description = "Compute HMAC-SHA-256 with an explicit or current key version.",
                   .input_schema = message_schema,
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "secret_mount",
                   .action = "write",
                   .resolve_object = mount_object("encryption"),
                   .resolves_operator_audit_target = true,
                   .handler = [](const json& input, ApiContext& context) {
                       auto version = optional_version(input);
                       if (!version) {
                           return ApiResult::failure(
                               version.error().code, version.error().message, version.error().path);
                       }
                       auto result = context.store->hmac(input.at("mount_id"),
                                                         input.at("name"),
                                                         input.at("message"),
                                                         *version,
                                                         context.instance_kek);
                       if (result) {
                           context.audit_details["version"] = result->key_version;
                       }
                       return one_result(std::move(result));
                   }});
    registry.add(Capability{
        .name = "derive_encryption_key",
        .title = "Derive encryption key",
        .description = "Derive and store a named child key from a parent and base64url context.",
        .input_schema = input_schema({{"mount_id", {{"type", "string"}}},
                                      {"parent_name", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"context", {{"type", "string"}}}},
                                     json::array({"mount_id", "parent_name", "name", "context"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "write",
        .resolve_object = mount_object("encryption"),
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            context.audit_details["parent_key"] = input.at("parent_name");
            auto result = context.store->derive_encryption_key(input.at("mount_id"),
                                                               input.at("parent_name"),
                                                               input.at("name"),
                                                               input.at("context"),
                                                               context.instance_kek);
            if (result) {
                context.audit_details["version"] = result->parent_version;
            }
            return one_result(std::move(result));
        }});
}

void register_customer_kek_capability(Registry& registry) {
    registry.add(Capability{
        .name = "rotate_customer_kek",
        .title = "Rotate customer KEK",
        .description =
            "Rotate a customer's KEK and rewrap each data key without touching payloads.",
        .input_schema =
            input_schema({{"customer_id", {{"type", "string"}}}}, json::array({"customer_id"})),
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = customer_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto result =
                context.store->rotate_customer_kek(input.at("customer_id"), context.instance_kek);
            if (result) {
                context.audit_details["version"] = result->version;
                context.audit_details["data_keys_rewrapped"] = result->data_keys_rewrapped;
            }
            return one_result(std::move(result));
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
