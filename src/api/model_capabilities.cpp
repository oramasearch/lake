#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <expected>
#include <set>
#include <string>

#include "lake/api/sync_store.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

std::expected<std::string, ApiError> model_object(const json& input, ApiContext& context) {
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

std::expected<control::Repository, ApiError> readable_model_repository(const json& input,
                                                                       ApiContext& context) {
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
        return std::unexpected(ApiError{"unauthorized", "the model is not public", "/id"});
    }
    if (repository->deleted_at.empty()) {
        return *repository;
    }
    return std::unexpected(ApiError{"not_found", "model repository is tombstoned", "/id"});
}

ApiResult model_error(const ApiError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
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

json model_input_schema(bool file) {
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

} // namespace

void register_model_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "get_model",
        .title = "Get model",
        .description = "Read a Hugging Face-compatible model card, files, and build provenance.",
        .input_schema = model_input_schema(false),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "model repository",
        .action = "read",
        .resolve_object = model_object,
        .allows_anonymous_public_read = true,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = readable_model_repository(input, context);
            if (!repository) {
                return model_error(repository.error());
            }
            if (!context.sync_store) {
                return ApiResult::failure("store_unavailable", "no repository store is configured");
            }
            const std::string revision = input.at("revision");
            const auto refs =
                context.sync_store->repository_refs(repository->customer_id, repository->id);
            if (!refs) {
                return sync_error(refs.error());
            }
            std::string resolved_revision = revision;
            for (const RepositoryContentRef& ref : *refs) {
                if (ref.name == revision) {
                    resolved_revision = ref.change;
                    break;
                }
            }
            const auto tree = context.sync_store->repository_tree(
                repository->customer_id, repository->id, revision);
            if (!tree) {
                return sync_error(tree.error());
            }
            const auto card_entry = std::ranges::find_if(*tree, [](const auto& entry) {
                return entry.path == "MODEL_CARD.md" || entry.path == "README.md";
            });
            if (card_entry == tree->end()) {
                return ApiResult::failure("not_found",
                                          "model repository has no MODEL_CARD.md or README.md");
            }
            const auto card = context.sync_store->repository_file(
                repository->customer_id, repository->id, revision, card_entry->path);
            if (!card) {
                return sync_error(card.error());
            }
            json siblings = json::array();
            for (const RepositoryTreeEntry& entry : *tree) {
                if (entry.kind == "blob") {
                    siblings.push_back(
                        {{"rfilename", entry.path}, {"blob_id", entry.blob}, {"size", nullptr}});
                }
            }
            json provenance = json::array();
            bool complete = true;
            auto filter = RepositoryArtifactFilter{};
            filter.limit = 1'000;
            const auto artifacts = context.sync_store->repository_artifacts(
                repository->customer_id, repository->id, filter);
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
                complete = complete && has_sbom && has_slsa;
                json attestations = json::array();
                for (const auto& attestation : artifact->attestations) {
                    attestations.push_back(attestation_json(attestation));
                }
                provenance.push_back({{"artifact", artifact->id},
                                      {"sbom", has_sbom},
                                      {"slsa_provenance", has_slsa},
                                      {"attestations", std::move(attestations)}});
            }
            if (!complete) {
                return ApiResult::failure(
                    "conflict",
                    "every model artifact requires verified sbom/1 and slsa_provenance/1 records");
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "model.read";
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"revision", revision}, {"files", siblings.size()}};
            return ApiResult::success({{"id", repository->id},
                                       {"modelId", repository->name},
                                       {"sha", resolved_revision},
                                       {"private", repository->visibility != "public"},
                                       {"card", card->contents},
                                       {"card_path", card->path},
                                       {"siblings", std::move(siblings)},
                                       {"provenance_complete", complete},
                                       {"provenance", std::move(provenance)}});
        }});

    registry.add(Capability{
        .name = "resolve_model_file",
        .title = "Resolve model file",
        .description = "Resolve one model file at an immutable repository revision.",
        .input_schema = model_input_schema(true),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "model repository",
        .action = "read",
        .resolve_object = model_object,
        .allows_anonymous_public_read = true,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = readable_model_repository(input, context);
            if (!repository) {
                return model_error(repository.error());
            }
            if (!context.sync_store) {
                return ApiResult::failure("store_unavailable", "no repository store is configured");
            }
            const auto file =
                context.sync_store->repository_file(repository->customer_id,
                                                    repository->id,
                                                    input.at("revision").get<std::string>(),
                                                    input.at("path").get<std::string>());
            if (!file) {
                return sync_error(file.error());
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_kind = "model.file.read";
            context.audit_object = "repository/" + repository->id + "/path/" + file->path;
            context.audit_details = {{"blob", file->blob}, {"path", file->path}};
            return ApiResult::success({{"path", file->path},
                                       {"blob", file->blob},
                                       {"size", file->size},
                                       {"contents", file->contents}});
        }});
}

} // namespace lake::api
