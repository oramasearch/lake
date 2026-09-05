#include "lake/api/capabilities.hpp"

#include <expected>
#include <string>

#include "lake/transparency/timestamps.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_timestamp_error(const transparency::Error& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_timestamp_store() {
    return ApiResult::failure("store_unavailable",
                              "no timestamp evidence store is configured (LAKE_STORE_ROOT)");
}

ApiResult no_checkpoint_store() {
    return ApiResult::failure("store_unavailable",
                              "no transparency checkpoint store is configured (LAKE_STORE_ROOT)");
}

std::expected<std::string, ApiError> instance_object(const json&, ApiContext&) {
    return std::string{};
}

json evidence_value(const transparency::TimestampEvidence& evidence) {
    json timestamps = json::array();
    for (const auto& timestamp : evidence.archive_timestamps) {
        timestamps.push_back({{"sequence", timestamp.sequence},
                              {"generated_at", timestamp.generated_at},
                              {"policy", timestamp.policy},
                              {"token_sha256", timestamp.token_sha256}});
    }
    return {{"checkpoint_digest", evidence.checkpoint_digest},
            {"signature_digest", evidence.signature_digest},
            {"digest_algorithm", evidence.digest_algorithm},
            {"evidence_record", evidence.evidence_record},
            {"archive_timestamps", std::move(timestamps)},
            {"renew_after", evidence.renew_after}};
}

json evidence_output_schema() {
    return {{"type", "object"},
            {"properties",
             {{"checkpoint_digest", {{"type", "string"}}},
              {"signature_digest", {{"type", "string"}}},
              {"digest_algorithm", {{"type", "string"}}},
              {"evidence_record", {{"type", "string"}}},
              {"archive_timestamps", {{"type", "array"}}},
              {"renew_after", {{"type", "string"}}}}}};
}

void set_audit_fields(ApiContext& context,
                      std::string operation,
                      std::string checkpoint_digest = {}) {
    context.audit_kind = "transparency";
    context.audit_object = "transparency/log";
    context.audit_details = {{"operation", std::move(operation)}};
    if (!checkpoint_digest.empty()) {
        context.audit_details["checkpoint_digest"] = std::move(checkpoint_digest);
    }
}

} // namespace

void register_timestamp_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "timestamp_checkpoint",
        .title = "Timestamp checkpoint signature",
        .description =
            "Obtain an RFC 3161 token over the current checkpoint signature and create RFC 4998 "
            "evidence.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = evidence_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json&, ApiContext& context) {
            set_audit_fields(context, "timestamp_checkpoint");
            if (!context.timestamping) {
                return no_timestamp_store();
            }
            if (!context.checkpoints) {
                return no_checkpoint_store();
            }
            const auto checkpoint = context.checkpoints->current();
            if (!checkpoint) {
                return from_timestamp_error(checkpoint.error());
            }
            if (!checkpoint->contains("checkpoint_hash_sha256") ||
                !checkpoint->at("checkpoint_hash_sha256").is_string() ||
                !checkpoint->contains("signature") || !checkpoint->at("signature").is_object() ||
                !checkpoint->at("signature").contains("signature") ||
                !checkpoint->at("signature").at("signature").is_string()) {
                return ApiResult::failure("store_error",
                                          "current checkpoint has no signature value or digest");
            }
            const std::string checkpoint_digest =
                checkpoint->at("checkpoint_hash_sha256").get<std::string>();
            context.audit_details["checkpoint_digest"] = checkpoint_digest;
            const auto evidence = context.timestamping->timestamp(
                checkpoint_digest,
                checkpoint->at("signature").at("signature").get<std::string>(),
                context.tsa_endpoint,
                context.tsa_trust_pem,
                context.timestamp_policy,
                context.tsa_sender);
            if (!evidence) {
                return from_timestamp_error(evidence.error());
            }
            context.audit_details["archive_timestamps"] = evidence->archive_timestamps.size();
            context.audit_details["signature_digest"] = evidence->signature_digest;
            return ApiResult::success(evidence_value(*evidence));
        }});
    registry.add(Capability{
        .name = "renew_timestamp_evidence",
        .title = "Renew timestamp evidence",
        .description =
            "Append an RFC 3161 archive timestamp covering the prior RFC 4998 archive timestamp.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"checkpoint_digest", {{"type", "string"}}}}},
                         {"required", json::array({"checkpoint_digest"})},
                         {"additionalProperties", false}},
        .output_schema = evidence_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "transparency log",
        .action = "admin",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string checkpoint_digest = input.at("checkpoint_digest").get<std::string>();
            set_audit_fields(context, "renew_timestamp_evidence", checkpoint_digest);
            if (!context.timestamping) {
                return no_timestamp_store();
            }
            const auto evidence = context.timestamping->renew(checkpoint_digest,
                                                              context.tsa_endpoint,
                                                              context.tsa_trust_pem,
                                                              context.timestamp_policy,
                                                              context.tsa_sender);
            if (!evidence) {
                return from_timestamp_error(evidence.error());
            }
            context.audit_details["archive_timestamps"] = evidence->archive_timestamps.size();
            return ApiResult::success(evidence_value(*evidence));
        }});
    registry.add(Capability{
        .name = "timestamp_evidence",
        .title = "Read timestamp evidence",
        .description = "Read and verify the RFC 4998 evidence record for a checkpoint signature.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"checkpoint_digest", {{"type", "string"}}}}},
                         {"required", json::array({"checkpoint_digest"})},
                         {"additionalProperties", false}},
        .output_schema = evidence_output_schema(),
        .kind = CapabilityKind::Read,
        .object = "transparency log",
        .action = "read",
        .resolve_object = instance_object,
        .handler = [](const json& input, ApiContext& context) {
            const std::string checkpoint_digest = input.at("checkpoint_digest").get<std::string>();
            set_audit_fields(context, "read_timestamp_evidence", checkpoint_digest);
            if (!context.timestamping) {
                return no_timestamp_store();
            }
            const auto evidence =
                context.timestamping->evidence(checkpoint_digest, context.timestamp_policy);
            if (!evidence) {
                return from_timestamp_error(evidence.error());
            }
            context.audit_details["archive_timestamps"] = evidence->archive_timestamps.size();
            return ApiResult::success(evidence_value(*evidence));
        }});
}

} // namespace lake::api
