#include "lake/api/capabilities.hpp"

#include <array>
#include <expected>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

#include "lake/audit/audit.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::string, ApiError> repository_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("repository", input.at("repository_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/repository_id"});
    }
    return object->id;
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

json sha256(const json& value) {
    const std::string encoded = value.dump();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(encoded.data(), encoded.size(), digest.data(), &size, EVP_sha256(), nullptr) !=
        1) {
        return nullptr;
    }
    std::string text;
    text.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
        text += std::format("{:02x}", digest[index]);
    }
    return text;
}

json manifest_for(const json& payload) {
    json entries = json::array();
    for (const std::string_view name :
         {"repository", "history", "artifacts", "forge", "grants", "git"}) {
        entries.push_back({{"name", name}, {"sha256", sha256(payload.at(name))}});
    }
    return {{"format", "lake_export_manifest/1"}, {"entries", std::move(entries)}};
}

ApiResult verify_archive(const json& archive) {
    if (!archive.is_object() || archive.value("format", "") != "lake_export/1" ||
        !archive.contains("payload") || !archive.at("payload").is_object() ||
        !archive.contains("manifest") || !archive.at("manifest").is_object() ||
        !archive.contains("signature")) {
        return ApiResult::failure(
            "invalid_input", "archive is not a lake_export/1 document", "/archive");
    }
    const json& payload = archive.at("payload");
    for (const std::string_view name :
         {"repository", "history", "artifacts", "forge", "grants", "git"}) {
        if (!payload.contains(name)) {
            return ApiResult::failure(
                "invalid_input", "archive payload is incomplete", "/archive/payload");
        }
    }
    if (archive.at("manifest") != manifest_for(payload)) {
        return ApiResult::failure(
            "invalid_digest", "archive manifest does not match its payload", "/archive");
    }
    const auto signature =
        audit::verify_export_manifest(archive.at("manifest"), archive.at("signature"));
    if (!signature) {
        return ApiResult::failure(
            signature.error().code, signature.error().message, "/archive/signature");
    }
    return ApiResult::success(nullptr);
}

json export_forge_records(control::Store& store, const std::string& repository_id) {
    const auto records = store.list_forge_records(repository_id, "", 100'000);
    if (!records) {
        return json();
    }
    json output = json::array();
    for (const control::ForgeRecord& record : *records) {
        if (record.kind != "export" && record.kind != "github_import") {
            output.push_back(control::to_json(record));
        }
    }
    return output;
}

ApiResult export_repository(const json& input, ApiContext& context) {
    if (!context.store || !context.sync_store) {
        return ApiResult::failure("store_unavailable",
                                  "control and repository stores are required");
    }
    const auto repository = context.store->get_repository(input.at("repository_id"));
    if (!repository) {
        return store_error(repository.error());
    }
    const auto portable = context.sync_store->export_portable(
        repository->customer_id, repository->id, input.value("include_git", false));
    if (!portable) {
        return sync_error(portable.error());
    }
    const auto object = context.store->find_policy_object("repository", repository->id);
    if (!object) {
        return store_error(object.error());
    }
    const auto grants = context.store->list_grants({object->id}, std::nullopt, 100'000);
    if (!grants) {
        return store_error(grants.error());
    }
    json grant_values = json::array();
    for (const control::Grant& grant : *grants) {
        grant_values.push_back(control::to_json(grant));
    }
    const json payload = {{"repository", control::to_json(*repository)},
                          {"history", portable->history},
                          {"artifacts", portable->artifacts},
                          {"forge", export_forge_records(*context.store, repository->id)},
                          {"grants", std::move(grant_values)},
                          {"git", portable->git}};
    if (payload.at("forge").is_null()) {
        return ApiResult::failure("store_error", "could not enumerate forge records");
    }
    const json manifest = manifest_for(payload);
    const auto signature = audit::sign_export_manifest(
        *context.store,
        manifest,
        {.configured_seed = context.identity_seed, .instance_kek = context.instance_kek});
    if (!signature) {
        return ApiResult::failure(signature.error().code, signature.error().message);
    }
    const json archive = {{"format", "lake_export/1"},
                          {"manifest", manifest},
                          {"signature", *signature},
                          {"payload", payload}};
    const auto recorded = context.store->create_forge_record(
        {repository->id,
         "export",
         {{"manifest", manifest}, {"include_git", input.value("include_git", false)}}});
    if (!recorded) {
        return store_error(recorded.error());
    }
    context.audit_customer_id = repository->customer_id;
    context.audit_object = "repository/" + repository->id;
    context.audit_details = {{"forge_kind", "export"}, {"record_id", recorded->id}};
    return ApiResult::success({{"archive", archive}, {"record", control::to_json(*recorded)}});
}

ApiResult import_repository(const json& input, ApiContext& context) {
    if (!context.store || !context.sync_store) {
        return ApiResult::failure("store_unavailable",
                                  "control and repository stores are required");
    }
    const json& archive = input.at("archive");
    if (const ApiResult verified = verify_archive(archive); !verified.ok) {
        return verified;
    }
    const json& payload = archive.at("payload");
    const json& source = payload.at("repository");
    // The archive's own workspace_id is where it came from; the caller names where it goes.
    const auto workspace = repository_workspace(context, input);
    if (!workspace) {
        return ApiResult::failure(
            workspace.error().code, workspace.error().message, workspace.error().path);
    }
    const auto created = context.store->create_repository({input.at("customer_id"),
                                                           input.at("name"),
                                                           source.value("description", ""),
                                                           source.value("visibility", "private"),
                                                           workspace->id});
    if (!created) {
        return store_error(created.error());
    }
    const RepositoryPortableData portable{
        payload.at("history"), payload.at("artifacts"), payload.at("git")};
    const auto imported =
        context.sync_store->import_portable(created->customer_id, created->id, portable);
    if (!imported) {
        return sync_error(imported.error());
    }
    const auto target_object = context.store->find_policy_object("repository", created->id);
    if (!target_object) {
        return store_error(target_object.error());
    }
    for (const json& grant : payload.at("grants")) {
        const auto written = context.store->create_grant({grant.value("principal_taap_id", ""),
                                                          target_object->id,
                                                          grant.value("role", ""),
                                                          grant.value("attestation_digest", ""),
                                                          grant.value("granted_by_taap_id", ""),
                                                          context.request_id});
        if (!written) {
            return store_error(written.error());
        }
    }
    std::map<std::string, std::string> ids;
    for (const json& record : payload.at("forge")) {
        const std::string kind = record.value("kind", "");
        if (kind == "issue_comment" || kind == "review_comment" || kind == "webhook_delivery") {
            continue;
        }
        json value = record.value("value", json::object());
        if (kind == "branch_protection") {
            const auto policy =
                context.store->create_policy_object({"path",
                                                     created->id + ":" + value.value("ref", ""),
                                                     target_object->id,
                                                     context.acting_identity,
                                                     context.request_id,
                                                     true});
            if (!policy) {
                return store_error(policy.error());
            }
            value["policy_object_id"] = policy->id;
        }
        const auto written = context.store->create_forge_record({created->id, kind, value});
        if (!written) {
            return store_error(written.error());
        }
        ids.emplace(record.value("id", ""), written->id);
    }
    for (const json& record : payload.at("forge")) {
        const std::string kind = record.value("kind", "");
        if (kind != "issue_comment" && kind != "review_comment" && kind != "webhook_delivery") {
            continue;
        }
        json value = record.value("value", json::object());
        const std::string key = kind == "issue_comment"    ? "issue_id"
                                : kind == "review_comment" ? "review_id"
                                                           : "webhook_id";
        if (const auto found = ids.find(value.value(key, "")); found != ids.end()) {
            value[key] = found->second;
        }
        const auto written = context.store->create_forge_record({created->id, kind, value});
        if (!written) {
            return store_error(written.error());
        }
    }
    context.audit_customer_id = created->customer_id;
    context.audit_object = "repository/" + created->id;
    context.audit_details = {{"forge_kind", "import"},
                             {"source_signer", archive["signature"]["identity"]["taap_id"]}};
    return ApiResult::success(control::to_json(*created));
}

} // namespace

void register_export_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "export_repository",
        .title = "Export repository",
        .description = "Export a repository, with its history, reviews, issues and artifacts, as a "
                       "signed archive you can download and import elsewhere.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"include_git", {{"type", "boolean"}}}}},
                         {"required", json::array({"repository_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "read",
        .resolve_object = repository_object,
        .handler = export_repository});
    registry.add(
        Capability{.name = "import_repository",
                   .title = "Import repository",
                   .description = "Verify and recreate a signed portable repository archive "
                                  "as a new repository in one workspace (`workspace_id`, "
                                  "required) of the customer's space.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"customer_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"archive", {{"type", "object"}}}}},
                                    {"required",
                                     json::array({"customer_id", "workspace_id", "name", "archive"})},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Action,
                   .object = "customer",
                   .action = "write",
                   .resolve_object = customer_object,
                   .handler = import_repository});
}

} // namespace lake::api
