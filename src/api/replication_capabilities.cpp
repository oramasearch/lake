#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <liquid/transparency.hpp>

#include "lake/api/mcp_client.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"
#include "lake/transparency/checkpoint.hpp"
#include "lake/transparency/log.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_transparency_error(const transparency::Error& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult from_sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<std::string, ApiError> instance_object(const json&, ApiContext&) {
    return std::string{};
}

std::expected<std::string, ApiError> repository_object(const json& input, ApiContext& context) {
    const auto object = context.store->find_policy_object("repository", input.at("repository_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/repository_id"});
    }
    return object->id;
}

json state_json(const transparency::LogState& state) {
    return {{"root_hash", state.root_hash}, {"tree_size", state.tree_size}};
}

std::expected<transparency::LogState, ApiError> state_from_json(const json& value,
                                                                std::string path) {
    if (!value.is_object() || !value.contains("tree_size") ||
        !value.at("tree_size").is_number_unsigned() || !value.contains("root_hash") ||
        !value.at("root_hash").is_string()) {
        return std::unexpected(
            ApiError{"unrecoverable", "replication peer returned a malformed log state", path});
    }
    return transparency::LogState{value.at("tree_size").get<std::uint64_t>(),
                                  value.at("root_hash").get<std::string>()};
}

std::expected<transparency::LogReplicationBatch, ApiError> batch_from_json(const json& value) {
    if (!value.is_object() || value.value("version", "") != "lake_log_replication/1" ||
        !value.contains("source_instance") || !value.at("source_instance").is_string() ||
        value.at("source_instance").get_ref<const std::string&>().empty() ||
        !value.contains("source_tree_size") || !value.at("source_tree_size").is_number_unsigned() ||
        !value.contains("base") || !value.contains("head") || !value.contains("attestations") ||
        !value.at("attestations").is_array()) {
        return std::unexpected(
            ApiError{"unrecoverable", "replication peer returned a malformed batch", {}});
    }
    const auto base = state_from_json(value.at("base"), "/base");
    if (!base) {
        return std::unexpected(base.error());
    }
    const auto head = state_from_json(value.at("head"), "/head");
    if (!head) {
        return std::unexpected(head.error());
    }
    std::vector<std::string> attestations;
    attestations.reserve(value.at("attestations").size());
    for (const json& attestation : value.at("attestations")) {
        if (!attestation.is_string()) {
            return std::unexpected(ApiError{
                "unrecoverable", "replication peer returned a non-string attestation", {}});
        }
        attestations.push_back(attestation.get<std::string>());
    }
    return transparency::LogReplicationBatch{
        .base = *base,
        .head = *head,
        .source_tree_size = value.at("source_tree_size").get<std::uint64_t>(),
        .attestations = std::move(attestations),
    };
}

std::expected<liquid::transparency::hash, ApiError> replication_hash(const json& value) {
    if (!value.is_string()) {
        return std::unexpected(ApiError{"unrecoverable", "replication hash is malformed", {}});
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || !std::ranges::all_of(text, [](char digit) {
            return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
        })) {
        return std::unexpected(ApiError{"unrecoverable", "replication hash is malformed", {}});
    }
    const auto nibble = [](char digit) { return digit <= '9' ? digit - '0' : digit - 'a' + 10; };
    liquid::transparency::hash hash{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        hash[index] = std::byte((nibble(text[2 * index]) << 4) | nibble(text[2 * index + 1]));
    }
    return hash;
}

// Authenticate the checkpoint independently of the batch's self-consistent Merkle roots.
// A partial batch must prove its head is a prefix of that signed checkpoint before any write.
std::expected<json, ApiError> checkpoint_from_response(
    const json& response, const transparency::LogReplicationBatch& batch, ApiContext& context) {
    if (!response.contains("checkpoint")) {
        return std::unexpected(
            ApiError{"unrecoverable", "replication requires an authenticated checkpoint", {}});
    }
    if (!context.store || !context.checkpoints) {
        return std::unexpected(
            ApiError{"store_unavailable", "checkpoint replication is not configured", {}});
    }
    const json& record = response.at("checkpoint");
    if (!record.is_object() || !record.contains("checkpoint") ||
        !record.at("checkpoint").is_object() || !record.at("checkpoint").contains("tree_size") ||
        !record.at("checkpoint").at("tree_size").is_number_unsigned() ||
        !record.at("checkpoint").contains("root_hash") || !record.contains("signature")) {
        return std::unexpected(
            ApiError{"unrecoverable", "replication peer returned a malformed checkpoint", {}});
    }
    const json& checkpoint = record.at("checkpoint");
    const auto size = checkpoint.at("tree_size").get<std::uint64_t>();
    if (size > batch.source_tree_size || size < batch.head.tree_size) {
        return std::unexpected(
            ApiError{"unrecoverable", "replicated checkpoint does not cover the batch head", {}});
    }
    const auto verified =
        audit::verify_transparency_checkpoint(*context.store, checkpoint, record.at("signature"));
    if (!verified) {
        return std::unexpected(ApiError{verified.error().code, verified.error().message, {}});
    }
    const auto head_hash = replication_hash(batch.head.root_hash);
    const auto checkpoint_hash = replication_hash(checkpoint.at("root_hash"));
    if (!head_hash || !checkpoint_hash) {
        return std::unexpected(head_hash ? checkpoint_hash.error() : head_hash.error());
    }
    liquid::transparency::consistency_proof proof{
        .old_tree_size = batch.head.tree_size,
        .new_tree_size = size,
        .old_root_hash = *head_hash,
        .new_root_hash = *checkpoint_hash,
    };
    if (size != batch.head.tree_size) {
        if (!response.contains("checkpoint_consistency") ||
            !response.at("checkpoint_consistency").is_array() ||
            response.at("checkpoint_consistency").size() > 64) {
            return std::unexpected(
                ApiError{"unrecoverable",
                         "partial replication requires a checkpoint consistency proof",
                         {}});
        }
        for (const auto& item : response.at("checkpoint_consistency")) {
            const auto hash = replication_hash(item);
            if (!hash) {
                return std::unexpected(hash.error());
            }
            proof.audit_path.push_back(*hash);
        }
    }
    const liquid::transparency::checkpoint head{.tree_size = batch.head.tree_size,
                                                .root_hash = *head_hash};
    const liquid::transparency::checkpoint signed_head{.tree_size = size,
                                                       .root_hash = *checkpoint_hash};
    proof.bound_checkpoint = signed_head;
    if (const auto consistent = proof.verify(head, signed_head); !consistent) {
        return std::unexpected(ApiError{
            "unrecoverable", "replication head is not committed by the signed checkpoint", {}});
    }
    return record;
}

std::expected<std::uint64_t, ApiError> adopt_reached_checkpoint(
    const json& record, const transparency::AppendResult& applied, ApiContext& context) {
    const json& checkpoint = record.at("checkpoint");
    const std::uint64_t checkpoint_size = checkpoint.at("tree_size").get<std::uint64_t>();
    if (checkpoint_size > applied.tree_size) {
        return 0;
    }
    const auto committed = context.transparency_log->state_at(checkpoint_size);
    if (!committed || committed->root_hash != checkpoint.at("root_hash").get<std::string>()) {
        return std::unexpected(
            ApiError{"unrecoverable", "replicated checkpoint does not commit to local tiles", {}});
    }
    const auto adopted = context.checkpoints->adopt(record);
    if (!adopted) {
        return std::unexpected(ApiError{adopted.error().code, adopted.error().message, {}});
    }
    return adopted->at("sequence").get<std::uint64_t>();
}

std::expected<std::uint64_t, ApiError> replication_cursor(const json& input) {
    const json& value = input.at("after_tree_size");
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) {
        return std::unexpected(
            ApiError{"invalid_input", "after_tree_size must not be negative", "/after_tree_size"});
    }
    return static_cast<std::uint64_t>(value.get<std::int64_t>());
}

json object_schema() {
    return {{"type", "object"}};
}

// The source origin a replication may pull from (lake#915): a configured peer, or a refusal that
// names LAKE_REPLICATION_PEERS. Nothing is sent anywhere before this passes, so the instance's
// replication credential only ever reaches an origin the operator configured.
std::expected<std::string, ApiError> configured_peer(const json& input, const ApiContext& context) {
    const auto origin = replication_origin(input.at("source_url").get_ref<const std::string&>());
    if (!origin) {
        return std::unexpected(ApiError{"invalid_input", origin.error(), "/source_url"});
    }
    if (std::ranges::find(context.replication_peers, *origin) == context.replication_peers.end()) {
        return std::unexpected(ApiError{"invalid_input",
                                        "source_url " + *origin +
                                            " is not a configured replication peer "
                                            "(LAKE_REPLICATION_PEERS)",
                                        "/source_url"});
    }
    return *origin;
}

} // namespace

std::expected<std::string, std::string> replication_origin(std::string_view url) {
    while (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    const auto parsed = parse_mcp_url(url, true);
    if (!parsed) {
        return std::unexpected("a replication source is an https origin: " + parsed.error());
    }
    if (parsed->path != "/") {
        return std::unexpected("a replication source is an origin, with no path or query");
    }
    return parsed->origin();
}

std::expected<std::vector<std::string>, std::string>
parse_replication_peers(std::string_view list) {
    std::vector<std::string> peers;
    while (!list.empty()) {
        const std::size_t end = list.find_first_of(", \t\n");
        const std::string_view entry = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        if (entry.empty()) {
            continue;
        }
        auto origin = replication_origin(entry);
        if (!origin) {
            return std::unexpected("LAKE_REPLICATION_PEERS entry '" + std::string(entry) +
                                   "': " + origin.error());
        }
        if (std::ranges::find(peers, *origin) == peers.end()) {
            peers.push_back(std::move(*origin));
        }
    }
    return peers;
}

bool replication_peer_allowed(std::string_view url, const std::vector<std::string>& peers) {
    const auto origin = replication_origin(url);
    return origin && std::ranges::find(peers, *origin) != peers.end();
}

void register_replication_capabilities(Registry& registry) {
    registry.add(
        Capability{.name = "export_transparency_replication_batch",
                   .title = "Export transparency replication batch",
                   .description = "Export an append-only log suffix and its prefix commitments.",
                   .input_schema = {{"type", "object"},
                                    {"properties", {{"after_tree_size", {{"type", "integer"}}}}},
                                    {"required", json::array({"after_tree_size"})},
                                    {"additionalProperties", false}},
                   .output_schema = object_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "transparency log",
                   .action = "admin",
                   .resolve_object = instance_object,
                   .handler = [](const json& input, ApiContext& context) {
                       context.audit_kind = "replication";
                       context.audit_object = "transparency/log";
                       if (!context.transparency_log) {
                           return ApiResult::failure("store_unavailable",
                                                     "no tiled transparency log is configured");
                       }
                       const auto after = replication_cursor(input);
                       if (!after) {
                           return ApiResult::failure(
                               after.error().code, after.error().message, after.error().path);
                       }
                       const auto batch = context.transparency_log->replication_batch(*after);
                       if (!batch) {
                           return from_transparency_error(batch.error());
                       }
                       json value{{"attestations", batch->attestations},
                                  {"base", state_json(batch->base)},
                                  {"head", state_json(batch->head)},
                                  {"source_instance", context.node_name},
                                  {"source_tree_size", batch->source_tree_size},
                                  {"version", "lake_log_replication/1"}};
                       if (!context.checkpoints) {
                           return ApiResult::failure("store_unavailable",
                                                     "checkpoint replication is not configured");
                       }
                       const auto checkpoint = context.checkpoints->current();
                       if (!checkpoint) {
                           return from_transparency_error(checkpoint.error());
                       }
                       const auto& body = checkpoint->at("checkpoint");
                       if (body.at("tree_size") != batch->source_tree_size) {
                           return ApiResult::failure(
                               "conflict", "publish a checkpoint covering the source log");
                       }
                       const auto checkpoint_root = body.at("root_hash").get<std::string>();
                       const auto proof = context.transparency_log->consistency_proof(
                           batch->head.tree_size, body.at("tree_size").get<std::uint64_t>());
                       if (!proof || proof->new_root_hash != checkpoint_root ||
                           proof->old_root_hash != batch->head.root_hash) {
                           return ApiResult::failure("unrecoverable",
                                                     "checkpoint does not commit to the batch");
                       }
                       value["checkpoint"] = *checkpoint;
                       value["checkpoint_consistency"] = proof->audit_path;
                       context.audit_details = {{"after_tree_size", *after},
                                                {"entries", batch->attestations.size()},
                                                {"head_tree_size", batch->head.tree_size}};
                       return ApiResult::success(std::move(value));
                   }});

    registry.add(Capability{
        .name = "replicate_transparency_log",
        .title = "Replicate transparency log",
        .description = "Pull and verify the next append-only log suffix from another Lake.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"source_url", {{"type", "string"}, {"minLength", 1}}}}},
                         {"required", json::array({"source_url"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            context.audit_kind = "replication";
            context.audit_object = "transparency/log";
            if (!context.transparency_log || !context.replication_requester) {
                return ApiResult::failure("store_unavailable",
                                          "transparency replication is not configured");
            }
            const auto peer = configured_peer(input, context);
            if (!peer) {
                context.audit_details = {{"refused_source_url", input.at("source_url")}};
                return ApiResult::failure(
                    peer.error().code, peer.error().message, peer.error().path);
            }
            const auto local = context.transparency_log->state();
            if (!local) {
                return from_transparency_error(local.error());
            }
            const std::string& source_url = *peer;
            const auto response = context.replication_requester(source_url, local->tree_size);
            if (!response) {
                return ApiResult::failure(
                    response.error().code, response.error().message, response.error().path);
            }
            const auto batch = batch_from_json(*response);
            if (!batch) {
                return ApiResult::failure(
                    batch.error().code, batch.error().message, batch.error().path);
            }
            const auto checkpoint = checkpoint_from_response(*response, *batch, context);
            if (!checkpoint) {
                return ApiResult::failure(
                    checkpoint.error().code, checkpoint.error().message, checkpoint.error().path);
            }
            const auto applied = context.transparency_log->apply_replication_batch(*batch);
            if (!applied) {
                return from_transparency_error(applied.error());
            }
            const auto checkpoint_sequence =
                adopt_reached_checkpoint(*checkpoint, *applied, context);
            if (!checkpoint_sequence) {
                return ApiResult::failure(checkpoint_sequence.error().code,
                                          checkpoint_sequence.error().message,
                                          checkpoint_sequence.error().path);
            }
            context.audit_details = {{"entries", batch->attestations.size()},
                                     {"source_url", source_url},
                                     {"tree_size", applied->tree_size}};
            return ApiResult::success({{"caught_up", applied->tree_size == batch->source_tree_size},
                                       {"checkpoint_sequence", *checkpoint_sequence},
                                       {"entries_applied", batch->attestations.size()},
                                       {"root_hash", applied->root_hash},
                                       {"source_tree_size", batch->source_tree_size},
                                       {"tree_size", applied->tree_size}});
        }});

    registry.add(Capability{
        .name = "replicate_repository",
        .title = "Replicate repository",
        .description = "Pull verified liquid objects and refs from another Lake instance.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"repository_id", {{"type", "string"}}},
                           {"source_instance", {{"type", "string"}, {"minLength", 1}}},
                           {"source_url", {{"type", "string"}, {"minLength", 1}}}}},
                         {"required",
                          json::array({"repository_id", "source_instance", "source_url"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "admin",
        .resolve_object = repository_object,
        .resolves_operator_audit_target = true,
        .handler = [](const json& input, ApiContext& context) {
            context.audit_kind = "replication";
            if (!context.store || !context.sync_store) {
                return ApiResult::failure("store_unavailable",
                                          "repository replication is not configured");
            }
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return from_store_error(repository.error());
            }
            if (!repository->deleted_at.empty()) {
                return ApiResult::failure("conflict", "repository is tombstoned");
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_object = "repository/" + repository->id;
            // The replication credential goes only to a configured peer (lake#915).
            const auto peer = configured_peer(input, context);
            if (!peer) {
                context.audit_details = {{"refused_source_url", input.at("source_url")}};
                return ApiResult::failure(
                    peer.error().code, peer.error().message, peer.error().path);
            }
            const std::string source_url = *peer + "/v1/repositories/" + repository->id;
            const auto replicated =
                context.sync_store->replicate_from(repository->customer_id,
                                                   repository->id,
                                                   input.at("source_instance").get<std::string>(),
                                                   source_url,
                                                   context.replication_token);
            if (!replicated) {
                return from_sync_error(replicated.error());
            }
            const auto references =
                context.sync_store->refs(repository->customer_id, repository->id);
            if (!references) {
                return from_sync_error(references.error());
            }
            context.audit_details = {{"metadata_items", replicated->metadata_items},
                                     {"received_bytes", replicated->received_bytes},
                                     {"received_objects", replicated->received_objects},
                                     {"references", references->size()},
                                     {"source_instance", input.at("source_instance")}};
            return ApiResult::success({{"cursor", replicated->cursor},
                                       {"metadata_items", replicated->metadata_items},
                                       {"received_bytes", replicated->received_bytes},
                                       {"received_objects", replicated->received_objects},
                                       {"references", references->size()}});
        }});
}

} // namespace lake::api
