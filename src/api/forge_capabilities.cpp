#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/hmac.h>

#include "forge_records_api.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/forge_records.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"
#include "repository_people.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

template <class T> using ForgeTask = liquid::runtime::task<T>;

// A finished nested task: a runtime failure is a store error.
ApiResult settled_result(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

std::expected<control::ForgeRecord, ApiResult> settled_record(
    std::expected<std::expected<control::ForgeRecord, ApiResult>, liquid::error> completed) {
    if (!completed) {
        return std::unexpected(ApiResult::failure("store_error", completed.error().message));
    }
    return std::move(*completed);
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool uuid_shaped(std::string_view value) {
    return value.size() == 36 && std::ranges::all_of(value, [](unsigned char c) {
               return std::isxdigit(c) != 0 || c == '-';
           });
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

ApiResult failure(const ApiError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

// The forge records of the repository `input` names, in its workspace.
std::expected<RepositoryForge, ApiResult> open_forge(const json& input, ApiContext& context) {
    const auto repository =
        forge_repository(context, input.at("repository_id").get<std::string>());
    if (!repository) {
        return std::unexpected(failure(repository.error()));
    }
    auto forge = RepositoryForge::open(context, *repository);
    if (!forge) {
        return std::unexpected(failure(forge.error()));
    }
    return std::move(*forge);
}

void audit_record(ApiContext& context, const control::ForgeRecord& record,
                  const RepositoryForge& forge) {
    context.audit_customer_id = forge.repository().customer_id;
    context.audit_object = "repository/" + record.repository_id;
    context.audit_details = {{"forge_kind", record.kind},
                             {"record_id", record.id},
                             {"workspace_id", forge.repository().workspace_id}};
}

ForgeTask<void> deliver_event(const std::string& event, const control::ForgeRecord& subject,
                              RepositoryForge& forge, ApiContext& context);

ForgeTask<ApiResult> create_record(RepositoryForge& forge, std::string kind, json value,
                                   ApiContext& context) {
    const auto record = control::settled(co_await forge.create(kind, std::move(value)));
    if (!record) {
        co_return forge_records_failure(record.error());
    }
    audit_record(context, *record, forge);
    const std::string event = kind == "issue" || kind == "issue_comment"     ? "issue"
                              : kind == "review" || kind == "review_comment" ? "review"
                              : kind == "release"                            ? "release"
                                                                             : "";
    if (!event.empty()) {
        co_await deliver_event(event, *record, forge, context);
    }
    co_return ApiResult::success(control::to_json(*record));
}

ForgeTask<ApiResult> list_records(std::string kind, const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    if (!context.authorized) {
        const auto repository =
            context.store->get_repository(input.at("repository_id").get<std::string>());
        if (!repository) {
            co_return from_store_error(repository.error());
        }
        if (repository->visibility != "public") {
            co_return ApiResult::failure("unauthorized", "the repository is not public");
        }
    } else if (context.acting_identity != "admin") {
        const auto object = repository_object(input, context);
        if (!object) {
            co_return ApiResult::failure(
                object.error().code, object.error().message, object.error().path);
        }
        const auto decision = control::authorize(
            *context.store, context.acting_identity, *object, "read", context.authorization_cache);
        if (!decision) {
            co_return from_store_error(decision.error());
        }
        if (!decision->allowed) {
            co_return ApiResult::failure("unauthorized",
                                         "principal is not allowed to read repository");
        }
    }
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto records =
        control::settled(co_await forge->list(kind, input.value("limit", 100)));
    if (!records) {
        co_return forge_records_failure(records.error());
    }
    json items = json::array();
    for (const control::ForgeRecord& record : *records) {
        json output = control::to_json(record);
        if (record.kind == "webhook") {
            output["value"].erase("secret");
        }
        items.push_back(std::move(output));
    }
    co_return ApiResult::success({{"items", std::move(items)}});
}

// This repository's record `id` of `kind`; `not_found` at `/<field>` when there is none.
ForgeTask<std::expected<control::ForgeRecord, ApiResult>>
get_kind(RepositoryForge& forge, std::string id, std::string kind, std::string field) {
    using Result = std::expected<control::ForgeRecord, ApiResult>;
    const auto record = control::settled(co_await forge.get(std::move(id)));
    if (!record) {
        co_return Result(std::unexpected(forge_records_failure(record.error())));
    }
    if (!*record || (*record)->kind != kind) {
        co_return Result(std::unexpected(ApiResult::failure(
            "not_found", "forge record does not match repository", "/" + field)));
    }
    co_return Result(**record);
}

// Merges `changes` into the record's value, conditionally on the revision read.
ForgeTask<ApiResult> update_record(RepositoryForge& forge, const control::ForgeRecord& record,
                                   json changes, ApiContext& context) {
    const auto updated = control::settled(co_await forge.update(
        record.id, record.kind, [changes = std::move(changes)](json value) {
            for (const auto& [key, item] : changes.items()) {
                value[key] = item;
            }
            return value;
        }));
    if (!updated) {
        co_return forge_records_failure(updated.error());
    }
    if (!*updated) {
        co_return ApiResult::failure("not_found", "forge record does not match repository");
    }
    audit_record(context, **updated, forge);
    co_return ApiResult::success(control::to_json(**updated));
}

// Who wrote an issue or an issue comment (lake#598), or opened a review (LSR-077): the caller's
// TAAP identity, set by lake and never taken from the input (the schemas refuse those members). A
// GitHub import keeps the GitHub login it read instead. A review records its opener as `opened_by`,
// not `author`: liquid's forge client reads a review comment's `author` as an actor object.
void stamp_author(const std::string& kind, json& value, const ApiContext& context) {
    if (context.acting_identity.empty()) {
        return;
    }
    if (kind == "issue" || kind == "issue_comment") {
        value["author"] = context.acting_identity;
    } else if (kind == "review") {
        value["opened_by"] = context.acting_identity;
    }
}

ForgeTask<ApiResult> create_kind(std::string kind, const json& input, ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    json value = input;
    value.erase("repository_id");
    co_return settled_result(
        co_await create_record(*forge, std::move(kind), std::move(value), context));
}

ForgeTask<ApiResult> create_opened(std::string kind, const json& input, ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    json value = input;
    value.erase("repository_id");
    value["state"] = "open";
    stamp_author(kind, value, context);
    co_return settled_result(
        co_await create_record(*forge, std::move(kind), std::move(value), context));
}

ForgeTask<ApiResult> update_kind(std::string kind, std::string id_name, const json& input,
                                 ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto record = settled_record(co_await get_kind(*forge, input.at(id_name), kind, id_name));
    if (!record) {
        co_return record.error();
    }
    json changes = input;
    changes.erase("repository_id");
    changes.erase(id_name);
    co_return settled_result(
        co_await update_record(*forge, *record, std::move(changes), context));
}

// A string member of a record's value, or empty when it is missing or not a string.
std::string string_member(const json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// update_review (LSR-077). An approval is lake's record of who gave it and for which change,
// `approval: {by, change_id}`, never the caller's input. Approving needs `review` on the
// repository and is refused to the person who opened the review; any other state withdraws it.
// Branch protection counts only such an approval, checked again when the push is
// (evaluate_branch_protection).
ForgeTask<ApiResult> update_review(const json& input, ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto record =
        settled_record(co_await get_kind(*forge, input.at("review_id"), "review", "review_id"));
    if (!record) {
        co_return record.error();
    }
    const std::string state = input.at("state").get<std::string>();
    json changes = {{"state", state}, {"approval", nullptr}};
    if (state == "approved") {
        const std::string& approver = context.acting_identity;
        if (approver.empty()) {
            co_return ApiResult::failure("unauthorized", "approving a review needs a principal");
        }
        if (approver != "admin") {
            const auto object = repository_object(input, context);
            if (!object) {
                co_return failure(object.error());
            }
            const auto decision = control::authorize(
                *context.store, approver, *object, "review", context.authorization_cache);
            if (!decision) {
                co_return from_store_error(decision.error());
            }
            if (!decision->allowed) {
                co_return ApiResult::failure("unauthorized",
                                             "approving a review needs review on the repository");
            }
        }
        if (string_member(record->value, "opened_by") == approver) {
            co_return ApiResult::failure(
                "forbidden", "a review cannot be approved by the person who opened it", "/state");
        }
        changes["approval"] = {{"by", approver},
                               {"change_id", string_member(record->value, "change_id")}};
    }
    co_return settled_result(co_await update_record(*forge, *record, std::move(changes), context));
}

// A comment on this repository's issue or review: the parent must exist here.
ForgeTask<ApiResult> create_comment(std::string kind, std::string parent_kind,
                                    std::string parent_id, const json& input,
                                    ApiContext& context) {
    if (kind == "review_comment") {
        const json& anchor = input.at("anchor");
        if (!anchor.contains("change") || !anchor.contains("path") || !anchor.contains("line") ||
            !anchor.contains("column") || !anchor.at("change").is_string() ||
            !anchor.at("path").is_string() || !anchor.at("line").is_number_integer() ||
            !anchor.at("column").is_number_integer() ||
            anchor.at("line").get<std::int64_t>() < 0 ||
            anchor.at("column").get<std::int64_t>() < 0) {
            co_return ApiResult::failure(
                "invalid_input", "anchor requires change, path, line, and column", "/anchor");
        }
    }
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto parent = settled_record(
        co_await get_kind(*forge, input.at(parent_id), parent_kind, parent_id));
    if (!parent) {
        co_return parent.error();
    }
    json value = input;
    value.erase("repository_id");
    stamp_author(kind, value, context);
    co_return settled_result(
        co_await create_record(*forge, std::move(kind), std::move(value), context));
}

ForgeTask<ApiResult> create_release(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    if (!context.sync_store) {
        co_return ApiResult::failure("store_unavailable", "no repository store is configured");
    }
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const control::Repository& repository = forge->repository();
    json artifacts = json::array();
    for (const json& artifact_id : input.at("artifact_ids")) {
        const auto artifact = co_await blocking_call(
            [&] {
                return context.sync_store->repository_artifact(
                    repository.customer_id, repository.id, artifact_id.get<std::string>());
            },
            [](std::string message) -> SyncStoreResult<RepositoryArtifact> {
                return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
            });
        if (!artifact) {
            co_return ApiResult::failure(artifact.error().code, artifact.error().message);
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
            co_return ApiResult::failure(
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
    if (context.search) {
        const auto published = co_await blocking_call(
            [&] {
                return context.search->publish(
                    search::Publish{repository.customer_id, repository.id,
                                    input.at("tag").get<std::string>(),
                                    input.at("change_id").get<std::string>(),
                                    search::PublicationKind::TagSnapshot});
            },
            [](std::string message) -> search::Result<void> {
                return std::unexpected(search::Error{"store_unavailable", std::move(message)});
            });
        if (!published) {
            co_return ApiResult::failure(published.error().code, published.error().message);
        }
    }
    auto created = co_await create_record(*forge, "release", std::move(value), context);
    // lake#381: published again once the release record exists. A workspace index being built
    // meanwhile records the repository as pending and reads this record when it applies it, so
    // the snapshot is not lost between the build's reading of the releases and its end. The same
    // tag and change is a no-op otherwise.
    if (context.search && created && created->ok) {
        const auto republished = co_await blocking_call(
            [&] {
                return context.search->publish(
                    search::Publish{repository.customer_id, repository.id,
                                    input.at("tag").get<std::string>(),
                                    input.at("change_id").get<std::string>(),
                                    search::PublicationKind::TagSnapshot});
            },
            [](std::string message) -> search::Result<void> {
                return std::unexpected(search::Error{"store_unavailable", std::move(message)});
            });
        if (!republished) {
            context.audit_details["search_index"] = republished.error().message;
        }
    }
    co_return settled_result(std::move(created));
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

bool webhook_accepts_event(const control::ForgeRecord& webhook, const std::string& event) {
    if (!webhook.value.contains("events") || !webhook.value["events"].is_array()) {
        return false;
    }
    return std::ranges::any_of(webhook.value["events"], [&event](const json& configured) {
        return configured.is_string() &&
               (configured.get<std::string>() == event || configured.get<std::string>() == "*");
    });
}

// Sends one signed event to `webhook` and records the attempt in the repository's workspace.
ForgeTask<std::expected<control::ForgeRecord, ApiResult>>
record_webhook_attempt(RepositoryForge& forge, const control::ForgeRecord& webhook,
                       std::string event, json payload, int attempt, ApiContext& context) {
    using Result = std::expected<control::ForgeRecord, ApiResult>;
    const std::string encoded_payload = payload.dump();
    const std::string signature =
        webhook_signature(webhook.value.value("secret", ""), encoded_payload);
    WebhookDeliveryResult delivery{.error = "webhook sender is not configured"};
    if (context.webhook_sender) {
        // The endpoint is the customer's and may be slow: the request waits on the blocking pool,
        // never on the thread that serves every other request (lake#772).
        auto sent = co_await run_blocking(
            [sender = context.webhook_sender,
             url = webhook.value.value("url", ""),
             headers = json{{"content-type", "application/json"},
                            {"x-lake-event", event},
                            {"x-lake-signature-256", signature}},
             encoded_payload] { return sender(url, headers, encoded_payload); });
        delivery = sent ? std::move(*sent) : WebhookDeliveryResult{.error = sent.error().message};
    }
    const bool delivered =
        delivery.error.empty() && delivery.status >= 200 && delivery.status < 300;
    const auto record = control::settled(co_await forge.create("webhook_delivery",
                                                               {{"webhook_id", webhook.id},
                                                                {"event", event},
                                                                {"payload", payload},
                                                                {"signature", signature},
                                                                {"attempt", attempt},
                                                                {"status", delivery.status},
                                                                {"error", delivery.error},
                                                                {"delivered", delivered}}));
    if (!record) {
        co_return Result(std::unexpected(forge_records_failure(record.error())));
    }
    co_return Result(*record);
}

ForgeTask<void> deliver_event(const std::string& event, const control::ForgeRecord& subject,
                              RepositoryForge& forge, ApiContext& context) {
    const auto webhooks = control::settled(co_await forge.list("webhook", 1000));
    if (!webhooks) {
        co_return {};
    }
    const json payload = {{"event", event},
                          {"repository_id", subject.repository_id},
                          {"record", control::to_json(subject)}};
    for (const control::ForgeRecord& webhook : *webhooks) {
        if (!webhook_accepts_event(webhook, event)) {
            continue;
        }
        static_cast<void>(
            co_await record_webhook_attempt(forge, webhook, event, payload, 1, context));
    }
    co_return {};
}

ForgeTask<ApiResult> deliver_webhook(const json& input, ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto webhook = settled_record(
        co_await get_kind(*forge, input.at("webhook_id"), "webhook", "webhook_id"));
    if (!webhook) {
        co_return webhook.error();
    }
    if (!webhook_accepts_event(*webhook, input.at("event").get<std::string>())) {
        co_return ApiResult::failure(
            "conflict", "webhook does not subscribe to this event", "/event");
    }
    const auto delivery = settled_record(co_await record_webhook_attempt(
        *forge, *webhook, input.at("event").get<std::string>(), input.at("payload"), 1, context));
    if (!delivery) {
        co_return delivery.error();
    }
    audit_record(context, *delivery, *forge);
    co_return ApiResult::success(control::to_json(*delivery));
}

ForgeTask<ApiResult> retry_webhook_delivery(const json& input, ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto delivery = settled_record(
        co_await get_kind(*forge, input.at("delivery_id"), "webhook_delivery", "delivery_id"));
    if (!delivery) {
        co_return delivery.error();
    }
    if (delivery->value.value("delivered", false)) {
        co_return ApiResult::failure("conflict", "delivered webhook events are not retried");
    }
    const auto webhook = settled_record(co_await get_kind(
        *forge, delivery->value.value("webhook_id", ""), "webhook", "delivery_id"));
    if (!webhook) {
        co_return ApiResult::failure("not_found", "delivery webhook does not match repository");
    }
    const auto retried = settled_record(
        co_await record_webhook_attempt(*forge, *webhook, delivery->value.value("event", ""),
                                        delivery->value.value("payload", json::object()),
                                        delivery->value.value("attempt", 0) + 1, context));
    if (!retried) {
        co_return retried.error();
    }
    audit_record(context, *retried, *forge);
    co_return ApiResult::success(control::to_json(*retried));
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

ForgeTask<ApiResult> map_review_anchors(const json& input, ApiContext& context) {
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto records = control::settled(co_await forge->list("review_comment", 1000));
    if (!records) {
        co_return forge_records_failure(records.error());
    }
    const std::string source_change = input.at("source_change");
    const std::string path = input.at("path");
    const std::string target_change = input.at("target_change");
    json items = json::array();
    for (const control::ForgeRecord& record : *records) {
        if (!record.value.contains("anchor") || !record.value["anchor"].is_object() ||
            record.value["anchor"].value("change", "") != source_change ||
            record.value["anchor"].value("path", "") != path) {
            continue;
        }
        const auto updated = control::settled(co_await forge->update(
            record.id, record.kind,
            [&input, &target_change, &source_change, &path](json value) {
                // Reread under the revision: map only an anchor still at the source location.
                if (value.contains("anchor") && value["anchor"].is_object() &&
                    value["anchor"].value("change", "") == source_change &&
                    value["anchor"].value("path", "") == path) {
                    value["anchor"] = map_anchor(value["anchor"], input.at("hunks"),
                                                 target_change);
                }
                return value;
            }));
        if (!updated) {
            co_return forge_records_failure(updated.error());
        }
        if (*updated) {
            items.push_back(control::to_json(**updated));
        }
    }
    context.audit_customer_id = forge->repository().customer_id;
    context.audit_object = "repository/" + forge->repository().id;
    context.audit_details = {{"forge_kind", "review_comment"},
                             {"mapped", items.size()},
                             {"workspace_id", forge->repository().workspace_id}};
    co_return ApiResult::success({{"items", std::move(items)}});
}

// What each forge list and create capability does for the person calling it (lake#450).
std::string forge_description(const std::string& name) {
    static const std::map<std::string, std::string> descriptions = {
        {"create_webhook",
         "Register a webhook so a repository's events are POSTed to your URL, signed with your "
         "secret, and every delivery is recorded."},
        {"list_issues", "List a repository's issues, newest first, to see what is open or done."},
        {"list_issue_comments", "List the discussion on a repository's issues, newest first."},
        {"list_reviews", "List a repository's change reviews and their state, newest first."},
        {"list_review_comments",
         "List the comments left on a repository's change reviews, newest first."},
        {"list_releases",
         "List a repository's tagged releases with their verified build artifacts."},
        {"list_webhooks", "List the webhooks registered on a repository, without their secrets."},
        {"list_webhook_deliveries",
         "List a repository's webhook delivery attempts with their status, to find failed ones "
         "to retry."},
        {"list_exports", "List a repository's signed portable exports, to download or check one."},
        {"list_branch_protections",
         "List the branch protection rules on a repository: who may write to which "
         "references and what must pass first."},
    };
    const auto found = descriptions.find(name);
    return found == descriptions.end() ? std::string() : found->second;
}

void add_create(Registry& registry,
                std::string name,
                std::string title,
                std::string kind,
                json properties,
                json required) {
    std::string description = forge_description(name);
    registry.add(
        Capability{.name = std::move(name),
                   .title = std::move(title),
                   .description = std::move(description),
                   .input_schema = repository_input(std::move(properties), std::move(required)),
                   .output_schema = record_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "repository",
                   .action = "write",
                   .resolve_object = repository_object,
                   .async_handler = [kind = std::move(kind)](const json& input,
                                                             ApiContext& context) {
                       return create_kind(kind, input, context);
                   }});
}

void add_list(Registry& registry,
              std::string name,
              std::string title,
              std::string kind,
              bool allows_anonymous_public_read = false) {
    std::string description = forge_description(name);
    registry.add(Capability{
        .name = std::move(name),
        .title = std::move(title),
        .description = std::move(description),
        .input_schema = repository_input({{"limit", {{"type", "integer"}}}}, json::array()),
        .output_schema = records_schema(),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = repository_object,
        .authorizes_candidates = true,
        .allows_anonymous_public_read = allows_anonymous_public_read,
        .async_handler = [kind = std::move(kind)](const json& input, ApiContext& context) {
            return list_records(kind, input, context);
        }});
}

} // namespace

namespace {

std::string branch_policy_key(const std::string& repository_id, const std::string& ref) {
    return repository_id + ":" + ref;
}

bool branch_matches(std::string_view pattern, std::string_view ref) {
    if (pattern == ref) {
        return true;
    }
    if (!pattern.ends_with('*')) {
        return false;
    }
    return ref.starts_with(pattern.substr(0, pattern.size() - 1));
}

bool allowed_pusher(const json& rule, const std::string& identity) {
    if (!rule.contains("allowed_pushers") || !rule["allowed_pushers"].is_array() ||
        rule["allowed_pushers"].empty()) {
        return true;
    }
    return std::ranges::any_of(rule["allowed_pushers"], [&identity](const json& pusher) {
        return pusher.is_string() && pusher.get<std::string>() == identity;
    });
}

// Whether an approved review of exactly `change_id` counts for this push (LSR-077): its approval
// is lake's record for that change, given by someone other than the pusher and the review's
// opener, who still holds `review` on the rule's object now. An approval lake did not record
// (one written before LSR-077, or carried in by an import) never counts.
control::Result<bool> approved_review(control::Store& store,
                                      const std::string& pusher,
                                      control::AuthorizationCache& authorization_cache,
                                      const std::string& object_id,
                                      const std::vector<control::ForgeRecord>& records,
                                      const std::string& change_id) {
    for (const control::ForgeRecord& review : records) {
        const json& value = review.value;
        if (!value.is_object() || string_member(value, "change_id") != change_id ||
            string_member(value, "state") != "approved" || !value.contains("approval") ||
            !value["approval"].is_object()) {
            continue;
        }
        const json& approval = value["approval"];
        const std::string approver = string_member(approval, "by");
        if (approver.empty() || string_member(approval, "change_id") != change_id ||
            approver == pusher || approver == string_member(value, "opened_by")) {
            continue;
        }
        if (approver == "admin") {
            return true;
        }
        const auto decision =
            control::authorize(store, approver, object_id, "review", authorization_cache);
        if (!decision) {
            return std::unexpected(decision.error());
        }
        if (decision->allowed) {
            return true;
        }
    }
    return false;
}

bool verified_release(const std::vector<control::ForgeRecord>& records,
                      const std::string& change_id) {
    return std::ranges::any_of(records, [&change_id](const control::ForgeRecord& release) {
        return release.value.value("change_id", "") == change_id &&
               release.value.contains("artifacts") && release.value["artifacts"].is_array() &&
               !release.value["artifacts"].empty();
    });
}

ForgeTask<ApiResult> set_branch_protection(const json& input, ApiContext& context) {
    const std::string repository_id = input.at("repository_id").get<std::string>();
    const std::string ref = input.at("ref").get<std::string>();
    auto forge = open_forge(input, context);
    if (!forge) {
        co_return forge.error();
    }
    const auto repository = context.store->find_policy_object("repository", repository_id);
    if (!repository) {
        co_return from_store_error(repository.error());
    }
    const std::string key = branch_policy_key(repository_id, ref);
    auto policy = context.store->find_policy_object("path", key);
    if (!policy && policy.error().code == "not_found") {
        policy = context.store->create_policy_object(control::NewPolicyObject{
            "path", key, repository->id, context.acting_identity, context.request_id, true});
    }
    if (!policy) {
        co_return from_store_error(policy.error());
    }
    // The rule's object sits directly under the repository's own. One holding the same id
    // anywhere else was made to claim this branch (lake#913) and is never adopted.
    if (policy->parent_id != repository->id) {
        co_return ApiResult::failure("conflict",
                                     "the policy object for " + ref +
                                         " belongs elsewhere; ask the operator to remove it",
                                     "/ref");
    }
    json value = input;
    value.erase("repository_id");
    value["policy_object_id"] = policy->id;
    const auto rules = control::settled(co_await forge->list("branch_protection", 1000));
    if (!rules) {
        co_return forge_records_failure(rules.error());
    }
    for (const control::ForgeRecord& rule : *rules) {
        if (rule.value.value("ref", "") != ref) {
            continue;
        }
        co_return settled_result(
            co_await update_record(*forge, rule, std::move(value), context));
    }
    co_return settled_result(
        co_await create_record(*forge, "branch_protection", std::move(value), context));
}

} // namespace

liquid::runtime::task<std::expected<BranchProtectionRecords, SyncStoreError>>
load_branch_protection(ApiContext& context, control::Repository repository) {
    using Result = std::expected<BranchProtectionRecords, SyncStoreError>;
    const auto refused = [](std::string code, std::string message) {
        return Result(std::unexpected(
            SyncStoreError{std::move(code), "branch protection is unavailable: " +
                                                std::move(message), "branch protection"}));
    };
    // A lake that serves no workspace Zettas (no workspace service configured) keeps no forge
    // records anywhere, so there are no rules to apply. Every other case reads the repository's
    // workspace, and an unreadable workspace refuses the push's reference updates.
    if (!context.workspaces) {
        co_return Result(BranchProtectionRecords{});
    }
    auto forge = RepositoryForge::open(context, std::move(repository));
    if (!forge) {
        co_return refused(forge.error().code, forge.error().message);
    }
    BranchProtectionRecords records;
    auto rules = control::settled(co_await forge->list("branch_protection", 1000));
    if (!rules) {
        co_return refused(rules.error().code, rules.error().message);
    }
    records.rules = std::move(*rules);
    const auto any_rule = [&records](std::string_view field) {
        return std::ranges::any_of(records.rules, [field](const control::ForgeRecord& rule) {
            return rule.value.value(std::string(field), false);
        });
    };
    // Reviews and releases are read only when a rule asks for them.
    if (any_rule("required_review")) {
        auto reviews = control::settled(co_await forge->list("review", 1000));
        if (!reviews) {
            co_return refused(reviews.error().code, reviews.error().message);
        }
        records.reviews = std::move(*reviews);
    }
    if (any_rule("required_verification")) {
        auto releases = control::settled(co_await forge->list("release", 1000));
        if (!releases) {
            co_return refused(releases.error().code, releases.error().message);
        }
        records.releases = std::move(*releases);
    }
    co_return Result(std::move(records));
}

SyncStoreResult<BranchProtectionEvaluation>
evaluate_branch_protection(control::Store& store,
                           const std::string& acting_identity,
                           control::AuthorizationCache& authorization_cache,
                           const BranchProtectionRecords& records,
                           const BranchRefUpdate& update) {
    BranchProtectionEvaluation evaluation;
    for (const control::ForgeRecord& rule : records.rules) {
        const std::string rule_ref = rule.value.value("ref", "");
        if (!branch_matches(rule_ref, update.name)) {
            continue;
        }
        evaluation.matched_rules.push_back(rule_ref);
        if (rule.value.value("fast_forward_only", false) && !update.fast_forward) {
            return std::unexpected(SyncStoreError{
                "conflict",
                "branch protection rejects a non-fast-forward update to " + update.name,
                update.name,
                "fast_forward_only"});
        }
        if (!allowed_pusher(rule.value, acting_identity)) {
            return std::unexpected(
                SyncStoreError{"unauthorized",
                               "branch protection rejects this pusher for " + update.name,
                               update.name,
                               "allowed_pushers"});
        }
        const std::string policy_object_id = rule.value.value("policy_object_id", "");
        if (acting_identity != "admin" && !policy_object_id.empty()) {
            const auto decision = control::authorize(
                store, acting_identity, policy_object_id, "write", authorization_cache);
            if (!decision) {
                return std::unexpected(SyncStoreError{
                    decision.error().code, decision.error().message, update.name, "write_policy"});
            }
            if (!decision->allowed) {
                return std::unexpected(SyncStoreError{"unauthorized",
                                                      "branch protection rejects a push to " +
                                                          update.name + " by write policy",
                                                      update.name,
                                                      "write_policy"});
            }
        }
        if (rule.value.value("required_review", false)) {
            // Reviewers are checked on the rule's own object, which inherits the repository's
            // grants; a rule without one (written outside set_branch_protection) uses the
            // repository's object.
            std::string review_object = policy_object_id;
            if (review_object.empty()) {
                const auto repository = store.find_policy_object("repository", rule.repository_id);
                if (!repository) {
                    return std::unexpected(SyncStoreError{repository.error().code,
                                                          repository.error().message,
                                                          update.name,
                                                          "required_review"});
                }
                review_object = repository->id;
            }
            const auto approved = approved_review(store,
                                                  acting_identity,
                                                  authorization_cache,
                                                  review_object,
                                                  records.reviews,
                                                  update.new_head);
            if (!approved) {
                return std::unexpected(SyncStoreError{approved.error().code,
                                                      approved.error().message,
                                                      update.name,
                                                      "required_review"});
            }
            if (!*approved) {
                return std::unexpected(SyncStoreError{"conflict",
                                                      "branch protection rejects " + update.name +
                                                          ": an approved review is required",
                                                      update.name,
                                                      "required_review"});
            }
        }
        if (rule.value.value("required_verification", false) &&
            !verified_release(records.releases, update.new_head)) {
            return std::unexpected(SyncStoreError{"conflict",
                                                  "branch protection rejects " + update.name +
                                                      ": verified build evidence is required",
                                                  update.name,
                                                  "required_verification"});
        }
    }
    return evaluation;
}

void register_forge_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "create_issue",
        .title = "Create issue",
        .description =
            "Open an issue on a repository to track a bug or a task, written in Markdown.",
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
        .async_handler = [](const json& input, ApiContext& context) {
            return create_opened("issue", input, context);
        }});
    add_list(registry, "list_issues", "List issues", "issue", true);
    registry.add(
        Capability{.name = "update_issue",
                   .title = "Update issue",
                   .description = "Edit an issue's title, body or labels, or close and reopen it.",
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
                   .async_handler = [](const json& input, ApiContext& context) {
                       return update_kind("issue", "issue_id", input, context);
                   }});
    registry.add(Capability{
        .name = "create_issue_comment",
        .title = "Comment on issue",
        .description =
            "Comment on a repository issue, in Markdown, so its discussion stays with it.",
        .input_schema =
            repository_input({{"issue_id", {{"type", "string"}}}, {"body", {{"type", "string"}}}},
                             json::array({"issue_id", "body"})),
        .output_schema = record_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = repository_object,
        .async_handler = [](const json& input, ApiContext& context) {
            return create_comment("issue_comment", "issue", "issue_id", input, context);
        }});
    add_list(registry, "list_issue_comments", "List issue comments", "issue_comment", true);
    registry.add(Capability{
        .name = "issue_assignees",
        .title = "People to Assign",
        .description =
            "The people an issue on a repository can be assigned to: the person who owns the "
            "repository's workspace, or the organization and its members, each with their TAAP "
            "identity (what `assignees` holds), display name, email and handle. Needs `write` on "
            "the repository, as assigning does.",
        .input_schema = repository_input(json::object(), json::array()),
        .output_schema =
            {{"type", "object"},
             {"properties",
              {{"people",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"taap_id", {{"type", "string"}}},
                     {"kind",
                      {{"type", "string"}, {"enum", json::array({"user", "organization"})}}},
                     {"display_name", {{"type", "string"}}},
                     {"email", {{"type", "string"}}},
                     {"handle", {{"type", "string"}}}}},
                   {"required",
                    json::array({"taap_id", "kind", "display_name", "email", "handle"})},
                   {"additionalProperties", false}}}}}}},
             {"required", json::array({"people"})},
             {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "write",
        .resolve_object = repository_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto repository = context.store->get_repository(input.at("repository_id"));
            if (!repository) {
                return from_store_error(repository.error());
            }
            const auto people = repository_people(context, *repository);
            if (!people) {
                return failure(people.error());
            }
            json out = json::array();
            for (const RepositoryPerson& person : *people) {
                out.push_back(repository_person_json(person));
            }
            context.audit_customer_id = repository->customer_id;
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"people", out.size()}};
            return ApiResult::success({{"people", std::move(out)}});
        }});

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
                            .async_handler = [](const json& input, ApiContext& context) {
                                return create_opened("review", input, context);
                            }});
    add_list(registry, "list_reviews", "List change reviews", "review", true);
    registry.add(Capability{
        .name = "update_review",
        .title = "Update change review",
        .description = "Approve a change review, ask for changes, or close it, which decides "
                       "whether the change may land. Approving needs review on the repository "
                       "and is refused to the person who opened the review; Lake records the "
                       "approver, and a protected branch never counts the pusher's own approval.",
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
        .async_handler = [](const json& input, ApiContext& context) {
            return update_review(input, context);
        }});
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
                   .async_handler = [](const json& input, ApiContext& context) {
                       return create_comment("review_comment", "review", "review_id", input,
                                             context);
                   }});
    add_list(registry, "list_review_comments", "List review comments", "review_comment", true);

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
        .async_handler = map_review_anchors});

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
                            .async_handler = create_release});
    add_list(registry, "list_releases", "List releases", "release", true);
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
        .name = "deliver_webhook",
        .title = "Deliver signed webhook",
        .description = "Send one signed webhook event and record its delivery attempt.",
        .input_schema = repository_input({{"webhook_id", {{"type", "string"}}},
                                          {"event", {{"type", "string"}}},
                                          {"payload", {{"type", "object"}}}},
                                         json::array({"webhook_id", "event", "payload"})),
        .output_schema = record_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = repository_object,
        .async_handler = deliver_webhook});
    registry.add(
        Capability{.name = "retry_webhook_delivery",
                   .title = "Retry webhook delivery",
                   .description = "Retry a failed signed webhook delivery and retain each attempt.",
                   .input_schema = repository_input({{"delivery_id", {{"type", "string"}}}},
                                                    json::array({"delivery_id"})),
                   .output_schema = record_schema(),
                   .kind = CapabilityKind::Action,
                   .object = "repository",
                   .action = "write",
                   .resolve_object = repository_object,
                   .async_handler = retry_webhook_delivery});
    add_list(registry, "list_webhook_deliveries", "List webhook deliveries", "webhook_delivery");
    add_list(registry, "list_exports", "List exports", "export");

    registry.add(Capability{
        .name = "set_branch_protection",
        .title = "Set branch protection",
        .description = "Set the review, verification, fast-forward, and pusher policy for a ref.",
        .input_schema = repository_input({{"ref", {{"type", "string"}}},
                                          {"required_review", {{"type", "boolean"}}},
                                          {"required_verification", {{"type", "boolean"}}},
                                          {"fast_forward_only", {{"type", "boolean"}}},
                                          {"allowed_pushers", {{"type", "array"}}}},
                                         json::array({"ref"})),
        .output_schema = record_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "admin",
        .resolve_object = repository_object,
        .async_handler = set_branch_protection,
    });
    add_list(registry, "list_branch_protections", "List branch protections", "branch_protection");
}

std::expected<control::Repository, ApiError> forge_repository(const ApiContext& context,
                                                              const std::string& repository_id) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    auto repository = context.store->get_repository(repository_id);
    if (!repository) {
        return std::unexpected(
            ApiError{repository.error().code, repository.error().message, "/repository_id"});
    }
    return std::move(*repository);
}

ApiResult forge_records_failure(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::expected<RepositoryForge, ApiError> RepositoryForge::open(const ApiContext& context,
                                                               control::Repository repository) {
    if (repository.workspace_id.empty()) {
        return std::unexpected(ApiError{"conflict",
                                        "the repository has no workspace yet, so it has nowhere "
                                        "to keep forge records; an operator must assign one",
                                        "/repository_id"});
    }
    auto records = workspace_records(context, repository.customer_id, repository.workspace_id);
    if (!records) {
        return std::unexpected(ApiError{records.error().code, records.error().message, {}});
    }
    return RepositoryForge(std::move(*records), std::move(repository));
}

RepositoryForge::Task<control::ForgeRecord>
RepositoryForge::create(std::string kind, nlohmann::json value, control::RecordWrite write) {
    using Result = control::ZettaControlResult<control::ForgeRecord>;
    if (!control::is_forge_record_kind(kind) || !value.is_object()) {
        co_return Result(std::unexpected(control::ZettaControlError{
            "invalid_input", "forge record kind and object value are required"}));
    }
    control::ForgeRecord record;
    record.repository_id = repository_.id;
    record.kind = std::move(kind);
    record.value = std::move(value);
    control::ForgeRecords set(records_);
    co_return control::settled(co_await set.append(std::move(record), std::move(write)));
}

RepositoryForge::Task<control::ForgeRecord> RepositoryForge::copy(control::ForgeRecord record) {
    record.repository_id = repository_.id;
    control::ForgeRecords set(records_);
    const control::RecordWrite write{record.id, record.at_us};
    co_return control::settled(co_await set.append(std::move(record), write));
}

RepositoryForge::Task<std::vector<control::ForgeRecord>> RepositoryForge::list(std::string kind,
                                                                               int limit) {
    using Result = control::ZettaControlResult<std::vector<control::ForgeRecord>>;
    std::vector<std::string_view> kinds;
    if (kind.empty()) {
        kinds.assign(control::forge_record_kinds.begin(), control::forge_record_kinds.end());
    } else if (control::is_forge_record_kind(kind)) {
        kinds.push_back(kind);
    }
    const std::size_t newest = limit > 0 ? static_cast<std::size_t>(limit) : 0;
    control::ForgeRecords set(records_);
    std::vector<control::ForgeRecord> output;
    for (const std::string_view each : kinds) {
        auto listed = control::settled(
            co_await set.list(control::forge_record_lookup(repository_.id, each), newest));
        if (!listed) {
            co_return Result(std::unexpected(listed.error()));
        }
        for (control::ForgeRecord& record : *listed) {
            if (record.repository_id == repository_.id && record.kind == each) {
                output.push_back(std::move(record));
            }
        }
    }
    std::ranges::sort(output, [](const control::ForgeRecord& left,
                                 const control::ForgeRecord& right) {
        return left.at_us != right.at_us ? left.at_us > right.at_us : left.id > right.id;
    });
    if (newest != 0 && output.size() > newest) {
        output.resize(newest);
    }
    co_return Result(std::move(output));
}

RepositoryForge::Task<std::optional<control::ForgeRecord>> RepositoryForge::get(std::string id) {
    using Found = std::optional<control::ForgeRecord>;
    using Result = control::ZettaControlResult<Found>;
    if (!uuid_shaped(id)) {
        co_return Result(Found{});
    }
    auto stored =
        control::settled(co_await records_.get(control::ForgeRecordCodec::collection, id));
    if (!stored) {
        co_return Result(std::unexpected(stored.error()));
    }
    if (!*stored) {
        co_return Result(Found{});
    }
    auto decoded = control::ForgeRecordCodec::decode(**stored);
    if (!decoded || decoded->repository_id != repository_.id) {
        co_return Result(Found{});
    }
    co_return Result(std::move(decoded));
}

RepositoryForge::Task<std::optional<control::ForgeRecord>>
RepositoryForge::update(std::string id, std::string kind,
                        std::function<nlohmann::json(nlohmann::json)> change, int attempts) {
    using Found = std::optional<control::ForgeRecord>;
    using Result = control::ZettaControlResult<Found>;
    control::ZettaControlError last{"conflict", "the forge record kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto current = control::settled(co_await get(id));
        if (!current) {
            co_return Result(std::unexpected(current.error()));
        }
        if (!*current || (*current)->kind != kind) {
            co_return Result(Found{});
        }
        control::ForgeRecord next = **current;
        next.value = change(next.value);
        if (!next.value.is_object()) {
            co_return Result(std::unexpected(control::ZettaControlError{
                "invalid_input", "forge record value must be an object"}));
        }
        next.updated_at = control::record_instant(now_us());
        auto written = control::settled(co_await records_.put(
            control::ForgeRecordCodec::collection, id, control::ForgeRecordCodec::lookup(next),
            control::ForgeRecordCodec::encode(next),
            control::RecordCondition{.if_revision = (*current)->revision},
            (*current)->lake_revision + 1, (*current)->at_us));
        if (written) {
            auto decoded = control::ForgeRecordCodec::decode(*written);
            if (!decoded) {
                co_return Result(std::unexpected(control::ZettaControlError{
                    "store_error", "the stored forge record did not decode"}));
            }
            co_return Result(std::move(decoded));
        }
        if (written.error().code != "conflict") {
            co_return Result(std::unexpected(written.error()));
        }
        last = written.error(); // someone else wrote first: reread and apply the change again
    }
    co_return Result(std::unexpected(last));
}

} // namespace lake::api
