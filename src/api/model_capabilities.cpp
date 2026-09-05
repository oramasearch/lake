#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

#include "lake/api/sync_store.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

std::expected<std::string, ApiError> repository_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("repository", input.at("id"));
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/id"});
    }
    return object->id;
}

std::expected<control::Repository, ApiError>
readable_repository(const json& input, ApiContext& context, std::string_view kind) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto repository = context.store->get_repository(input.at("id"));
    if (!repository) {
        return std::unexpected(
            ApiError{repository.error().code, repository.error().message, "/id"});
    }
    if (!context.authorized && repository->visibility != "public") {
        return std::unexpected(
            ApiError{"unauthorized", "the " + std::string(kind) + " is not public", "/id"});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(
            ApiError{"not_found", std::string(kind) + " repository is tombstoned", "/id"});
    }
    return *repository;
}

std::expected<control::Repository, ApiError> writable_repository(const json& input,
                                                                 ApiContext& context) {
    if (!context.authorized) {
        return std::unexpected(ApiError{"unauthorized", "authentication is required", {}});
    }
    return readable_repository(input, context, "repository");
}

std::expected<control::Repository, ApiError> readable_training_dataset(const std::string& id,
                                                                       ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto dataset = context.store->get_repository(id);
    if (!dataset) {
        return std::unexpected(
            ApiError{dataset.error().code, dataset.error().message, "/dataset_repository_id"});
    }
    if (!dataset->deleted_at.empty()) {
        return std::unexpected(
            ApiError{"not_found", "dataset repository is tombstoned", "/dataset_repository_id"});
    }
    if (dataset->visibility == "public" || context.acting_identity == "admin") {
        return *dataset;
    }
    const auto object = context.store->find_policy_object("repository", dataset->id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/dataset_repository_id"});
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, object->id, "read", context.authorization_cache);
    if (!decision) {
        return std::unexpected(
            ApiError{decision.error().code, decision.error().message, "/dataset_repository_id"});
    }
    if (!decision->allowed) {
        return std::unexpected(ApiError{"unauthorized",
                                        "principal is not allowed to read the training dataset",
                                        "/dataset_repository_id"});
    }
    return *dataset;
}

ApiResult api_error(const ApiError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::vector<std::byte>, ApiError> decode_base64(std::string_view input,
                                                              std::string path) {
    if (input.empty()) {
        return std::vector<std::byte>{};
    }
    if (input.size() % 4 != 0) {
        return std::unexpected(ApiError{"invalid_input", "invalid base64 data", std::move(path)});
    }
    std::vector<std::byte> output((input.size() / 4) * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(ApiError{"invalid_input", "invalid base64 data", std::move(path)});
    }
    std::size_t padding = 0;
    if (input.back() == '=') {
        ++padding;
    }
    if (input.size() > 1 && input[input.size() - 2] == '=') {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

std::string immutable_revision(std::span<const RepositoryContentRef> refs,
                               std::string_view revision) {
    const auto found = std::ranges::find(refs, revision, &RepositoryContentRef::name);
    return found == refs.end() ? std::string(revision) : found->change;
}

json storage_json(const RepositoryStorageSummary& storage) {
    json revisions = json::array();
    for (const RepositoryRevisionStorage& revision : storage.revisions) {
        revisions.push_back({{"revision", revision.revision},
                             {"logical_bytes", revision.logical_bytes},
                             {"unique_bytes", revision.unique_bytes},
                             {"deduplicated_bytes", revision.deduplicated_bytes},
                             {"dedup_ratio", revision.dedup_ratio}});
    }
    return {{"logical_bytes", storage.logical_bytes},
            {"unique_bytes", storage.unique_bytes},
            {"deduplicated_bytes", storage.deduplicated_bytes},
            {"dedup_ratio", storage.dedup_ratio},
            {"revisions", std::move(revisions)}};
}

json provenance_json(const RepositoryModelProvenance& value) {
    return {{"model_revision", value.model_revision},
            {"dataset_repository_id", value.dataset_repository_id},
            {"dataset_revision", value.dataset_revision},
            {"code_revision", value.code_revision},
            {"model_attestation", value.model_attestation},
            {"run_attestation", value.run_attestation},
            {"parameters", value.parameters},
            {"environment", value.environment},
            {"environment_digest", value.environment_digest},
            {"runner", value.runner},
            {"builder", value.builder},
            {"invocation_id", value.invocation_id},
            {"verified", value.verified},
            {"reason", value.reason}};
}

bool has_profile(const RepositoryArtifact& artifact, std::string_view profile) {
    return std::ranges::any_of(artifact.attestations, [&](const auto& attestation) {
        return attestation.verified && attestation.profile == profile;
    });
}

json attestation_json(const RepositoryArtifactAttestation& attestation) {
    return {{"id", attestation.id},
            {"profile", attestation.profile},
            {"subject", attestation.subject},
            {"issuer", attestation.issuer},
            {"verified", attestation.verified},
            {"reason", attestation.reason}};
}

json repository_input_schema(bool file) {
    json required = json::array({"id", "revision"});
    if (file) {
        required.push_back("path");
    }
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"revision", {{"type", "string"}}},
              {"path", {{"type", "string"}}}}},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

ApiResult repository_info(const json& input, ApiContext& context, std::string_view kind) {
    const auto repository = readable_repository(input, context, kind);
    if (!repository) {
        return api_error(repository.error());
    }
    if (!context.sync_store) {
        return ApiResult::failure("store_unavailable", "no repository store is configured");
    }
    const std::string revision = input.at("revision");
    const auto refs = context.sync_store->repository_refs(repository->customer_id, repository->id);
    if (!refs) {
        return sync_error(refs.error());
    }
    const std::string resolved_revision = immutable_revision(*refs, revision);
    const auto tree =
        context.sync_store->repository_tree(repository->customer_id, repository->id, revision);
    if (!tree) {
        return sync_error(tree.error());
    }
    std::string card_contents;
    std::string card_path;
    const auto card_entry = std::ranges::find_if(*tree, [](const auto& entry) {
        return entry.path == "MODEL_CARD.md" || entry.path == "README.md";
    });
    if (card_entry != tree->end()) {
        const auto card = context.sync_store->repository_file(
            repository->customer_id, repository->id, revision, card_entry->path);
        if (!card) {
            return sync_error(card.error());
        }
        card_contents = card->contents;
        card_path = card->path;
    }
    json siblings = json::array();
    for (const RepositoryTreeEntry& entry : *tree) {
        if (entry.kind != "blob") {
            continue;
        }
        siblings.push_back({{"rfilename", entry.path},
                            {"blobId", entry.blob},
                            {"size", entry.size},
                            {"storedSize", entry.stored_size},
                            {"deduplicatedBytes", entry.size - entry.stored_size}});
    }
    json build_provenance = json::array();
    bool build_provenance_complete = true;
    RepositoryArtifactFilter filter;
    filter.limit = 1'000;
    const auto artifacts =
        context.sync_store->repository_artifacts(repository->customer_id, repository->id, filter);
    if (!artifacts) {
        return sync_error(artifacts.error());
    }
    for (const RepositoryArtifact& item : artifacts->items) {
        const auto artifact = context.sync_store->repository_artifact(
            repository->customer_id, repository->id, item.id);
        if (!artifact) {
            return sync_error(artifact.error());
        }
        const bool has_sbom = has_profile(*artifact, "sbom/1");
        const bool has_slsa = has_profile(*artifact, "slsa_provenance/1");
        build_provenance_complete = build_provenance_complete && has_sbom && has_slsa;
        json attestations = json::array();
        for (const auto& attestation : artifact->attestations) {
            attestations.push_back(attestation_json(attestation));
        }
        build_provenance.push_back({{"artifact", artifact->id},
                                    {"sbom", has_sbom},
                                    {"slsa_provenance", has_slsa},
                                    {"attestations", std::move(attestations)}});
    }
    if (kind == "model" && !build_provenance_complete) {
        return ApiResult::failure(
            "conflict",
            "every model artifact requires verified sbom/1 and slsa_provenance/1 records");
    }
    const auto stored =
        context.sync_store->repository_storage(repository->customer_id, repository->id);
    RepositoryStorageSummary storage;
    if (!stored && stored.error().code != "unsupported") {
        return sync_error(stored.error());
    }
    if (stored) {
        storage = *stored;
    }
    json training_provenance = nullptr;
    if (kind == "model") {
        const auto provenance =
            context.sync_store->model_provenance(repository->customer_id, repository->id, revision);
        if (!provenance) {
            return sync_error(provenance.error());
        }
        if (*provenance) {
            RepositoryModelProvenance verified = **provenance;
            const auto dataset = context.store->get_repository(verified.dataset_repository_id);
            if (!dataset) {
                verified.verified = false;
                verified.reason = "the training dataset repository is unavailable";
            } else {
                const auto dataset_tree = context.sync_store->repository_tree(
                    dataset->customer_id, dataset->id, verified.dataset_revision);
                if (!dataset_tree) {
                    verified.verified = false;
                    verified.reason = "the training dataset revision is unavailable";
                }
            }
            training_provenance = provenance_json(verified);
        }
    }
    context.audit_customer_id = repository->customer_id;
    context.audit_kind = std::string(kind) + ".read";
    context.audit_object = "repository/" + repository->id;
    context.audit_details = {{"revision", revision}, {"files", siblings.size()}};
    return ApiResult::success({{"id", repository->id},
                               {kind == "model" ? "modelId" : "datasetId", repository->name},
                               {"sha", resolved_revision},
                               {"private", repository->visibility != "public"},
                               {"card", card_contents},
                               {"card_path", card_path},
                               {"siblings", std::move(siblings)},
                               {"storage", storage_json(storage)},
                               {"provenance_complete", build_provenance_complete},
                               {"provenance", std::move(build_provenance)},
                               {"training_provenance", std::move(training_provenance)}});
}

ApiResult resolve_file(const json& input, ApiContext& context, std::string_view kind) {
    const auto repository = readable_repository(input, context, kind);
    if (!repository) {
        return api_error(repository.error());
    }
    if (!context.sync_store) {
        return ApiResult::failure("store_unavailable", "no repository store is configured");
    }
    const auto refs = context.sync_store->repository_refs(repository->customer_id, repository->id);
    if (!refs) {
        return sync_error(refs.error());
    }
    const std::string revision = input.at("revision");
    const std::string resolved_revision = immutable_revision(*refs, revision);
    const auto file = context.sync_store->repository_file(
        repository->customer_id, repository->id, revision, input.at("path"));
    if (!file) {
        return sync_error(file.error());
    }
    context.sync_response = file->raw_contents;
    context.audit_customer_id = repository->customer_id;
    context.audit_kind = std::string(kind) + ".file.read";
    context.audit_object = "repository/" + repository->id + "/path/" + file->path;
    context.audit_details = {{"blob", file->blob}, {"path", file->path}};
    return ApiResult::success({{"path", file->path},
                               {"blob", file->blob},
                               {"size", file->size},
                               {"revision", resolved_revision},
                               {"contents", file->contents}});
}

Capability read_capability(
    std::string name, std::string title, std::string description, bool file, std::string kind) {
    return Capability{
        .name = std::move(name),
        .title = std::move(title),
        .description = std::move(description),
        .input_schema = repository_input_schema(file),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = repository_object,
        .allows_anonymous_public_read = true,
        .handler = [file, kind = std::move(kind)](const json& input, ApiContext& context) {
            return file ? resolve_file(input, context, kind)
                        : repository_info(input, context, kind);
        }};
}

json write_input_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"revision", {{"type", "string"}}},
              {"message", {{"type", "string"}}},
              {"oid", {{"type", "string"}}},
              {"size", {{"type", "integer"}}},
              {"content", {{"type", "string"}}},
              {"files", {{"type", "array"}}},
              {"operations", {{"type", "array"}}},
              {"dataset_repository_id", {{"type", "string"}}},
              {"dataset_revision", {{"type", "string"}}},
              {"model_attestation", {{"type", "string"}}},
              {"run_attestation", {{"type", "string"}}}}},
            {"required", json::array({"id"})}};
}

Capability write_capability(std::string name,
                            std::string title,
                            std::string description,
                            CapabilityHandler handler) {
    return Capability{.name = std::move(name),
                      .title = std::move(title),
                      .description = std::move(description),
                      .input_schema = write_input_schema(),
                      .output_schema = {{"type", "object"}},
                      .kind = CapabilityKind::Action,
                      .object = "repository",
                      .action = "write",
                      .resolve_object = repository_object,
                      .handler = std::move(handler)};
}

} // namespace

void register_model_capabilities(Registry& registry) {
    registry.add(read_capability("get_model",
                                 "Get model",
                                 "Read model files, storage savings, and verified provenance.",
                                 false,
                                 "model"));
    registry.add(read_capability(
        "get_dataset", "Get dataset", "Read dataset files and storage savings.", false, "dataset"));
    registry.add(read_capability("resolve_model_file",
                                 "Resolve model file",
                                 "Resolve one model file at a named or immutable revision.",
                                 true,
                                 "model"));
    registry.add(read_capability("resolve_dataset_file",
                                 "Resolve dataset file",
                                 "Resolve one dataset file at a named or immutable revision.",
                                 true,
                                 "dataset"));

    registry.add(write_capability(
        "prepare_repository_upload",
        "Prepare repository upload",
        "Choose regular or LFS-compatible upload mode for repository files.",
        [](const json& input, ApiContext& context) {
            const auto repository = writable_repository(input, context);
            if (!repository) {
                return api_error(repository.error());
            }
            if (!input.contains("files") || !input.at("files").is_array()) {
                return ApiResult::failure("invalid_input", "files must be an array", "/files");
            }
            json files = json::array();
            for (const json& file : input.at("files")) {
                if (!file.is_object() || !file.contains("path") || !file.contains("size")) {
                    return ApiResult::failure(
                        "invalid_input", "each file requires path and size", "/files");
                }
                const std::uint64_t size = file.at("size").get<std::uint64_t>();
                files.push_back({{"path", file.at("path")},
                                 {"uploadMode", size > 5U * 1024U * 1024U ? "lfs" : "regular"},
                                 {"shouldIgnore", false}});
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "repository.upload.prepare";
            context.audit_object = "repository/" + repository->id;
            return ApiResult::success({{"files", std::move(files)}});
        }));

    registry.add(write_capability(
        "stage_repository_file",
        "Stage repository file",
        "Stage a SHA-256-addressed LFS-compatible file in the liquid chunk store.",
        [](const json& input, ApiContext& context) {
            const auto repository = writable_repository(input, context);
            if (!repository) {
                return api_error(repository.error());
            }
            if (!context.sync_store) {
                return ApiResult::failure("store_unavailable", "no repository store is configured");
            }
            if (!input.contains("oid") || !input.contains("size")) {
                return ApiResult::failure("invalid_input", "oid and size are required");
            }
            std::vector<std::byte> contents;
            if (context.sync_wire_request) {
                contents.assign(context.sync_request.begin(), context.sync_request.end());
            } else {
                const auto decoded = decode_base64(input.value("content", ""), "/content");
                if (!decoded) {
                    return api_error(decoded.error());
                }
                contents = std::move(*decoded);
            }
            const std::uint64_t expected_size = input.at("size").get<std::uint64_t>();
            if (contents.size() != expected_size) {
                return ApiResult::failure("invalid_input", "uploaded size does not match");
            }
            const std::string bytes(reinterpret_cast<const char*>(contents.data()),
                                    contents.size());
            if (control::identity_crypto::sha256_hex(bytes) != input.at("oid").get<std::string>()) {
                return ApiResult::failure("invalid_input", "uploaded SHA-256 does not match");
            }
            const auto stored =
                context.sync_store->stage_repository_file(repository->customer_id,
                                                          repository->id,
                                                          input.at("oid"),
                                                          std::move(contents),
                                                          context.metering.storage_quota_bytes);
            if (!stored) {
                return sync_error(stored.error());
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "repository.file.stage";
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"oid", input.at("oid")}, {"stored_bytes", *stored}};
            return ApiResult::success({{"stored_bytes", *stored}});
        }));

    registry.add(write_capability(
        "commit_repository_files",
        "Commit repository files",
        "Commit regular, staged LFS, and delete operations to a repository revision.",
        [](const json& input, ApiContext& context) {
            const auto repository = writable_repository(input, context);
            if (!repository) {
                return api_error(repository.error());
            }
            if (!context.sync_store) {
                return ApiResult::failure("store_unavailable", "no repository store is configured");
            }
            if (!input.contains("operations") || !input.at("operations").is_array()) {
                return ApiResult::failure(
                    "invalid_input", "operations must be an array", "/operations");
            }
            std::vector<RepositoryWriteOperation> operations;
            for (std::size_t index = 0; index < input.at("operations").size(); ++index) {
                const json& item = input.at("operations").at(index);
                const std::string path = "/operations/" + std::to_string(index);
                if (!item.is_object() || !item.contains("type") || !item.contains("path")) {
                    return ApiResult::failure(
                        "invalid_input", "each operation requires type and path", path);
                }
                RepositoryWriteOperation operation;
                operation.path = item.at("path");
                const std::string type = item.at("type");
                if (type == "file") {
                    operation.kind = RepositoryWriteKind::File;
                    const auto decoded =
                        decode_base64(item.value("content", ""), path + "/content");
                    if (!decoded) {
                        return api_error(decoded.error());
                    }
                    operation.contents = std::move(*decoded);
                } else if (type == "lfsFile") {
                    operation.kind = RepositoryWriteKind::LfsFile;
                    operation.oid = item.value("oid", "");
                    operation.size = item.value("size", std::uint64_t{0});
                } else if (type == "deletedFile") {
                    operation.kind = RepositoryWriteKind::DeletedFile;
                } else if (type == "deletedFolder") {
                    operation.kind = RepositoryWriteKind::DeletedFolder;
                } else {
                    return ApiResult::failure("invalid_input", "unsupported operation type", path);
                }
                operations.push_back(std::move(operation));
            }
            const auto committed = context.sync_store->commit_repository_files(
                repository->customer_id,
                repository->id,
                input.value("revision", "main"),
                input.value("message", "Hugging Face upload"),
                std::move(operations));
            if (!committed) {
                return sync_error(committed.error());
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "repository.commit";
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"revision", *committed}};
            return ApiResult::success({{"commitOid", *committed}});
        }));

    registry.add(write_capability(
        "attach_model_provenance",
        "Attach model provenance",
        "Attach liquid model and run attestations to a model revision.",
        [](const json& input, ApiContext& context) {
            const auto repository = writable_repository(input, context);
            if (!repository) {
                return api_error(repository.error());
            }
            if (!context.sync_store || !context.store) {
                return ApiResult::failure("store_unavailable", "repository stores are unavailable");
            }
            const auto dataset =
                readable_training_dataset(input.value("dataset_repository_id", ""), context);
            if (!dataset) {
                return api_error(dataset.error());
            }
            const std::string requested_dataset_revision = input.value("dataset_revision", "");
            const auto dataset_refs =
                context.sync_store->repository_refs(dataset->customer_id, dataset->id);
            if (!dataset_refs) {
                return sync_error(dataset_refs.error());
            }
            const std::string dataset_revision =
                immutable_revision(*dataset_refs, requested_dataset_revision);
            const auto dataset_tree = context.sync_store->repository_tree(
                dataset->customer_id, dataset->id, dataset_revision);
            if (!dataset_tree) {
                return sync_error(dataset_tree.error());
            }
            const auto model =
                decode_base64(input.value("model_attestation", ""), "/model_attestation");
            const auto run = decode_base64(input.value("run_attestation", ""), "/run_attestation");
            if (!model || !run) {
                return api_error(!model ? model.error() : run.error());
            }
            const auto attached =
                context.sync_store->attach_model_provenance(repository->customer_id,
                                                            repository->id,
                                                            input.value("revision", "main"),
                                                            dataset->id,
                                                            dataset_revision,
                                                            std::move(*model),
                                                            std::move(*run));
            if (!attached) {
                return sync_error(attached.error());
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "model.provenance.attach";
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"model_revision", attached->model_revision},
                                     {"dataset_revision", attached->dataset_revision}};
            return ApiResult::success(provenance_json(*attached));
        }));

    registry.add(write_capability(
        "verify_model_provenance",
        "Verify model provenance",
        "Verify the signed dataset-to-model training provenance chain.",
        [](const json& input, ApiContext& context) {
            const auto repository = readable_repository(input, context, "model");
            if (!repository) {
                return api_error(repository.error());
            }
            if (!context.sync_store || !context.store) {
                return ApiResult::failure("store_unavailable", "repository stores are unavailable");
            }
            const auto provenance = context.sync_store->model_provenance(
                repository->customer_id, repository->id, input.value("revision", "main"));
            if (!provenance) {
                return sync_error(provenance.error());
            }
            if (!*provenance) {
                return ApiResult::success(
                    {{"verified", false}, {"reason", "no training provenance is attached"}});
            }
            RepositoryModelProvenance verified = **provenance;
            const auto dataset = context.store->get_repository(verified.dataset_repository_id);
            if (!dataset) {
                verified.verified = false;
                verified.reason = "the training dataset repository is unavailable";
            } else {
                const auto tree = context.sync_store->repository_tree(
                    dataset->customer_id, dataset->id, verified.dataset_revision);
                if (!tree) {
                    verified.verified = false;
                    verified.reason = "the training dataset revision is unavailable";
                }
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "model.provenance.verify";
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"verified", verified.verified}};
            return ApiResult::success(provenance_json(verified));
        }));
}

} // namespace lake::api
