#include "lake/api/capabilities.hpp"

#include <array>
#include <expected>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

#include "forge_records_api.hpp"
#include "repository_records_api.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_bus.hpp"
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

liquid::runtime::task<ApiResult> export_repository(const json& input, ApiContext& context) {
    if (!context.store || !context.sync_store) {
        co_return ApiResult::failure("store_unavailable",
                                  "control and repository stores are required");
    }
    const auto repository = context.store->get_repository(input.at("repository_id"));
    if (!repository) {
        co_return store_error(repository.error());
    }
    // The forge records come from the repository's workspace (lake#341).
    auto forge = RepositoryForge::open(context, *repository);
    if (!forge) {
        co_return ApiResult::failure(forge.error().code, forge.error().message,
                                     forge.error().path);
    }
    const auto records = control::settled(co_await forge->list("", 0));
    if (!records) {
        co_return forge_records_failure(records.error());
    }
    json forge_values = json::array();
    for (const control::ForgeRecord& record : *records) {
        if (record.kind != "export" && record.kind != "github_import") {
            forge_values.push_back(control::to_json(record));
        }
    }
    const auto portable = co_await blocking_call(
        [&] {
            return context.sync_store->export_portable(
                repository->customer_id, repository->id, input.value("include_git", false));
        },
        [](std::string message) -> SyncStoreResult<RepositoryPortableData> {
            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
        });
    if (!portable) {
        co_return sync_error(portable.error());
    }
    const auto object = context.store->find_policy_object("repository", repository->id);
    if (!object) {
        co_return store_error(object.error());
    }
    const auto grants = context.store->list_grants({object->id}, std::nullopt, 100'000);
    if (!grants) {
        co_return store_error(grants.error());
    }
    json grant_values = json::array();
    for (const control::Grant& grant : *grants) {
        grant_values.push_back(control::to_json(grant));
    }
    const json payload = {{"repository", control::to_json(*repository)},
                          {"history", portable->history},
                          {"artifacts", portable->artifacts},
                          {"forge", std::move(forge_values)},
                          {"grants", std::move(grant_values)},
                          {"git", portable->git}};
    const json manifest = manifest_for(payload);
    const auto signature = audit::sign_export_manifest(
        *context.store,
        manifest,
        {.configured_seed = context.identity_seed, .instance_kek = context.instance_kek});
    if (!signature) {
        co_return ApiResult::failure(signature.error().code, signature.error().message);
    }
    const json archive = {{"format", "lake_export/1"},
                          {"manifest", manifest},
                          {"signature", *signature},
                          {"payload", payload}};
    const auto recorded = control::settled(co_await forge->create(
        "export", {{"manifest", manifest}, {"include_git", input.value("include_git", false)}}));
    if (!recorded) {
        co_return forge_records_failure(recorded.error());
    }
    context.audit_customer_id = repository->customer_id;
    context.audit_object = "repository/" + repository->id;
    context.audit_details = {{"forge_kind", "export"},
                             {"record_id", recorded->id},
                             {"workspace_id", repository->workspace_id}};
    co_return ApiResult::success({{"archive", archive}, {"record", control::to_json(*recorded)}});
}

liquid::runtime::task<ApiResult> import_repository(const json& input, ApiContext& context) {
    if (!context.store || !context.sync_store) {
        co_return ApiResult::failure("store_unavailable",
                                  "control and repository stores are required");
    }
    const json& archive = input.at("archive");
    if (ApiResult verified = verify_archive(archive); !verified.ok) {
        co_return verified;
    }
    const json& payload = archive.at("payload");
    const json& source = payload.at("repository");
    // The archive's own workspace_id is where it came from; the caller names where it goes.
    const auto workspace = repository_workspace(context, input);
    if (!workspace) {
        co_return ApiResult::failure(
            workspace.error().code, workspace.error().message, workspace.error().path);
    }
    // The archive's description is the repository's settings record in its workspace (lake#315).
    const auto created = settled_create(co_await create_repository_with_settings(
        context,
        {input.at("customer_id"), input.at("name"), "", source.value("visibility", "private"),
         workspace->id},
        source.value("description", "")));
    if (!created) {
        co_return created.error();
    }
    const RepositoryPortableData portable{
        payload.at("history"), payload.at("artifacts"), payload.at("git")};
    const auto imported = co_await blocking_call(
        [&] { return context.sync_store->import_portable(created->customer_id, created->id, portable); },
        [](std::string message) -> SyncStoreResult<void> {
            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
        });
    if (!imported) {
        co_return sync_error(imported.error());
    }
    const auto target_object = context.store->find_policy_object("repository", created->id);
    if (!target_object) {
        co_return store_error(target_object.error());
    }
    for (const json& grant : payload.at("grants")) {
        const auto written = context.store->create_grant({grant.value("principal_taap_id", ""),
                                                          target_object->id,
                                                          grant.value("role", ""),
                                                          grant.value("attestation_digest", ""),
                                                          grant.value("granted_by_taap_id", ""),
                                                          context.request_id});
        if (!written) {
            co_return store_error(written.error());
        }
    }
    // The archive's forge records go to the new repository's workspace (lake#341).
    auto forge = RepositoryForge::open(context, *created);
    if (!forge) {
        co_return ApiResult::failure(forge.error().code, forge.error().message,
                                     forge.error().path);
    }
    std::map<std::string, std::string> ids;
    for (const json& record : payload.at("forge")) {
        const std::string kind = record.value("kind", "");
        if (kind == "issue_comment" || kind == "review_comment" || kind == "webhook_delivery") {
            continue;
        }
        json value = record.value("value", json::object());
        // An approval is lake's record of who approved in this repository (LSR-077); one carried
        // in an archive is not, so an imported review never satisfies a required review.
        if (kind == "review" && value.is_object()) {
            value.erase("approval");
        }
        if (kind == "branch_protection") {
            const auto policy =
                context.store->create_policy_object({"path",
                                                     created->id + ":" + value.value("ref", ""),
                                                     target_object->id,
                                                     context.acting_identity,
                                                     context.request_id,
                                                     true});
            if (!policy) {
                co_return store_error(policy.error());
            }
            value["policy_object_id"] = policy->id;
        }
        const auto written = control::settled(co_await forge->create(kind, std::move(value)));
        if (!written) {
            co_return forge_records_failure(written.error());
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
        const auto written = control::settled(co_await forge->create(kind, std::move(value)));
        if (!written) {
            co_return forge_records_failure(written.error());
        }
    }
    context.audit_customer_id = created->customer_id;
    context.audit_object = "repository/" + created->id;
    context.audit_details = {{"forge_kind", "import"},
                             {"source_signer", archive["signature"]["identity"]["taap_id"]}};
    co_return ApiResult::success(control::to_json(*created));
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
        .async_handler = export_repository});
    registry.add(
        Capability{.name = "import_repository",
                   .title = "Import repository",
                   .description = "Verify and recreate a signed portable repository archive "
                                  "as a new repository in one workspace (`workspace_id`, "
                                  "required) of the customer's account.",
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
                   .async_handler = import_repository});
}

} // namespace lake::api
