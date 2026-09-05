#include "lake/api/capabilities.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "lake/audit/audit.hpp"
#include "lake/transparency/checkpoint.hpp"
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
        .description =
            "Sign the current TUF metadata state and publish it after witness and anchor checks.",
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
            const auto root = context.transparency->metadata("root");
            if (!root) {
                return from_transparency_error(root.error());
            }
            const auto timestamp = context.transparency->metadata("timestamp");
            if (!timestamp) {
                return from_transparency_error(timestamp.error());
            }
            const json checkpoint{{"root", root->document},
                                  {"root_version", root->version},
                                  {"timestamp", timestamp->document},
                                  {"timestamp_version", timestamp->version},
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
