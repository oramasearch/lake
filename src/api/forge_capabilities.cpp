#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/hmac.h>

#include "lake/control/identity_crypto.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError& error) {
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

json record_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"repository_id", {{"type", "string"}}},
              {"kind", {{"type", "string"}}},
              {"value", {{"type", "object"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}

json records_schema() {
    return {{"type", "object"}, {"properties", {{"items", {{"type", "array"}}}}}};
}

json repository_input(json properties, json required) {
    properties["repository_id"] = {{"type", "string"}};
    required.push_back("repository_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

ApiResult create_record(const std::string& kind, const json& value, ApiContext& context) {
    if (!context.store) {
        return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    const auto record = context.store->create_forge_record(control::NewForgeRecord{
        value.at("repository_id").get<std::string>(), kind, value.at("value")});
    if (!record) {
        return from_store_error(record.error());
    }
    context.audit_object = "repository/" + record->repository_id;
    context.audit_details = {{"forge_kind", kind}, {"record_id", record->id}};
    return ApiResult::success(control::to_json(*record));
}

ApiResult list_records(const std::string& kind, const json& input, ApiContext& context) {
    if (!context.store) {
        return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    const auto records = context.store->list_forge_records(
        input.at("repository_id"), kind, input.value("limit", 100));
    if (!records) {
        return from_store_error(records.error());
    }
    json items = json::array();
    for (const control::ForgeRecord& record : *records) {
        json output = control::to_json(record);
        if (record.kind == "webhook") {
            output["value"].erase("secret");
        }
        items.push_back(std::move(output));
    }
    return ApiResult::success({{"items", std::move(items)}});
}

std::expected<control::ForgeRecord, ApiResult>
get_kind(const json& input, ApiContext& context, std::string_view kind, std::string_view id_name) {
    if (!context.store) {
        return std::unexpected(
            ApiResult::failure("store_unavailable", "no control-plane store is configured"));
    }
    const auto record = context.store->get_forge_record(input.at(std::string(id_name)));
    if (!record) {
        return std::unexpected(from_store_error(record.error()));
    }
    if (record->repository_id != input.at("repository_id").get<std::string>() ||
        record->kind != kind) {
        return std::unexpected(
            ApiResult::failure("not_found", "forge record does not match repository"));
    }
    return *record;
}

ApiResult
update_record(const control::ForgeRecord& record, const json& changes, ApiContext& context) {
    json value = record.value;
    for (const auto& [key, item] : changes.items()) {
        value[key] = item;
    }
    const auto updated = context.store->update_forge_record(record.id, value);
    if (!updated) {
        return from_store_error(updated.error());
    }
    context.audit_object = "repository/" + record.repository_id;
    context.audit_details = {{"forge_kind", record.kind}, {"record_id", record.id}};
    return ApiResult::success(control::to_json(*updated));
}

ApiResult create_issue(const json& input, ApiContext& context) {
    json value = input;
    value.erase("repository_id");
    value["state"] = "open";
    return create_record(
        "issue", {{"repository_id", input.at("repository_id")}, {"value", value}}, context);
}

ApiResult update_issue(const json& input, ApiContext& context) {
    const auto issue = get_kind(input, context, "issue", "issue_id");
    if (!issue) {
        return issue.error();
    }
    json changes = input;
    changes.erase("repository_id");
    changes.erase("issue_id");
    return update_record(*issue, changes, context);
}

ApiResult create_review(const json& input, ApiContext& context) {
    json value = input;
    value.erase("repository_id");
    value["state"] = "open";
    return create_record(
        "review", {{"repository_id", input.at("repository_id")}, {"value", value}}, context);
}

ApiResult update_review(const json& input, ApiContext& context) {
    const auto review = get_kind(input, context, "review", "review_id");
    if (!review) {
        return review.error();
    }
    json changes = input;
    changes.erase("repository_id");
    changes.erase("review_id");
    return update_record(*review, changes, context);
}

ApiResult create_issue_comment(const json& input, ApiContext& context) {
    const auto issue = get_kind(input, context, "issue", "issue_id");
    if (!issue) {
        return issue.error();
    }
    json value = input;
    value.erase("repository_id");
    return create_record(
        "issue_comment", {{"repository_id", input.at("repository_id")}, {"value", value}}, context);
}

ApiResult create_review_comment(const json& input, ApiContext& context) {
    const auto review = get_kind(input, context, "review", "review_id");
    if (!review) {
        return review.error();
    }
    const json& anchor = input.at("anchor");
    if (!anchor.contains("change") || !anchor.contains("path") || !anchor.contains("line") ||
        !anchor.contains("column") || !anchor.at("change").is_string() ||
        !anchor.at("path").is_string() || !anchor.at("line").is_number_integer() ||
        !anchor.at("column").is_number_integer() || anchor.at("line").get<std::int64_t>() < 0 ||
        anchor.at("column").get<std::int64_t>() < 0) {
        return ApiResult::failure(
            "invalid_input", "anchor requires change, path, line, and column", "/anchor");
    }
    json value = input;
    value.erase("repository_id");
    return create_record("review_comment",
                         {{"repository_id", input.at("repository_id")}, {"value", value}},
                         context);
}

ApiResult create_release(const json& input, ApiContext& context) {
    if (!context.store) {
        return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    if (!context.sync_store) {
        return ApiResult::failure("store_unavailable", "no repository store is configured");
    }
    const std::string repository_id = input.at("repository_id").get<std::string>();
    const auto repository = context.store->get_repository(repository_id);
    if (!repository) {
        return from_store_error(repository.error());
    }
    json artifacts = json::array();
    for (const json& artifact_id : input.at("artifact_ids")) {
        const auto artifact = context.sync_store->repository_artifact(
            repository->customer_id, repository_id, artifact_id.get<std::string>());
        if (!artifact) {
            return ApiResult::failure(artifact.error().code, artifact.error().message);
        }
        json attestations = json::array();
        for (const RepositoryArtifactAttestation& attestation : artifact->attestations) {
            if (!attestation.verified) {
                continue;
            }
            attestations.push_back({{"id", attestation.id},
                                    {"profile", attestation.profile},
                                    {"subject", attestation.subject},
                                    {"issuer", attestation.issuer}});
        }
        if (attestations.empty()) {
            return ApiResult::failure(
                "conflict", "release artifact has no verified attestation", "/artifact_ids");
        }
        artifacts.push_back({{"id", artifact->id},
                             {"action", artifact->action},
                             {"outputs", artifact->outputs.size()},
                             {"attestations", std::move(attestations)}});
    }
    json value = input;
    value.erase("repository_id");
    value.erase("artifact_ids");
    value["artifacts"] = std::move(artifacts);
    return create_record(
        "release", {{"repository_id", repository_id}, {"value", std::move(value)}}, context);
}

std::string webhook_signature(std::string_view secret, std::string_view payload) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (HMAC(EVP_sha256(),
             secret.data(),
             static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(payload.data()),
             payload.size(),
             digest,
             &size) == nullptr) {
        return {};
    }
    return "sha256=" + control::identity_crypto::base64url_encode(
                           std::string_view(reinterpret_cast<const char*>(digest), size));
}

json map_anchor(json anchor, const json& hunks, const std::string& target_change) {
    std::int64_t line = anchor.value("line", std::int64_t{0});
    std::int64_t shift = 0;
    std::string disposition = "same";
    for (const json& hunk : hunks) {
        const std::int64_t old_line = hunk.value("old_line", std::int64_t{0});
        const std::int64_t old_count = hunk.value("old_count", std::int64_t{0});
        const std::int64_t new_line = hunk.value("new_line", old_line);
        const std::int64_t new_count = hunk.value("new_count", std::int64_t{0});
        if (line < old_line) {
            break;
        }
        if (old_count == 0 || line >= old_line + old_count) {
            shift += new_count - old_count;
            continue;
        }
        anchor["line"] =
            new_count == 0 ? new_line : new_line + std::min(line - old_line, new_count - 1);
        anchor["change"] = target_change;
        anchor["disposition"] = new_count == 0 ? "deleted" : "approximate";
        return anchor;
    }
    anchor["line"] = std::max<std::int64_t>(0, line + shift);
    anchor["change"] = target_change;
    anchor["disposition"] = disposition == "same" && shift != 0 ? "shifted" : disposition;
    return anchor;
}

void add_create(Registry& registry,
                std::string name,
                std::string title,
                std::string kind,
                json properties,
                json required) {
    registry.add(
        Capability{.name = std::move(name),
                   .title = std::move(title),
                   .description = "Create a durable forge record.",
                   .input_schema = repository_input(std::move(properties), std::move(required)),
                   .output_schema = record_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "repository",
                   .action = "write",
                   .resolve_object = repository_object,
                   .handler = [kind = std::move(kind)](const json& input, ApiContext& context) {
                       json value = input;
                       value.erase("repository_id");
                       return create_record(kind,
                                            {{"repository_id", input.at("repository_id")},
                                             {"value", std::move(value)}},
                                            context);
                   }});
}

void add_list(Registry& registry, std::string name, std::string title, std::string kind) {
    registry.add(Capability{
        .name = std::move(name),
        .title = std::move(title),
        .description = "List durable forge records for one repository.",
        .input_schema = repository_input({{"limit", {{"type", "integer"}}}}, json::array()),
        .output_schema = records_schema(),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = repository_object,
        .handler = [kind = std::move(kind)](const json& input, ApiContext& context) {
            return list_records(kind, input, context);
        }});
}

} // namespace

void register_forge_capabilities(Registry& registry) {
    registry.add(Capability{.name = "create_issue",
                            .title = "Create issue",
                            .description = "Create an open Markdown issue.",
                            .input_schema = repository_input({{"title", {{"type", "string"}}},
                                                              {"body", {{"type", "string"}}},
                                                              {"labels", {{"type", "array"}}},
                                                              {"assignees", {{"type", "array"}}},
                                                              {"references", {{"type", "array"}}}},
                                                             json::array({"title"})),
                            .output_schema = record_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "repository",
                            .action = "write",
                            .resolve_object = repository_object,
                            .handler = create_issue});
    add_list(registry, "list_issues", "List issues", "issue");
    registry.add(
        Capability{.name = "update_issue",
                   .title = "Update issue",
                   .description = "Update an issue's fields or lifecycle state.",
                   .input_schema = repository_input(
                       {{"issue_id", {{"type", "string"}}},
                        {"title", {{"type", "string"}}},
                        {"body", {{"type", "string"}}},
                        {"labels", {{"type", "array"}}},
                        {"assignees", {{"type", "array"}}},
                        {"references", {{"type", "array"}}},
                        {"state", {{"type", "string"}, {"enum", json::array({"open", "closed"})}}}},
                       json::array({"issue_id"})),
                   .output_schema = record_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "repository",
                   .action = "write",
                   .resolve_object = repository_object,
                   .handler = update_issue});
    registry.add(Capability{.name = "create_issue_comment",
                            .title = "Comment on issue",
                            .description = "Add Markdown discussion to an issue.",
                            .input_schema = repository_input({{"issue_id", {{"type", "string"}}},
                                                              {"body", {{"type", "string"}}}},
                                                             json::array({"issue_id", "body"})),
                            .output_schema = record_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "repository",
                            .action = "write",
                            .resolve_object = repository_object,
                            .handler = create_issue_comment});
    add_list(registry, "list_issue_comments", "List issue comments", "issue_comment");

    registry.add(Capability{.name = "create_review",
                            .title = "Create change review",
                            .description = "Create an open review for one liquid change identity.",
                            .input_schema = repository_input({{"change_id", {{"type", "string"}}},
                                                              {"reviewers", {{"type", "array"}}}},
                                                             json::array({"change_id"})),
                            .output_schema = record_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "repository",
                            .action = "write",
                            .resolve_object = repository_object,
                            .handler = create_review});
    add_list(registry, "list_reviews", "List change reviews", "review");
    registry.add(Capability{
        .name = "update_review",
        .title = "Update change review",
        .description = "Set a review's lifecycle state.",
        .input_schema = repository_input(
            {{"review_id", {{"type", "string"}}},
             {"state",
              {{"type", "string"}, {"enum", json::array({"open", "approved", "closed"})}}}},
            json::array({"review_id", "state"})),
        .output_schema = record_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = repository_object,
        .handler = update_review});
    registry.add(
        Capability{.name = "create_review_comment",
                   .title = "Comment on reviewed change",
                   .description = "Attach Markdown discussion to a liquid change location.",
                   .input_schema = repository_input({{"review_id", {{"type", "string"}}},
                                                     {"body", {{"type", "string"}}},
                                                     {"anchor", {{"type", "object"}}}},
                                                    json::array({"review_id", "body", "anchor"})),
                   .output_schema = record_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "repository",
                   .action = "write",
                   .resolve_object = repository_object,
                   .handler = create_review_comment});
    add_list(registry, "list_review_comments", "List review comments", "review_comment");

    registry.add(Capability{
        .name = "map_review_anchors",
        .title = "Map review anchors",
        .description = "Map stored review anchors through structural line hunks.",
        .input_schema =
            repository_input({{"source_change", {{"type", "string"}}},
                              {"target_change", {{"type", "string"}}},
                              {"path", {{"type", "string"}}},
                              {"hunks", {{"type", "array"}}}},
                             json::array({"source_change", "target_change", "path", "hunks"})),
        .output_schema = records_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = repository_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto records = context.store->list_forge_records(
                input.at("repository_id"), "review_comment", 1000);
            if (!records) {
                return from_store_error(records.error());
            }
            json items = json::array();
            for (const control::ForgeRecord& record : *records) {
                json value = record.value;
                if (!value.contains("anchor") || !value["anchor"].is_object()) {
                    continue;
                }
                if (value["anchor"].value("change", "") !=
                        input.at("source_change").get<std::string>() ||
                    value["anchor"].value("path", "") != input.at("path").get<std::string>()) {
                    continue;
                }
                value["anchor"] =
                    map_anchor(value["anchor"], input.at("hunks"), input.at("target_change"));
                const auto updated = context.store->update_forge_record(record.id, value);
                if (!updated) {
                    return from_store_error(updated.error());
                }
                items.push_back(control::to_json(*updated));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});

    registry.add(Capability{.name = "create_release",
                            .title = "Create release",
                            .description = "Create a tag release with verified build artifacts.",
                            .input_schema =
                                repository_input({{"tag", {{"type", "string"}}},
                                                  {"change_id", {{"type", "string"}}},
                                                  {"artifact_ids", {{"type", "array"}}}},
                                                 json::array({"tag", "change_id", "artifact_ids"})),
                            .output_schema = record_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "repository",
                            .action = "write",
                            .resolve_object = repository_object,
                            .handler = create_release});
    add_list(registry, "list_releases", "List releases", "release");
    add_create(registry,
               "create_webhook",
               "Create signed webhook",
               "webhook",
               {{"url", {{"type", "string"}}},
                {"events", {{"type", "array"}}},
                {"secret", {{"type", "string"}}}},
               json::array({"url", "events", "secret"}));
    add_list(registry, "list_webhooks", "List webhooks", "webhook");

    registry.add(Capability{
        .name = "record_webhook_delivery",
        .title = "Record signed webhook delivery",
        .description = "Create a signed delivery attempt and durable event log record.",
        .input_schema =
            repository_input({{"webhook_id", {{"type", "string"}}},
                              {"event", {{"type", "string"}}},
                              {"payload", {{"type", "object"}}},
                              {"delivered", {{"type", "boolean"}}}},
                             json::array({"webhook_id", "event", "payload", "delivered"})),
        .output_schema = record_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = repository_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto webhook = get_kind(input, context, "webhook", "webhook_id");
            if (!webhook) {
                return webhook.error();
            }
            const std::string payload = input.at("payload").dump();
            const json value = {
                {"webhook_id", webhook->id},
                {"event", input.at("event")},
                {"payload", input.at("payload")},
                {"signature", webhook_signature(webhook->value.value("secret", ""), payload)},
                {"attempt", 1},
                {"delivered", input.at("delivered")}};
            return create_record("webhook_delivery",
                                 {{"repository_id", input.at("repository_id")}, {"value", value}},
                                 context);
        }});
    add_list(registry, "list_webhook_deliveries", "List webhook deliveries", "webhook_delivery");

    add_create(registry,
               "set_branch_protection",
               "Set branch protection",
               "branch_protection",
               {{"ref", {{"type", "string"}}},
                {"required_review", {{"type", "boolean"}}},
                {"required_verification", {{"type", "boolean"}}},
                {"allowed_pushers", {{"type", "array"}}}},
               json::array({"ref"}));
    add_list(registry, "list_branch_protections", "List branch protections", "branch_protection");
}

} // namespace lake::api
