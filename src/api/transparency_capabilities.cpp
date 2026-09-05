#include "lake/api/capabilities.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "lake/audit/audit.hpp"
#include "lake/transparency/checkpoint.hpp"
#include "lake/transparency/log.hpp"
#include "lake/transparency/tuf.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_transparency_error(const transparency::Error& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_transparency() {
    return ApiResult::failure("store_unavailable",
                              "no transparency store is configured (LAKE_STORE_ROOT)");
}

ApiResult no_checkpoint_service() {
    return ApiResult::failure("store_unavailable",
                              "no transparency checkpoint service is configured (LAKE_STORE_ROOT)");
}

ApiResult no_tiled_log() {
    return ApiResult::failure("store_unavailable",
                              "no tiled transparency log is configured (LAKE_STORE_ROOT)");
}

std::expected<std::string, ApiError> instance_object(const json&, ApiContext&) {
    return std::string{};
}

json metadata_value(const transparency::Metadata& metadata) {
    return {{"role", metadata.role},
            {"version", metadata.version},
            {"encoded", metadata.encoded},
            {"metadata", metadata.document}};
}

void set_audit_fields(ApiContext& context, std::string operation) {
    context.audit_kind = "transparency";
    context.audit_object = "transparency/log";
    context.audit_details = {{"operation", std::move(operation)}};
}

json metadata_output_schema() {
    return {{"type", "object"},
            {"properties",
             {{"role", {{"type", "string"}}},
              {"version", {{"type", "integer"}}},
              {"encoded", {{"type", "string"}}},
              {"metadata", {{"type", "object"}}}}}};
}

json checkpoint_output_schema() {
    return {{"type", "object"}};
}

json append_value(const transparency::AppendResult& appended) {
    return {{"first_index", appended.first_index},
            {"root_hash", appended.root_hash},
            {"tile_width", transparency::transparency_tile_width},
            {"tiles_published", appended.tiles_published},
            {"tree_size", appended.tree_size}};
}

json tile_value(const transparency::Tile& tile) {
    return {{"encoding", "base64url"},
            {"entries", tile.width},
            {"index", tile.index},
            {"sha256", tile.sha256},
            {"tile", tile.encoded},
            {"version", "lake_tile/1"}};
}

json scan_value(const transparency::IdentityScan& scan) {
    json matches = json::array();
    for (const transparency::IdentityMatch& match : scan.matches) {
        matches.push_back({{"attestation", match.attestation},
                           {"index", match.index},
                           {"issuer", match.issuer},
                           {"profile", match.profile},
                           {"subject", match.subject}});
    }
    return {{"duration_ms", scan.duration_ms},
            {"entries_scanned", scan.entries_scanned},
            {"identity", scan.identity},
            {"matches", std::move(matches)},
            {"root_hash", scan.root_hash},
            {"tiles_scanned", scan.tiles_scanned},
            {"tree_size", scan.tree_size},
            {"verified", true}};
}

std::expected<nlohmann::json, transparency::Error> sign_checkpoint(const nlohmann::json& checkpoint,
                                                                   ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            transparency::Error{"store_unavailable", "no control store is configured"});
    }
    const auto signed_checkpoint = audit::sign_transparency_checkpoint(
        *context.store,
        checkpoint,
        {.configured_seed = context.identity_seed, .instance_kek = context.instance_kek});
    if (!signed_checkpoint) {
        return std::unexpected(
            transparency::Error{signed_checkpoint.error().code, signed_checkpoint.error().message});
    }
    return *signed_checkpoint;
}

} // namespace

void register_transparency_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "append_transparency_entries",
        .title = "Append transparency entries",
        .description =
            "Verify canonical TAAP attestations and append them to immutable bulk tiles.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"attestations", {{"type", "array"}}}}},
                         {"required", json::array({"attestations"})},
                         {"additionalProperties", false}},
        .output_schema = checkpoint_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "append_entries");
            if (!context.transparency_log) {
                return no_tiled_log();
            }
            const json& input_attestations = input.at("attestations");
            std::vector<std::string> attestations;
            attestations.reserve(input_attestations.size());
            for (std::size_t index = 0; index < input_attestations.size(); ++index) {
                if (!input_attestations[index].is_string()) {
                    return ApiResult::failure("invalid_input",
                                              "attestation must be a base64url string",
                                              "/attestations/" + std::to_string(index));
                }
                attestations.push_back(input_attestations[index].get<std::string>());
            }
            const auto appended = context.transparency_log->append(attestations);
            if (!appended) {
                return from_transparency_error(appended.error());
            }
            context.audit_details["first_index"] = appended->first_index;
            context.audit_details["tree_size"] = appended->tree_size;
            context.audit_details["entries_appended"] = attestations.size();
            return ApiResult::success(append_value(*appended));
        }});
    registry.add(Capability{
        .name = "transparency_tile",
        .title = "Fetch transparency tile",
        .description = "Fetch one immutable bulk entry tile for public monitoring.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"index", {{"type", "integer"}}}, {"width", {{"type", "integer"}}}}},
                         {"required", json::array({"index", "width"})},
                         {"additionalProperties", false}},
        .output_schema = checkpoint_output_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "transparency log",
        .action = "read",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "read_tile");
            if (!context.transparency_log) {
                return no_tiled_log();
            }
            const std::int64_t index = input.at("index").get<std::int64_t>();
            const std::int64_t width = input.at("width").get<std::int64_t>();
            if (index < 0) {
                return ApiResult::failure("invalid_input", "index must not be negative", "/index");
            }
            if (width <= 0 ||
                width > static_cast<std::int64_t>(transparency::transparency_tile_width)) {
                return ApiResult::failure(
                    "invalid_input", "width must be between 1 and 1024", "/width");
            }
            const auto tile = context.transparency_log->tile(static_cast<std::uint64_t>(index),
                                                             static_cast<std::size_t>(width));
            if (!tile) {
                return from_transparency_error(tile.error());
            }
            context.audit_details["tile_index"] = tile->index;
            context.audit_details["tile_width"] = tile->width;
            return ApiResult::success(tile_value(*tile));
        }});
    registry.add(Capability{
        .name = "transparency_identity_scan",
        .title = "Scan transparency identity",
        .description =
            "Verify checkpoint-bound bulk tiles and return every attestation naming one identity.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"identity", {{"type", "string"}}}}},
                         {"required", json::array({"identity"})},
                         {"additionalProperties", false}},
        .output_schema = checkpoint_output_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "transparency log",
        .action = "read",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "identity_scan");
            if (!context.transparency_log) {
                return no_tiled_log();
            }
            if (!context.checkpoints) {
                return no_checkpoint_service();
            }
            const auto current = context.checkpoints->current();
            if (!current) {
                return from_transparency_error(current.error());
            }
            if (!current->contains("checkpoint") || !current->at("checkpoint").is_object() ||
                !current->at("checkpoint").contains("tree_size") ||
                !current->at("checkpoint").at("tree_size").is_number_unsigned() ||
                !current->at("checkpoint").contains("root_hash") ||
                !current->at("checkpoint").at("root_hash").is_string()) {
                return ApiResult::failure("unrecoverable",
                                          "current checkpoint has no tiled-log commitment");
            }
            const json& checkpoint = current->at("checkpoint");
            const std::string identity = input.at("identity").get<std::string>();
            const auto scan = context.transparency_log->scan_identity(
                identity,
                checkpoint.at("tree_size").get<std::uint64_t>(),
                checkpoint.at("root_hash").get<std::string>());
            if (!scan) {
                return from_transparency_error(scan.error());
            }
            context.audit_details["identity"] = identity;
            context.audit_details["entries_scanned"] = scan->entries_scanned;
            context.audit_details["matches"] = scan->matches.size();
            return ApiResult::success(scan_value(*scan));
        }});
    registry.add(Capability{
        .name = "initialize_tuf",
        .title = "Initialize TUF trust",
        .description =
            "Initialize the instance transparency log with threshold-signed TUF root metadata.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"root", {{"type", "object"}}}}},
                         {"required", json::array({"root"})},
                         {"additionalProperties", false}},
        .output_schema = metadata_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "initialize");
            if (!context.transparency) {
                return no_transparency();
            }
            const auto initialized = context.transparency->initialize(input.at("root"));
            if (!initialized) {
                return from_transparency_error(initialized.error());
            }
            context.audit_details["root_version"] = initialized->version;
            return ApiResult::success(metadata_value(*initialized));
        }});
    registry.add(Capability{
        .name = "publish_tuf_metadata",
        .title = "Publish TUF metadata",
        .description = "Verify and append a signed targets, snapshot, and timestamp metadata set.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"targets", {{"type", "object"}}},
                           {"snapshot", {{"type", "object"}}},
                           {"timestamp", {{"type", "object"}}}}},
                         {"required", json::array({"targets", "snapshot", "timestamp"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"root_version", {{"type", "integer"}}},
                            {"targets_version", {{"type", "integer"}}},
                            {"snapshot_version", {{"type", "integer"}}},
                            {"timestamp_version", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "publish");
            if (!context.transparency) {
                return no_transparency();
            }
            const auto published = context.transparency->publish(
                input.at("targets"), input.at("snapshot"), input.at("timestamp"));
            if (!published) {
                return from_transparency_error(published.error());
            }
            context.audit_details.update(*published);
            return ApiResult::success(*published);
        }});
    registry.add(Capability{
        .name = "rotate_tuf_root",
        .title = "Rotate TUF root",
        .description = "Append the next root after checking trusted-root and new-root thresholds.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"root", {{"type", "object"}}}}},
                         {"required", json::array({"root"})},
                         {"additionalProperties", false}},
        .output_schema = metadata_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "rotate_root");
            if (!context.transparency) {
                return no_transparency();
            }
            const auto rotated = context.transparency->rotate_root(input.at("root"));
            if (!rotated) {
                return from_transparency_error(rotated.error());
            }
            context.audit_details["root_version"] = rotated->version;
            return ApiResult::success(metadata_value(*rotated));
        }});
    registry.add(Capability{
        .name = "tuf_metadata",
        .title = "Read TUF metadata",
        .description = "Read the latest or a named immutable TUF metadata role version.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"role", {{"type", "string"}}}, {"version", {{"type", "integer"}}}}},
                         {"required", json::array({"role"})},
                         {"additionalProperties", false}},
        .output_schema = metadata_output_schema(),
        .kind = CapabilityKind::Read,
        .object = "transparency log",
        .action = "read",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            set_audit_fields(context, "read");
            if (!context.transparency) {
                return no_transparency();
            }
            std::optional<std::uint64_t> version;
            if (input.contains("version")) {
                const std::int64_t requested = input.at("version").get<std::int64_t>();
                if (requested <= 0) {
                    return ApiResult::failure(
                        "invalid_input", "version must be positive", "/version");
                }
                version = static_cast<std::uint64_t>(requested);
            }
            const std::string role = input.at("role").get<std::string>();
            const auto metadata = context.transparency->metadata(role, version);
            if (!metadata) {
                return from_transparency_error(metadata.error());
            }
            context.audit_details["role"] = role;
            context.audit_details["version"] = metadata->version;
            return ApiResult::success(metadata_value(*metadata));
        }});
    registry.add(Capability{
        .name = "transparency_log_configuration",
        .title = "Read transparency-log configuration",
        .description =
            "Read the effective transparency log mode, witness quorum, and anchoring endpoint.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = checkpoint_output_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "transparency log",
        .action = "read",
        .resolve_object = instance_object,
        .handler = [](const json&, ApiContext& context) {
            set_audit_fields(context, "configuration");
            if (!context.checkpoints) {
                return no_checkpoint_service();
            }
            return ApiResult::success(context.checkpoints->configuration());
        }});
    registry.add(Capability{
        .name = "publish_transparency_checkpoint",
        .title = "Publish transparency checkpoint",
        .description = "Sign the current TUF and tiled-log state after witness and anchor checks.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = checkpoint_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json&, ApiContext& context) {
            set_audit_fields(context, "publish_checkpoint");
            if (!context.transparency) {
                return no_transparency();
            }
            if (!context.checkpoints) {
                return no_checkpoint_service();
            }
            if (!context.transparency_log) {
                return no_tiled_log();
            }
            const auto root = context.transparency->metadata("root");
            if (!root) {
                return from_transparency_error(root.error());
            }
            const auto timestamp = context.transparency->metadata("timestamp");
            if (!timestamp) {
                return from_transparency_error(timestamp.error());
            }
            const auto log_state = context.transparency_log->state();
            if (!log_state) {
                return from_transparency_error(log_state.error());
            }
            const json checkpoint{{"root", root->document},
                                  {"root_version", root->version},
                                  {"root_hash", log_state->root_hash},
                                  {"timestamp", timestamp->document},
                                  {"timestamp_version", timestamp->version},
                                  {"tile_width", transparency::transparency_tile_width},
                                  {"tree_size", log_state->tree_size},
                                  {"version", "lake_checkpoint/1"}};
            const auto published = context.checkpoints->publish(
                checkpoint,
                [&context](const json& value) { return sign_checkpoint(value, context); },
                context.transparency_submitter);
            if (!published) {
                return from_transparency_error(published.error());
            }
            context.audit_details["checkpoint_sequence"] = published->at("sequence");
            context.audit_details["mode"] = context.checkpoints->configuration().at("mode");
            context.audit_details["tree_size"] = log_state->tree_size;
            return ApiResult::success(*published);
        }});
    registry.add(Capability{
        .name = "transparency_checkpoint",
        .title = "Read current transparency checkpoint",
        .description =
            "Read the latest checkpoint that met the configured witness and anchor requirements.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = checkpoint_output_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "transparency log",
        .action = "read",
        .resolve_object = instance_object,
        .handler = [](const json&, ApiContext& context) {
            set_audit_fields(context, "read_checkpoint");
            if (!context.checkpoints) {
                return no_checkpoint_service();
            }
            const auto checkpoint = context.checkpoints->current();
            if (!checkpoint) {
                return from_transparency_error(checkpoint.error());
            }
            return ApiResult::success(*checkpoint);
        }});
}

} // namespace lake::api
