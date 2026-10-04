#include "lake/api/capabilities.hpp"
#include "lake/api/images.hpp"
#include "audit_logs.hpp"
#include "repository_records_api.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/metering_recorder.hpp"
#include "lake/version.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <expected>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <string>
#include <string_view>

#include <openssl/evp.h>

#include "lake/audit/audit.hpp"

#include "lake/api/apps.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/oauth.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/control/agent_library.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/saml.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult from_store_error(const control::StoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

ApiResult no_store() {
    return ApiResult::failure("store_unavailable",
                              "no control-plane store is configured (LAKE_CONTROL_DB_URI)");
}

ApiResult no_search() {
    return ApiResult::failure("store_unavailable",
                              "no search store is configured (LAKE_STORE_ROOT)");
}

ApiResult no_repository_store() {
    return ApiResult::failure("store_unavailable",
                              "no repository store is configured (LAKE_STORE_ROOT)");
}

// The relying party of a passkey ceremony in `pool_id`, and the origin it answers. An application's
// pool accepts lake's own host, the application's custom sign-in host (both routing), and the
// application's allowed origins, which are its settings in its workspace (lake#351): those are
// read only when the origin is neither host.
liquid::runtime::task<std::expected<std::pair<std::string, std::string>, ApiResult>>
webauthn_relying_party(ApiContext& context, std::string pool_id) {
    using Party = std::expected<std::pair<std::string, std::string>, ApiResult>;
    std::string origin =
        context.request_origin.empty() ? "https://" + context.public_host : context.request_origin;
    std::string host = context.public_host;
    const std::size_t scheme = origin.find("://");
    if (scheme != std::string::npos) {
        host = origin.substr(scheme + 3);
    }
    const std::size_t port = host.find(':');
    if (port != std::string::npos) {
        host.resize(port);
    }
    if (pool_id == "lake") {
        co_return Party(std::pair{host, origin});
    }
    auto application = context.store->get_application_by_pool(pool_id);
    if (!application) {
        co_return Party(std::unexpected(from_store_error(application.error())));
    }
    const std::string custom_origin = application->custom_sign_in_domain.empty()
                                          ? std::string{}
                                          : "https://" + application->custom_sign_in_domain;
    const bool hosted_on_lake = host == context.public_host;
    const bool hosted_on_custom = !custom_origin.empty() && origin == custom_origin;
    if (!hosted_on_lake && !hosted_on_custom) {
        auto configured_one =
            configured(co_await configured_application(std::move(application), context));
        if (!configured_one) {
            co_return Party(std::unexpected(std::move(configured_one.error())));
        }
        if (std::ranges::find(configured_one->allowed_origins, origin) ==
            configured_one->allowed_origins.end()) {
            co_return Party(std::unexpected(
                ApiResult::failure("unauthorized", "origin is not allowed for this application")));
        }
    }
    co_return Party(std::pair{host, origin});
}

// Refuses a cross-origin caller of an application's pool whose origin is not one of the
// application's allowed origins, read from its settings in its workspace (lake#351). A same-origin
// or lake-pool call reads nothing.
liquid::runtime::task<ApiResult> authorize_application_origin(ApiContext& context,
                                                              std::string pool_id) {
    if (pool_id == "lake" || context.caller_origin.empty() ||
        context.caller_origin == context.request_origin) {
        co_return ApiResult::success(nullptr);
    }
    auto application = configured(co_await configured_application(
        context.store->get_application_by_pool(pool_id), context));
    if (!application) {
        co_return std::move(application.error());
    }
    if (std::ranges::find(application->allowed_origins, context.caller_origin) ==
        application->allowed_origins.end()) {
        co_return ApiResult::failure("unauthorized", "origin is not allowed for this application");
    }
    co_return ApiResult::success(nullptr);
}

// The answer of a completed authorize_application_origin.
ApiResult origin_answer(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

ApiResult from_sync_store_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
}

std::string repository_input_value(const json& input,
                                   std::string_view canonical,
                                   std::string_view builder_name) {
    const std::string canonical_value = input.value(std::string(canonical), "");
    const std::string builder_value = input.value(std::string(builder_name), "");
    if (!canonical_value.empty() && !builder_value.empty() && canonical_value != builder_value) {
        return {};
    }
    return canonical_value.empty() ? builder_value : canonical_value;
}

std::expected<std::string, ApiError> repository_content_object(const json& input,
                                                               ApiContext& context) {
    const std::string id = repository_input_value(input, "id", "repository");
    if (id.empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "id or repository is required", "/repository"});
    }
    if (context.store == nullptr) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("repository", id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/repository"});
    }
    context.audit_object = object->id;
    return object->id;
}

std::expected<control::Repository, ApiError>
readable_repository(const json& input, ApiContext& context, bool revision_required = false) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string id = repository_input_value(input, "id", "repository");
    const std::string ref = repository_input_value(input, "ref", "revision");
    if (id.empty() || (revision_required && ref.empty())) {
        return std::unexpected(ApiError{"invalid_input",
                                        revision_required ? "repository and revision are required"
                                                          : "repository is required",
                                        id.empty() ? "/repository" : "/revision"});
    }
    const auto repository = context.store->get_repository(id);
    if (!repository) {
        return std::unexpected(
            ApiError{repository.error().code, repository.error().message, "/id"});
    }
    if (!context.authorized && repository->visibility != "public") {
        return std::unexpected(ApiError{"unauthorized", "the repository is not public", "/id"});
    }
    if (context.authorized &&
        (context.credential_kind == "app_source" || context.credential_kind == "build_source") &&
        repository->id == context.credential_repository_id &&
        repository->customer_id == context.credential_customer_id &&
        ref == context.credential_revision) {
        return *repository;
    }
    if (context.authorized) {
        const auto object = context.store->find_policy_object("repository", repository->id);
        if (!object) {
            return std::unexpected(ApiError{object.error().code, object.error().message, "/id"});
        }
        const auto decisions = control::authorize_batch(*context.store,
                                                        context.acting_identity,
                                                        {object->id},
                                                        "read",
                                                        context.authorization_cache);
        if (!decisions) {
            return std::unexpected(
                ApiError{decisions.error().code, decisions.error().message, "/id"});
        }
        if (!decisions->at(object->id).allowed) {
            return std::unexpected(
                ApiError{"unauthorized", "principal is not allowed to read repository", "/id"});
        }
    }
    return *repository;
}

json repository_ref_schema() {
    return {{"type", "object"},
            {"properties", {{"name", {{"type", "string"}}}, {"change", {{"type", "string"}}}}}};
}

json repository_tree_entry_schema() {
    return {{"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}}},
              {"kind", {{"type", "string"}}},
              {"blob", {{"type", "string"}}},
              {"mode", {{"type", "integer"}}},
              {"node_id", {{"type", "string"}}},
              {"size", {{"type", "integer"}}},
              {"stored_size", {{"type", "integer"}}}}}};
}

json repository_change_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"parents", {{"type", "array"}}},
              {"author", {{"type", "string"}}},
              {"message", {{"type", "string"}}},
              {"occurred_at", {{"type", "string"}}}}}};
}

json repository_conflict_schema() {
    return {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"base", {{"type", "string"}}},
          {"left", {{"type", "string"}}},
          {"right", {{"type", "string"}}},
          {"region",
           {{"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}}},
              {"bytes",
               {{"type", "object"},
                {"properties",
                 {{"offset", {{"type", "integer"}, {"minimum", 0}}},
                  {"length", {{"type", "integer"}, {"minimum", 0}}}}},
                {"required", json::array({"offset", "length"})},
                {"additionalProperties", false}}},
              {"nodes",
               {{"type", "object"},
                {"properties", {{"first", {{"type", "string"}}}, {"last", {{"type", "string"}}}}},
                {"required", json::array({"first", "last"})},
                {"additionalProperties", false}}}}},
            {"required", json::array({"path"})},
            {"additionalProperties", false}}},
          {"kind",
           {{"type", "string"},
            {"enum", json::array({"text", "structural", "crdt-suspicious", "suspicious-merge"})}}},
          {"explanation", {{"type", "string"}}},
          {"proposal", {{"type", "string"}}},
          {"reasons", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"node", {{"type", "string"}}}}},
        {"required",
         json::array({"id", "base", "left", "right", "region", "kind", "explanation", "reasons"})},
        {"additionalProperties", false}};
}

json repository_diff_entry_schema() {
    return {{"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}}},
              {"old_path", {{"type", "string"}}},
              {"kind", {{"type", "string"}}},
              {"changes", {{"type", "array"}}},
              {"old_mode", {{"type", {"integer", "null"}}}},
              {"new_mode", {{"type", {"integer", "null"}}}},
              {"old_blob", {{"type", "string"}}},
              {"new_blob", {{"type", "string"}}},
              {"line_hunks", {{"type", {"array", "null"}}}},
              {"old_conflicts", {{"type", "array"}}},
              {"new_conflicts", {{"type", "array"}}}}}};
}

json repository_artifact_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"action", {{"type", "string"}}},
              {"exit_code", {{"type", "integer"}}},
              {"outputs", {{"type", "array"}}},
              {"timing", {{"type", "object"}}},
              {"cache_status", {{"type", "string"}}},
              {"divergent", {{"type", "boolean"}}},
              {"verification", {{"type", "string"}}},
              {"worker", {{"type", "string"}}},
              {"observed_at", {{"type", "string"}}},
              {"started_at", {{"type", "string"}}},
              {"finished_at", {{"type", "string"}}},
              {"attestations", {{"type", "array"}}},
              {"trace", {{"type", {"object", "null"}}}},
              {"members", {{"type", "array"}}}}}};
}

json repository_ref_json(const RepositoryContentRef& ref) {
    return {{"name", ref.name}, {"change", ref.change}};
}

json repository_tree_entry_json(const RepositoryTreeEntry& entry) {
    return {{"path", entry.path},
            {"kind", entry.kind},
            {"blob", entry.blob},
            {"mode", entry.mode},
            {"node_id", entry.node_id},
            {"size", entry.size},
            {"stored_size", entry.stored_size}};
}

json repository_file_json(const RepositoryFile& file) {
    std::span<const std::byte> bytes = file.raw_contents;
    std::vector<std::byte> text_bytes;
    if (bytes.empty() && !file.contents.empty()) {
        text_bytes.reserve(file.contents.size());
        std::ranges::transform(file.contents, std::back_inserter(text_bytes), [](char byte) {
            return static_cast<std::byte>(byte);
        });
        bytes = text_bytes;
    }
    std::string encoded((bytes.size() + 2) / 3 * 4, '\0');
    if (!bytes.empty()) {
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                        reinterpret_cast<const unsigned char*>(bytes.data()),
                        static_cast<int>(bytes.size()));
    }
    return {{"path", file.path},
            {"blob", file.blob},
            {"contents", file.contents},
            {"content_base64", std::move(encoded)},
            {"size", file.size}};
}

json repository_change_json(const RepositoryChange& change) {
    return {{"id", change.id},
            {"parents", change.parents},
            {"author", change.author},
            {"message", change.message},
            {"occurred_at", change.occurred_at}};
}

json repository_conflict_json(const RepositoryConflict& conflict) {
    json region{{"path", conflict.region.path}};
    if (conflict.region.bytes) {
        region["bytes"] = {{"offset", conflict.region.bytes->offset},
                           {"length", conflict.region.bytes->length}};
    }
    if (conflict.region.nodes) {
        region["nodes"] = {{"first", conflict.region.nodes->first},
                           {"last", conflict.region.nodes->last}};
    }
    json output{{"id", conflict.id},
                {"base", conflict.base},
                {"left", conflict.left},
                {"right", conflict.right},
                {"region", std::move(region)},
                {"kind", conflict.kind},
                {"explanation", conflict.explanation},
                {"reasons", conflict.reasons}};
    if (conflict.proposal) {
        output["proposal"] = *conflict.proposal;
    }
    if (conflict.node) {
        output["node"] = *conflict.node;
    }
    return output;
}

json repository_diff_entry_json(const RepositoryDiffEntry& entry) {
    json hunks = nullptr;
    if (entry.line_hunks) {
        hunks = json::array();
        for (const RepositoryLineHunk& hunk : *entry.line_hunks) {
            hunks.push_back({{"old_line", hunk.old_line},
                             {"old_count", hunk.old_count},
                             {"new_line", hunk.new_line},
                             {"new_count", hunk.new_count},
                             {"removed_lines", hunk.removed_lines},
                             {"added_lines", hunk.added_lines}});
        }
    }
    return {{"path", entry.path},
            {"old_path", entry.old_path},
            {"kind", entry.kind},
            {"changes", entry.changes},
            {"old_mode", entry.old_mode ? json(*entry.old_mode) : json(nullptr)},
            {"new_mode", entry.new_mode ? json(*entry.new_mode) : json(nullptr)},
            {"old_blob", entry.old_blob},
            {"new_blob", entry.new_blob},
            {"line_hunks", std::move(hunks)},
            {"old_conflicts", entry.old_conflicts},
            {"new_conflicts", entry.new_conflicts}};
}

json repository_artifact_json(const RepositoryArtifact& artifact) {
    json outputs = json::array();
    for (const RepositoryArtifactOutput& output : artifact.outputs) {
        outputs.push_back({{"path", output.path}, {"mode", output.mode}, {"blob", output.blob}});
    }
    json attestations = json::array();
    for (const RepositoryArtifactAttestation& attestation : artifact.attestations) {
        attestations.push_back({{"id", attestation.id},
                                {"profile", attestation.profile},
                                {"subject", attestation.subject},
                                {"issuer", attestation.issuer},
                                {"verified", attestation.verified},
                                {"reason", attestation.reason}});
    }
    json members = json::array();
    for (const RepositoryArtifactMember& member : artifact.members) {
        members.push_back({{"artifact", member.artifact},
                           {"version", member.version},
                           {"path", member.path},
                           {"digest", member.digest},
                           {"indexed", member.indexed},
                           {"reason", member.reason}});
    }
    json trace = nullptr;
    if (artifact.trace) {
        trace = {{"id", artifact.trace->id},
                 {"wall_ms", artifact.trace->wall_ms},
                 {"actions", artifact.trace->actions},
                 {"dependencies", artifact.trace->dependencies},
                 {"cache_hits", artifact.trace->cache_hits},
                 {"cache_misses", artifact.trace->cache_misses}};
    }
    return {{"id", artifact.id},
            {"action", artifact.action},
            {"exit_code", artifact.exit_code},
            {"outputs", std::move(outputs)},
            {"timing",
             {{"queued_ms", artifact.timing.queued_ms},
              {"sandbox_ms", artifact.timing.sandbox_ms},
              {"exec_ms", artifact.timing.exec_ms},
              {"hash_ms", artifact.timing.hash_ms},
              {"store_ms", artifact.timing.store_ms}}},
            {"cache_status", artifact.cache_status},
            {"divergent", artifact.divergent},
            {"verification", artifact.verification},
            {"worker", artifact.worker},
            {"observed_at", artifact.observed_at},
            {"started_at", artifact.started_at},
            {"finished_at", artifact.finished_at},
            {"attestations", std::move(attestations)},
            {"trace", std::move(trace)},
            {"members", std::move(members)}};
}

template <class T, class ToJson>
ApiResult repository_content_list_result(SyncStoreResult<std::vector<T>> rows, ToJson to_json) {
    if (!rows) {
        return from_sync_store_error(rows.error());
    }
    json items = json::array();
    for (const T& row : *rows) {
        items.push_back(to_json(row));
    }
    return ApiResult::success({{"items", std::move(items)}});
}

// The rows the caller may read: public ones for an anonymous caller, otherwise those its policy
// allows, in order.
std::expected<std::vector<control::Repository>, ApiResult>
authorized_repositories(control::Result<std::vector<control::Repository>> rows,
                        ApiContext& context) {
    if (!rows) {
        return std::unexpected(from_store_error(rows.error()));
    }
    std::vector<control::Repository> readable;
    if (!context.authorized) {
        for (control::Repository& repository : *rows) {
            if (repository.visibility == "public") {
                readable.push_back(std::move(repository));
            }
        }
        return readable;
    }
    std::vector<std::string> objects;
    objects.reserve(rows->size());
    std::map<std::string, std::string> objects_by_repository;
    for (const control::Repository& repository : *rows) {
        const auto object = context.store->find_policy_object("repository", repository.id);
        if (!object) {
            return std::unexpected(from_store_error(object.error()));
        }
        objects.push_back(object->id);
        objects_by_repository.emplace(repository.id, object->id);
    }
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, objects, "read", context.authorization_cache);
    if (!decisions) {
        return std::unexpected(from_store_error(decisions.error()));
    }
    for (control::Repository& repository : *rows) {
        if (decisions->at(objects_by_repository.at(repository.id)).allowed) {
            readable.push_back(std::move(repository));
        }
    }
    return readable;
}

ApiResult authorized_repository_list_result(control::Result<std::vector<control::Repository>> rows,
                                            ApiContext& context) {
    auto readable = authorized_repositories(std::move(rows), context);
    if (!readable) {
        return readable.error();
    }
    json items = json::array();
    for (const control::Repository& repository : *readable) {
        items.push_back(control::to_json(repository));
    }
    return ApiResult::success({{"items", std::move(items)}});
}

template <class T>
ApiResult authorized_entity_list_result(control::Result<std::vector<T>> rows,
                                        ApiContext& context,
                                        const std::string& object_type) {
    if (!rows) {
        return from_store_error(rows.error());
    }
    std::vector<std::string> objects;
    std::map<std::string, std::string> objects_by_entity;
    objects.reserve(rows->size());
    for (const T& row : *rows) {
        const auto object = context.store->find_policy_object(object_type, row.id);
        if (!object) {
            return from_store_error(object.error());
        }
        objects.push_back(object->id);
        objects_by_entity.emplace(row.id, object->id);
    }
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, objects, "read", context.authorization_cache);
    if (!decisions) {
        return from_store_error(decisions.error());
    }
    json items = json::array();
    for (const T& row : *rows) {
        if (decisions->at(objects_by_entity.at(row.id)).allowed) {
            items.push_back(control::to_json(row));
        }
    }
    return ApiResult::success({{"items", std::move(items)}});
}

std::expected<void, ApiError>
authorize_scope(ApiContext& context, const std::string& object_id, const std::string& action) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, {object_id}, action, context.authorization_cache);
    if (!decisions) {
        return std::unexpected(ApiError{decisions.error().code, decisions.error().message, {}});
    }
    if (!decisions->at(object_id).allowed) {
        return std::unexpected(
            ApiError{"unauthorized", "principal is not allowed to " + action, {}});
    }
    return {};
}

std::expected<void, ApiError> authorize_entity_scope(ApiContext& context,
                                                     const std::string& object_type,
                                                     const std::string& external_id,
                                                     const std::string& action) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object(object_type, external_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, {}});
    }
    return authorize_scope(context, object->id, action);
}

ApiResult authorization_failure(const std::expected<void, ApiError>& authorization) {
    return ApiResult::failure(
        authorization.error().code, authorization.error().message, authorization.error().path);
}

ApiResult authorize_user_collection(ApiContext& context) {
    if (!context.store) {
        return no_store();
    }
    if (context.acting_identity != "admin") {
        return ApiResult::failure("unauthorized", "principal is not allowed to list users");
    }
    const auto decisions = control::authorize_batch(*context.store,
                                                    context.acting_identity,
                                                    {"user collection"},
                                                    "admin",
                                                    context.authorization_cache);
    if (!decisions) {
        return from_store_error(decisions.error());
    }
    return decisions->at("user collection").allowed
               ? ApiResult::success(nullptr)
               : ApiResult::failure("unauthorized", "principal is not allowed to list users");
}

json search_schema() {
    return {{"type", "object"},
            {"properties",
             {{"customer", {{"type", "string"}}},
              {"repository", {{"type", "string"}}},
              {"path", {{"type", "string"}}},
              {"change", {{"type", "string"}}},
              {"snippet", {{"type", "string"}}},
              {"score", {{"type", "number"}}},
              {"artifact", {{"type", "string"}}},
              {"artifact_version", {{"type", "string"}}},
              {"digest", {{"type", "string"}}},
              {"line", {{"type", "integer"}}}}}};
}

json search_output_schema() {
    return {{"type", "object"},
            {"properties",
             {{"mode", {{"type", "string"}}},
              {"scanned_changes", {{"type", "integer"}}},
              {"items", {{"type", "array"}, {"items", search_schema()}}}}}};
}

ApiResult search_result(search::Result<search::QueryResult> result) {
    if (!result) {
        return ApiResult::failure(result.error().code, result.error().message);
    }
    json values = json::array();
    for (const search::Hit& hit : result->items) {
        values.push_back({{"customer", hit.customer},
                          {"repository", hit.repository},
                          {"path", hit.path},
                          {"change", hit.change},
                          {"snippet", hit.snippet},
                          {"score", hit.score},
                          {"artifact", hit.artifact},
                          {"artifact_version", hit.artifact_version},
                          {"digest", hit.digest},
                          {"line", hit.line}});
    }
    return ApiResult::success({{"mode", search::to_string(result->mode)},
                               {"scanned_changes", result->scanned_changes},
                               {"items", std::move(values)}});
}

std::expected<std::string, ApiError> global_object(const json&, ApiContext&) {
    return std::string{};
}

std::expected<std::string, ApiError> self_object(const json&, ApiContext& context) {
    return "principal/" + context.acting_identity;
}

CapabilityObjectResolver entity_object(std::string object_type, std::string input_field) {
    return [object_type = std::move(object_type), input_field = std::move(input_field)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        auto object = context.store->find_policy_object(object_type, input.at(input_field));
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/" + input_field});
        }
        return object->id;
    };
}

CapabilityObjectResolver optional_entity_object(std::string object_type, std::string input_field) {
    return [object_type = std::move(object_type), input_field = std::move(input_field)](
               const json& input, ApiContext& context) -> std::expected<std::string, ApiError> {
        const std::string external_id = input.value(input_field, "");
        if (external_id.empty()) {
            return std::string{};
        }
        auto object = context.store->find_policy_object(object_type, external_id);
        if (!object) {
            return std::unexpected(
                ApiError{object.error().code, object.error().message, "/" + input_field});
        }
        return object->id;
    };
}

CapabilityObjectResolver direct_object(std::string input_field) {
    return [input_field = std::move(input_field)](
               const json& input, ApiContext&) -> std::expected<std::string, ApiError> {
        return input.at(input_field).get<std::string>();
    };
}

std::expected<std::string, ApiError> entity_type_object(const json& input, ApiContext& context) {
    const auto object = context.store->find_policy_object(input.at("object_type"), input.at("id"));
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> scope_object(const json& input, ApiContext& context) {
    const std::string team_id = input.value("team_id", "");
    const std::string type = team_id.empty() ? "organization" : "team";
    const std::string external_id = team_id.empty() ? input.value("organization_id", "") : team_id;
    auto object = context.store->find_policy_object(type, external_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code,
                                        object.error().message,
                                        team_id.empty() ? "/organization_id" : "/team_id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> invitation_object(const json& input, ApiContext& context) {
    auto invitation = context.store->get_invitation(input.at("invitation_id"));
    if (!invitation) {
        return std::unexpected(
            ApiError{invitation.error().code, invitation.error().message, "/invitation_id"});
    }
    const std::string type = invitation->team_id.empty() ? "organization" : "team";
    const std::string external_id =
        invitation->team_id.empty() ? invitation->organization_id : invitation->team_id;
    auto object = context.store->find_policy_object(type, external_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, {}});
    }
    return object->id;
}

std::expected<std::string, ApiError> membership_scope_object(const json& input,
                                                             ApiContext& context) {
    const auto membership = context.store->get_membership(input.at("id"));
    if (!membership) {
        return std::unexpected(
            ApiError{membership.error().code, membership.error().message, "/id"});
    }
    const std::string type = membership->team_id.empty() ? "organization" : "team";
    const std::string external_id =
        membership->team_id.empty() ? membership->organization_id : membership->team_id;
    const auto object = context.store->find_policy_object(type, external_id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/id"});
    }
    return object->id;
}

std::expected<std::string, ApiError> sso_provider_object(const json& input, ApiContext& context) {
    const auto provider = context.store->get_sso_provider(input.at("provider_id"));
    if (!provider) {
        return std::unexpected(
            ApiError{provider.error().code, provider.error().message, "/provider_id"});
    }
    const auto object =
        context.store->find_policy_object("organization", provider->organization_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/provider_id"});
    }
    return object->id;
}

constexpr std::string_view scim_user_schema = "urn:ietf:params:scim:schemas:core:2.0:User";
constexpr std::string_view scim_group_schema = "urn:ietf:params:scim:schemas:core:2.0:Group";
constexpr std::string_view scim_taap_schema =
    "urn:orama:params:scim:schemas:extension:taap:2.0:User";

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string scim_email(const json& resource) {
    if (resource.contains("userName") && resource["userName"].is_string()) {
        return resource["userName"].get<std::string>();
    }
    for (const json& email : resource.value("emails", json::array())) {
        if (email.is_object() && email.contains("value") && email["value"].is_string()) {
            return email["value"].get<std::string>();
        }
    }
    return {};
}

std::string scim_taap_id(const json& resource) {
    if (resource.contains("taapId") && resource["taapId"].is_string()) {
        return resource["taapId"].get<std::string>();
    }
    const std::string extension(scim_taap_schema);
    if (resource.contains(extension) && resource[extension].is_object()) {
        return resource[extension].value("taapId", "");
    }
    return {};
}

std::string scim_team_name(std::string_view external_id) {
    std::string result;
    result.reserve(std::min<std::size_t>(external_id.size(), 256));
    bool last_was_hyphen = false;
    for (const unsigned char character : external_id) {
        if (std::isalnum(character)) {
            result.push_back(static_cast<char>(std::tolower(character)));
            last_was_hyphen = false;
        } else if (!result.empty() && !last_was_hyphen) {
            result.push_back('-');
            last_was_hyphen = true;
        }
        if (result.size() == 256) {
            break;
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    return result;
}

json scim_user_document(const control::ScimUser& user) {
    json resource = user.attributes.is_object() ? user.attributes : json::object();
    resource["schemas"] = json::array({scim_user_schema});
    resource["id"] = user.user_id;
    resource["externalId"] = user.external_id;
    resource["active"] = user.active;
    resource["meta"] = {{"resourceType", "User"},
                        {"created", user.created_at},
                        {"lastModified", user.updated_at},
                        {"location", "/scim/v2/Users/" + user.user_id}};
    return resource;
}

json scim_group_document(const control::ScimGroup& group) {
    json resource = group.attributes.is_object() ? group.attributes : json::object();
    resource["schemas"] = json::array({scim_group_schema});
    resource["id"] = group.team_id;
    resource["externalId"] = group.external_id;
    resource["displayName"] = group.display_name;
    resource["meta"] = {{"resourceType", "Group"},
                        {"created", group.created_at},
                        {"lastModified", group.updated_at},
                        {"location", "/scim/v2/Groups/" + group.team_id}};
    return resource;
}

std::expected<control::SsoProvider, ApiResult> scim_provider(const json& input,
                                                             ApiContext& context) {
    if (!context.store) {
        return std::unexpected(no_store());
    }
    const auto provider = context.store->authenticate_scim(input.value("bearer_token", ""));
    if (!provider) {
        return std::unexpected(from_store_error(provider.error()));
    }
    context.acting_identity = "sso-provider/" + provider->id;
    context.authentication_method = "scim";
    return *provider;
}

std::expected<bool, std::string> scim_filter_matches(const json& resource,
                                                     std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    const std::size_t separator = filter.find(" eq ");
    if (separator == std::string_view::npos) {
        return std::unexpected("SCIM filter must use the eq operator");
    }
    const std::string attribute(filter.substr(0, separator));
    std::string value(filter.substr(separator + 4));
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return std::unexpected("SCIM filter value must be quoted");
    }
    value = value.substr(1, value.size() - 2);
    if (!resource.contains(attribute) || !resource[attribute].is_string()) {
        return false;
    }
    return resource[attribute].get<std::string>() == value;
}

std::expected<json, std::string> apply_scim_patch(json resource, const json& request) {
    if (!request.is_object() || !request.contains("Operations") ||
        !request["Operations"].is_array()) {
        return std::unexpected("SCIM PATCH requires an Operations array");
    }
    for (const json& operation : request["Operations"]) {
        if (!operation.is_object()) {
            return std::unexpected("SCIM PATCH operation must be an object");
        }
        const std::string action = lower_ascii(operation.value("op", ""));
        const std::string path = operation.value("path", "");
        const json value = operation.value("value", json());
        if (action != "add" && action != "replace" && action != "remove") {
            return std::unexpected("SCIM PATCH operation is not supported");
        }
        if (path.empty()) {
            if (action == "remove" || !value.is_object()) {
                return std::unexpected("SCIM PATCH without a path requires an object value");
            }
            for (auto field = value.begin(); field != value.end(); ++field) {
                resource[field.key()] = field.value();
            }
            continue;
        }
        if (path.starts_with("members[value eq \"") && path.ends_with("\"]")) {
            if (action != "remove") {
                return std::unexpected("filtered member PATCH only supports remove");
            }
            const std::size_t start = std::string_view("members[value eq \"").size();
            const std::string member_id = path.substr(start, path.size() - start - 2);
            json members = json::array();
            for (const json& member : resource.value("members", json::array())) {
                if (!member.is_object() || member.value("value", "") != member_id) {
                    members.push_back(member);
                }
            }
            resource["members"] = std::move(members);
            continue;
        }
        if (action == "remove") {
            resource.erase(path);
            continue;
        }
        if (action == "add" && path == "members" && resource.contains(path) &&
            resource[path].is_array() && value.is_array()) {
            for (const json& member : value) {
                resource[path].push_back(member);
            }
            continue;
        }
        resource[path] = value;
    }
    return resource;
}

std::expected<control::NewScimUser, ApiResult> new_scim_user(const std::string& provider_id,
                                                             json resource) {
    const std::string external_id = resource.value("externalId", resource.value("userName", ""));
    const std::string taap_id = scim_taap_id(resource);
    if (external_id.empty() || taap_id.empty()) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "SCIM user requires externalId or userName and a TAAP identity"));
    }
    resource["externalId"] = external_id;
    return control::NewScimUser{provider_id,
                                external_id,
                                scim_email(resource),
                                resource.value("displayName", ""),
                                taap_id,
                                resource.value("active", true),
                                std::move(resource)};
}

std::expected<control::NewScimGroup, ApiResult>
new_scim_group(control::Store& store, const std::string& provider_id, json resource) {
    const std::string external_id = resource.value("externalId", "");
    const std::string display_name = resource.value("displayName", "");
    const std::string team_name = scim_team_name(external_id);
    if (external_id.empty() || display_name.empty() || !control::valid_handle(team_name)) {
        return std::unexpected(ApiResult::failure(
            "invalid_input", "SCIM group requires a usable externalId and displayName"));
    }
    std::vector<std::string> members;
    for (const json& member : resource.value("members", json::array())) {
        if (!member.is_object() || !member.contains("value") || !member["value"].is_string()) {
            return std::unexpected(
                ApiResult::failure("invalid_input", "SCIM group member requires a value"));
        }
        const auto user = store.get_scim_user(provider_id, member["value"]);
        if (!user) {
            return std::unexpected(from_store_error(user.error()));
        }
        members.push_back(user->external_id);
    }
    resource["externalId"] = external_id;
    return control::NewScimGroup{
        provider_id, external_id, team_name, display_name, std::move(members), std::move(resource)};
}

ApiResult scim_list_response(std::vector<json> resources, int start_index, int count) {
    const int safe_start = std::max(1, start_index);
    const int safe_count = std::clamp(count, 0, 1000);
    const std::size_t offset = static_cast<std::size_t>(safe_start - 1);
    json page = json::array();
    if (offset < resources.size()) {
        const std::size_t end =
            std::min(resources.size(), offset + static_cast<std::size_t>(safe_count));
        for (std::size_t index = offset; index < end; ++index) {
            page.push_back(std::move(resources[index]));
        }
    }
    return ApiResult::success(
        {{"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:ListResponse"})},
         {"totalResults", resources.size()},
         {"startIndex", safe_start},
         {"itemsPerPage", page.size()},
         {"Resources", std::move(page)}});
}

json id_input() {
    return {{"type", "object"},
            {"properties", {{"id", {{"type", "string"}, {"description", "uuid"}}}}},
            {"required", json::array({"id"})},
            {"additionalProperties", false}};
}
json list_input() {
    return {{"type", "object"},
            {"properties",
             {{"limit",
               {{"type", "integer"}, {"description", "max rows, default 100, newest first"}}}}},
            {"additionalProperties", false}};
}
json user_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"taap_id", {{"type", json::array({"string", "null"})}}},
              {"email", {{"type", json::array({"string", "null"})}}},
              {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json organization_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"taap_id", {{"type", json::array({"string", "null"})}}},
              {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json customer_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"customer_type",
               {{"type", "string"}, {"enum", json::array({"individual", "organization"})}}},
              {"user_id", {{"type", json::array({"string", "null"})}}},
              {"organization_id", {{"type", json::array({"string", "null"})}}},
              {"name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json repository_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"customer_id", {{"type", "string"}}},
              {"workspace_id",
               {{"type", json::array({"string", "null"})},
                {"description",
                 "The workspace the repository belongs to; it never changes. Null only for a "
                 "repository created before workspaces that still needs one assigned."}}},
              {"name", {{"type", "string"}}},
              {"description",
               {{"type", "string"},
                {"description",
                 "From the repository's settings record in its workspace (lake#315); empty when "
                 "it has none."}}},
              {"description_unavailable",
               {{"type", "string"},
                {"description",
                 "Present only when the description could not be read from the repository's "
                 "workspace (no workspace yet, or the workspace refused): the reason. The "
                 "description is then empty."}}},
              {"visibility",
               {{"type", "string"}, {"enum", json::array({"private", "organization", "public"})}}},
              {"deleted_at", {{"type", json::array({"string", "null"})}}},
              {"retention_until", {{"type", json::array({"string", "null"})}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json sync_ref_schema() {
    return {{"type", "object"},
            {"properties",
             {{"repository_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"head", {{"type", "string"}}}}}};
}

std::string timestamp_after(std::chrono::seconds offset) {
    const std::time_t value = std::time(nullptr) + offset.count();
    std::tm timestamp{};
    gmtime_r(&value, &timestamp);
    char result[sizeof "1970-01-01T00:00:00Z"];
    std::strftime(result, sizeof result, "%FT%TZ", &timestamp);
    return result;
}
json team_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"organization_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"display_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json membership_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"organization_id", {{"type", "string"}}},
              {"team_id", {{"type", json::array({"string", "null"})}}},
              {"principal_type", {{"type", "string"}}},
              {"principal_id", {{"type", "string"}}},
              {"principal_taap_id", {{"type", "string"}}},
              {"role", {{"type", "string"}}},
              {"attestation_digest", {{"type", "string"}}},
              {"granted_by_taap_id", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json invitation_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"organization_id", {{"type", "string"}}},
              {"team_id", {{"type", json::array({"string", "null"})}}},
              {"invitee_type", {{"type", "string"}}},
              {"invitee", {{"type", "string"}}},
              {"role", {{"type", "string"}}},
              {"expires_at", {{"type", "string"}}},
              {"status", {{"type", "string"}}},
              {"accepted_membership_id", {{"type", json::array({"string", "null"})}}},
              {"created_by_taap_id", {{"type", "string"}}},
              {"organization_name", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json object_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"object_type", {{"type", "string"}}},
              {"external_id", {{"type", "string"}}},
              {"parent_id", {{"type", json::array({"string", "null"})}}},
              {"inherit", {{"type", "boolean"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json grant_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"principal_taap_id", {{"type", "string"}}},
              {"object_id", {{"type", "string"}}},
              {"role", {{"type", "string"}}},
              {"attestation_digest", {{"type", "string"}}},
              {"granted_by_taap_id", {{"type", "string"}}},
              {"created_at", {{"type", "string"}}},
              {"updated_at", {{"type", "string"}}}}}};
}
json list_schema(json item) {
    return {{"type", "object"},
            {"properties", {{"items", {{"type", "array"}, {"items", std::move(item)}}}}}};
}
json session_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"user_id", {{"type", {"string", "null"}}}},
              {"device_id", {{"type", {"string", "null"}}}},
              {"pool_id", {{"type", "string"}}},
              {"parent_session_id", {{"type", {"string", "null"}}}},
              {"kind", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"scopes", {{"type", "array"}}},
              {"expires_at", {{"type", {"string", "null"}}}},
              {"revoked_at", {{"type", {"string", "null"}}}},
              {"created_at", {{"type", "string"}}}}}};
}
json device_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"user_id", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"passkey_credential_id", {{"type", {"string", "null"}}}},
              {"passkey_binding_attestation", {{"type", {"object", "null"}}}},
              {"revoked_at", {{"type", {"string", "null"}}}},
              {"created_at", {{"type", "string"}}}}}};
}

int limit_of(const json& input) {
    const int limit = input.value("limit", 100);
    return limit <= 0 ? 100 : (limit > 1000 ? 1000 : limit);
}

json audit_event_schema() {
    return {{"type", "object"}};
}

json audit_filter_input() {
    return {{"type", "object"},
            {"properties",
             {{"principal", {{"type", "string"}}},
              {"request_id", {{"type", "string"}}},
              {"after_id", {{"type", "string"}}},
              {"object", {{"type", "string"}}},
              {"outcome", {{"type", "string"}}},
              {"from", {{"type", "string"}}},
              {"to", {{"type", "string"}}},
              {"limit", {{"type", "integer"}}},
              {"order", {{"type", "string"}, {"enum", json::array({"newest", "oldest"})}}},
              {"include_reads", {{"type", "boolean"}}},
              // Decision 52 (docs/api/audit.md#which-log): which logs a read covers.
              {"workspace_id",
               {{"type", "string"},
                {"description", "Read only this workspace's audit (its Zetta's chain)."}}},
              {"customer_id",
               {{"type", "string"},
                {"description", "With the central log, read the audits of this space's live "
                                "workspaces (by default, every workspace in scope)."}}},
              {"log",
               {{"type", "string"},
                {"enum", json::array({"all", "central"})},
                {"description", "`central`: the central log alone. `all` (the default): the "
                                "central log and the workspaces' audits."}}}}},
            {"additionalProperties", false}};
}

// How a query reads the log when its input does not say (lake#445). Queries a person reads put
// the newest events first; export keeps append order, the order the chain verifies in.
struct AuditDefaults {
    bool newest_first = true;
    bool include_reads = true;
};

// The capabilities whose events are reads: a Read capability, or one the policy checks as a
// read (workspace_query). A browser polls these, so a person's activity leaves out their
// successful events unless asked (lake#445).
std::vector<std::string> read_capability_names(const ApiContext& context) {
    std::vector<std::string> names;
    if (context.registry == nullptr) {
        return names;
    }
    for (const auto& capability : context.registry->list()) {
        if (capability->kind == CapabilityKind::Read || capability->action == "read") {
            names.push_back(capability->name);
        }
    }
    return names;
}

audit::Filter
audit_filter_of(const json& input, const ApiContext& context, AuditDefaults defaults = {}) {
    audit::Filter filter{
        .principal = input.value("principal", ""),
        .object = input.value("object", ""),
        .outcome = input.value("outcome", ""),
        .from = input.value("from", ""),
        .to = input.value("to", ""),
        .limit = limit_of(input),
        .request_id = input.value("request_id", ""),
        .after_id = input.value("after_id", ""),
    };
    // after_id continues a page in append order; an explicit order always wins.
    filter.newest_first = input.contains("order")
                              ? input.at("order") == "newest"
                              : defaults.newest_first && filter.after_id.empty();
    if (!input.value("include_reads", defaults.include_reads)) {
        filter.omit_successful_actions = read_capability_names(context);
    }
    return filter;
}

// What people call an audit principal (lake#445): a person's or organization's name, a library
// agent's name, or a plain description of a lake credential. An identity lake cannot name stays
// as recorded.
std::string principal_display_name(const std::string& principal,
                                   const std::map<std::string, std::string>& names) {
    if (const auto found = names.find(principal); found != names.end()) {
        return found->second;
    }
    const auto agent_name = [](std::string_view key) {
        const control::LibraryAgent* agent = control::library_agent(key);
        return agent ? std::string(agent->name) : std::string(key);
    };
    if (principal == "admin") {
        return "Lake operator";
    }
    if (principal == "anonymous") {
        return "Anonymous";
    }
    if (principal.starts_with("agent-service:")) {
        return agent_name(std::string_view(principal).substr(14));
    }
    if (principal.starts_with("app:")) {
        return "App " + principal.substr(4, principal.find(':', 4) - 4);
    }
    if (principal.starts_with("build:")) {
        return "Build " + principal.substr(6, principal.find(':', 6) - 6);
    }
    if (principal.starts_with("share-link:")) {
        return "Share link";
    }
    if (principal.starts_with("sso-provider/")) {
        return "SSO provider";
    }
    return principal;
}

// Audit events as a person reads them: each carries `principal_name` and, for a workspace call,
// the inner `capability` it called (lake#445). The signed fields are unchanged.
ApiResult audit_items(const std::vector<audit::Event>& events, ApiContext& context) {
    std::vector<std::string> identities;
    for (const audit::Event& event : events) {
        if (std::ranges::find(identities, event.draft.principal) == identities.end()) {
            identities.push_back(event.draft.principal);
        }
    }
    std::map<std::string, std::string> names;
    if (auto found = context.store->principal_names(identities)) {
        names = std::move(*found);
    }
    json items = json::array();
    for (const audit::Event& event : events) {
        json item = audit::to_json(event);
        std::string name = principal_display_name(event.draft.principal, names);
        const json& details = event.draft.details;
        if (const auto agent = details.find("acting_agent");
            agent != details.end() && agent->is_string()) {
            const control::LibraryAgent* library = control::library_agent(agent->get<std::string>());
            name = (library ? std::string(library->name) : agent->get<std::string>()) + " for " +
                   name;
        }
        item["principal_name"] = std::move(name);
        if (const auto capability = details.find("capability");
            capability != details.end() && capability->is_string()) {
            item["capability"] = *capability;
        }
        items.push_back(std::move(item));
    }
    return ApiResult::success({{"items", std::move(items)}});
}

// audit_items over events read from both logs (decision 52): each item says which log it came
// from, the workspace and its place in that workspace's chain, and whether it is still queued in
// this process; the answer names the workspaces that could not be read.
ApiResult logged_audit_items(const AuditRead& read, ApiContext& context) {
    std::vector<audit::Event> events;
    events.reserve(read.events.size());
    for (const LoggedEvent& logged : read.events) {
        events.push_back(logged.event);
    }
    ApiResult result = audit_items(events, context);
    if (!result.ok) {
        return result;
    }
    json& items = result.value["items"];
    for (std::size_t index = 0; index < read.events.size(); ++index) {
        const LoggedEvent& logged = read.events[index];
        json& item = items[index];
        item["log"] = logged.log;
        if (!logged.workspace_id.empty()) {
            item["workspace_id"] = logged.workspace_id;
        }
        if (logged.sequence > 0) {
            item["sequence"] = logged.sequence;
        }
        if (logged.pending) {
            item["pending"] = true;
        }
    }
    result.value["unread_workspaces"] = read.unread;
    return result;
}

audit::IdentityOptions identity_options(const ApiContext& context) {
    return audit::IdentityOptions{
        .configured_seed = context.identity_seed,
        .instance_kek = context.instance_kek,
    };
}

ApiResult from_sync_error(ApiContext& context, SyncStoreError error) {
    const std::string code = error.code;
    const std::string message = error.message;
    context.sync_error = std::move(error);
    return ApiResult::failure(code, message);
}

// Counts a sync request's bytes for its repository's workspace (lake#348, docs/api/metering.md):
// an in-memory increment the recorder writes to that workspace's hourly record off the request
// path. The repository's stored bytes are measured on each usage read, not here.
void record_transfer(ApiContext& context,
                     const control::Repository& repository,
                     std::uint64_t incoming,
                     std::uint64_t outgoing) {
    if (context.metering_recorder) {
        context.metering_recorder->add_transfer(repository, incoming, outgoing);
    }
    context.audit_details["metering"] = {
        {"transfer_in_bytes", incoming},
        {"transfer_out_bytes", outgoing},
        {"workspace_id", repository.workspace_id},
        {"recorded", context.metering_recorder != nullptr && !repository.workspace_id.empty()}};
}

SyncStoreResult<void> record_branch_protection_event(ApiContext& context,
                                                     const control::Repository& repository,
                                                     const BranchRefUpdate& update,
                                                     const std::string& rule,
                                                     bool allowed,
                                                     const std::string& reason) {
    const std::string& repository_id = repository.id;
    const audit::EventDraft event{
        .customer_id = repository.customer_id,
        .kind = allowed ? "forge.branch_protection.allowed" : "forge.branch_protection.refused",
        .principal = context.acting_identity.empty() ? "anonymous" : context.acting_identity,
        .object = "repository/" + repository_id + "/ref/" + update.name,
        .action = "reference_update",
        .outcome = allowed ? "success" : "denied",
        .request_id = context.request_id,
        .method = context.authentication_method,
        .device = context.device,
        .details = {{"repository", repository_id},
                    {"workspace_id", repository.workspace_id},
                    {"reference", update.name},
                    {"caller",
                     context.acting_identity.empty() ? "anonymous" : context.acting_identity},
                    {"rule", rule},
                    {"reason", reason}},
    };
    // A decision about a repository's reference is the repository's data: its workspace's audit.
    const auto recorded =
        record_audit(context, event, workspace_audit_route(context, repository.workspace_id));
    if (!recorded) {
        return std::unexpected(
            SyncStoreError{recorded.error().code, recorded.error().message, update.name, rule});
    }
    return {};
}

bool offline_grant_fields_match(const control::OfflineGrant& grant) {
    const json& credential = grant.credential;
    if (!credential.is_object() || !credential.contains("attestation_identifier") ||
        !credential.at("attestation_identifier").is_string() ||
        !credential.contains("subject_identifier") ||
        !credential.at("subject_identifier").is_string() ||
        !credential.contains("asserted_attributes") ||
        !credential.at("asserted_attributes").is_object()) {
        return false;
    }
    const json& attributes = credential.at("asserted_attributes");
    return attributes.contains("device_id") && attributes.at("device_id").is_string() &&
           credential.at("attestation_identifier").get<std::string>() == grant.id &&
           credential.at("subject_identifier").get<std::string>() == grant.principal_taap_id &&
           attributes.at("device_id").get<std::string>() == grant.device_id;
}

// The repository, with its workspace (lake#315), is resolved once by the caller; every branch
// protection decision and its audit record carry that workspace. `protection` is what the caller
// read from that workspace before the push (load_branch_protection); when it could not be read,
// every reference update is refused with its reason instead of being decided without its rules.
SyncPushAuthorizer
sync_change_authorizer(ApiContext& context, const control::Repository& repository,
                       std::expected<BranchProtectionRecords, SyncStoreError> protection) {
    const std::string customer_id = repository.customer_id;
    const std::string repository_id = repository.id;
    return [&context, repository, customer_id, repository_id,
            protection = std::move(protection)](
               const SyncPushItem& item,
               std::span<const BranchRefUpdate> updates)
               -> SyncStoreResult<std::vector<SyncRefVerdict>> {
        if (item.kind == SyncPushItemKind::Reference) {
            std::vector<SyncRefVerdict> verdicts;
            verdicts.reserve(updates.size());
            for (const BranchRefUpdate& update : updates) {
                if (!protection) {
                    // The rules could not be read: refuse rather than decide without them.
                    const auto recorded = record_branch_protection_event(
                        context, repository, update, "branch_protection_unavailable", false,
                        protection.error().message);
                    if (!recorded) {
                        return std::unexpected(recorded.error());
                    }
                    verdicts.push_back(
                        {update.name, SyncRefVerdictKind::Refused, protection.error().message});
                    continue;
                }
                const auto evaluated = evaluate_branch_protection(*context.store,
                                                                  context.acting_identity,
                                                                  context.authorization_cache,
                                                                  *protection,
                                                                  update);
                if (!evaluated) {
                    const auto recorded = record_branch_protection_event(context,
                                                                         repository,
                                                                         update,
                                                                         evaluated.error().rule,
                                                                         false,
                                                                         evaluated.error().message);
                    if (!recorded) {
                        return std::unexpected(recorded.error());
                    }
                    verdicts.push_back(
                        {update.name, SyncRefVerdictKind::Refused, evaluated.error().message});
                    continue;
                }
                for (const std::string& rule : evaluated->matched_rules) {
                    const auto recorded =
                        record_branch_protection_event(context,
                                                       repository,
                                                       update,
                                                       rule,
                                                       true,
                                                       "branch protection allows the update");
                    if (!recorded) {
                        return std::unexpected(recorded.error());
                    }
                }
                verdicts.push_back({update.name, SyncRefVerdictKind::Accepted, {}});
            }
            return verdicts;
        }

        const std::string& change_id = item.change_id;
        std::string denial_code;
        std::string denial_reason;
        if (!context.offline_grant_id.empty()) {
            const auto grant = context.store->get_offline_grant(context.offline_grant_id);
            if (!grant) {
                denial_code = grant.error().code;
                denial_reason = "offline grant is not recognized at publication";
            } else if (grant->principal_taap_id != context.acting_identity ||
                       grant->user_id != context.acting_user_id ||
                       grant->device_id != context.credential_device_id) {
                denial_code = "unauthorized";
                denial_reason = "offline grant does not belong to the publishing credential";
            } else if (!grant->revoked_at.empty()) {
                denial_code = "unauthorized";
                denial_reason = "offline grant was revoked before publication";
            } else if (!offline_grant_fields_match(*grant)) {
                denial_code = "unauthorized";
                denial_reason = "offline grant signed fields do not match its stored record";
            } else {
                const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count();
                const std::string scope = "repository:" + repository_id + ":read";
                const auto verified =
                    audit::verify_offline_grant(*context.store, grant->credential, scope, now);
                if (!verified) {
                    denial_code = verified.error().code;
                    denial_reason = verified.error().message;
                }
            }
        }
        auto object = context.store->find_policy_object("repository", repository_id);
        if (!object && denial_reason.empty()) {
            return std::unexpected(SyncStoreError{
                object.error().code, object.error().message, "repository authorization"});
        }
        control::Result<control::AuthorizationDecision> decision =
            std::unexpected(control::StoreError{denial_code, denial_reason});
        if (denial_reason.empty()) {
            decision = control::authorize(*context.store,
                                          context.acting_identity,
                                          object->id,
                                          "write",
                                          context.authorization_cache);
        }
        const bool allowed = decision && decision->allowed;
        const std::string error_code =
            !decision ? decision.error().code : (allowed ? "" : "unauthorized");
        const std::string outcome =
            allowed ? "success" : (error_code == "unauthorized" ? "denied" : "failure");
        const std::string reason = !decision
                                       ? decision.error().message
                                       : (allowed ? "current policy permits publication"
                                                  : "current policy no longer permits writes");
        context.reauthorization_results.push_back(
            {{"change", change_id}, {"allowed", allowed}, {"reason", reason}});
        const audit::EventDraft event{
            .kind = "repository_sync",
            .principal = context.acting_identity.empty() ? "anonymous" : context.acting_identity,
            .object = "repository/" + repository_id,
            .action = "push_change",
            .outcome = outcome,
            .request_id = context.request_id,
            .method = context.authentication_method,
            .device = context.device,
            .details = {{"change", change_id},
                        {"error_code", error_code},
                        {"offline_grant_id", context.offline_grant_id},
                        {"reason", reason}},
        };
        // Publication into a repository is its data: the repository's workspace audit. A refusal
        // for want of authority stays central, as a refused capability call does.
        const auto pushed_into = context.store->get_repository(repository_id);
        const AuditRoute route = outcome != "denied" && pushed_into
                                     ? workspace_audit_route(context, pushed_into->workspace_id)
                                     : AuditRoute{};
        auto recorded = record_audit(context, event, route);
        if (!recorded) {
            return std::unexpected(SyncStoreError{
                recorded.error().code, recorded.error().message, "repository sync audit"});
        }
        if (!decision) {
            return std::unexpected(
                SyncStoreError{decision.error().code, decision.error().message, change_id});
        }
        if (!decision->allowed) {
            return std::unexpected(SyncStoreError{"unauthorized", reason, change_id});
        }
        std::vector<SyncRefVerdict> verdicts;
        verdicts.reserve(updates.size());
        for (const BranchRefUpdate& update : updates) {
            verdicts.push_back({update.name, SyncRefVerdictKind::Accepted, {}});
        }
        return verdicts;
    };
}

std::expected<control::Repository, ApiResult> live_sync_repository(const json& input,
                                                                   ApiContext& context) {
    if (!context.store) {
        return std::unexpected(no_store());
    }
    if (!context.sync_store) {
        return std::unexpected(
            ApiResult::failure("store_unavailable", "LAKE_STORE_ROOT is not configured"));
    }
    auto repository = context.store->get_repository(input.at("repository_id"));
    if (!repository) {
        return std::unexpected(from_store_error(repository.error()));
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(ApiResult::failure("conflict", "repository is tombstoned"));
    }
    return *repository;
}

std::expected<std::uint64_t, ApiResult> storage_quota_for(const std::string& customer_id,
                                                          ApiContext& context) {
    const auto quota = context.store->storage_quota(customer_id);
    if (!quota) {
        return std::unexpected(from_store_error(quota.error()));
    }
    return quota->value_or(context.metering.storage_quota_bytes);
}

ApiResult require_sync_wire(ApiContext& context) {
    if (context.sync_wire_request) {
        return ApiResult::success(nullptr);
    }
    return ApiResult::failure("invalid_input",
                              "this capability requires the repository sync wire route");
}

template <class T> ApiResult list_result(control::Result<std::vector<T>> rows) {
    if (!rows)
        return from_store_error(rows.error());
    json items = json::array();
    for (const auto& row : *rows)
        items.push_back(control::to_json(row));
    return ApiResult::success({{"items", std::move(items)}});
}

template <class T> ApiResult one_result(control::Result<T> row) {
    if (!row)
        return from_store_error(row.error());
    return ApiResult::success(control::to_json(*row));
}

bool known_token_scope(const std::string& scope, const Registry* registry) {
    if (scope == "*" || scope == "registry_pull") {
        return true;
    }
    return registry != nullptr && registry->find(scope) != nullptr;
}

std::expected<control::NewToken, ApiError> attenuated_token(const json& input,
                                                            const ApiContext& context) {
    const bool is_static = input.value("static", false);
    if (input.contains("expires_in_seconds") &&
        input.at("expires_in_seconds").is_number_unsigned() &&
        input.at("expires_in_seconds").get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(
            ApiError{"invalid_input", "expires_in_seconds is too large", "/expires_in_seconds"});
    }
    const auto scopes = input.at("scopes").get<std::vector<std::string>>();
    // A scope is a capability name, `*`, or `registry_pull` (docs/api/auth.md#scopes); anything
    // else would grant nothing, so it is refused rather than stored (lake#450).
    for (std::size_t index = 0; index < scopes.size(); ++index) {
        if (!known_token_scope(scopes[index], context.registry)) {
            return std::unexpected(ApiError{
                "invalid_input",
                "unknown scope '" + scopes[index] +
                    "': a scope is a capability name (GET /v1/openapi.json lists them), * for "
                    "every capability, or registry_pull to pull images",
                "/scopes/" + std::to_string(index)});
        }
    }
    const auto requested = input.value("expires_in_seconds", std::int64_t{60 * 60 * 24 * 30});
    if (input.at("name").get<std::string>().empty()) {
        return std::unexpected(ApiError{"invalid_input", "a token needs a name", "/name"});
    }
    if (is_static && input.value("reason", "").empty()) {
        return std::unexpected(
            ApiError{"invalid_input", "a static token needs a reason", "/reason"});
    }
    if (!is_static && (requested <= 0 || requested > std::numeric_limits<int>::max())) {
        return std::unexpected(ApiError{
            "invalid_input", "expires_in_seconds must be positive", "/expires_in_seconds"});
    }
    control::NewToken token{context.acting_user_id,
                            context.pool_id,
                            input.at("name"),
                            input.value("reason", ""),
                            scopes,
                            is_static ? 0 : static_cast<int>(requested),
                            is_static};
    if (context.operator_credential) {
        return token;
    }
    if (!context.session_credential || context.credential_session_id.empty()) {
        return std::unexpected(
            ApiError{"unauthorized", "token issuance requires an issuing session"});
    }
    const bool wildcard = std::ranges::find(context.scopes, "*") != context.scopes.end();
    for (const auto& scope : scopes) {
        if (!wildcard && std::ranges::find(context.scopes, scope) == context.scopes.end()) {
            return std::unexpected(
                ApiError{"unauthorized", "requested scopes exceed issuing credential"});
        }
    }
    if (input.value("never_expires", false)) {
        // A standing token (lake#234): no expiry and no parent, so signing out does not end it.
        // Its authority is still the person's live grants on every request, and revoking it,
        // revoking the person's sessions or suspending them still stops it.
        if (input.value("reason", "").empty() || std::ranges::find(scopes, "*") != scopes.end()) {
            return std::unexpected(
                ApiError{"invalid_input",
                         "a token that never expires needs a reason and named scopes (not *)",
                         input.value("reason", "").empty() ? "/reason" : "/scopes"});
        }
        if (!context.non_expiring_credentials_allowed) {
            return std::unexpected(ApiError{"unauthorized",
                                            "this lake does not allow credentials that never "
                                            "expire (LAKE_ALLOW_NON_EXPIRING_CREDENTIALS)"});
        }
        const auto customers = context.store->list_customers(10, context.acting_user_id);
        const auto personal =
            customers
                ? std::ranges::find(*customers, "individual", &control::Customer::customer_type)
                : std::vector<control::Customer>::const_iterator{};
        if (!customers || personal == customers->end()) {
            return std::unexpected(ApiError{"unauthorized", "no personal space to check"});
        }
        const auto allowed = context.store->customer_allows_non_expiring_credentials(personal->id);
        if (!allowed || !*allowed) {
            return std::unexpected(
                ApiError{"unauthorized",
                         "credentials that never expire are off for your space; turn on "
                         "allow_non_expiring_credentials with set_customer_credential_policy"});
        }
        token.is_static = true;
        token.expires_in_seconds = 0;
        return token;
    }
    if (is_static && (context.credential_kind != "static" || context.credential_expires_at_unix)) {
        return std::unexpected(
            ApiError{"unauthorized", "issuing credential cannot derive a static token"});
    }
    if (context.credential_expires_at_unix) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto remaining = *context.credential_expires_at_unix - now;
        if (remaining <= 0 || (input.contains("expires_in_seconds") && requested > remaining)) {
            return std::unexpected(
                ApiError{"unauthorized", "requested lifetime exceeds issuing credential"});
        }
        token.expires_in_seconds = static_cast<int>(std::min(requested, remaining));
    } else if (context.credential_kind != "static") {
        return std::unexpected(
            ApiError{"unauthorized", "issuing credential has no lifetime authority"});
    }
    token.parent_session_id = context.credential_session_id;
    return token;
}

ApiResult session_answer(std::expected<ApiResult, liquid::error> completed) {
    return completed ? std::move(*completed)
                     : ApiResult::failure("store_error", completed.error().message);
}

// A session the store issued, completed by application_session_result: in an application's pool
// the user's record is read in the application's workspace (lake#353), so this is asynchronous.
liquid::runtime::task<ApiResult> issued_result(control::Result<control::IssuedSession> issued,
                                               ApiContext& context,
                                               std::string lifecycle_event = "session.started") {
    co_return session_answer(co_await application_session_result(
        std::move(issued), context, std::move(lifecycle_event)));
}

ApiResult authorized_object_list_result(control::Result<std::vector<control::PolicyObject>> rows,
                                        ApiContext& context) {
    if (!rows) {
        return from_store_error(rows.error());
    }
    std::vector<std::string> object_ids;
    object_ids.reserve(rows->size());
    for (const auto& row : *rows) {
        object_ids.push_back(row.id);
    }
    auto decisions = control::authorize_batch(
        *context.store, context.acting_identity, object_ids, "read", context.authorization_cache);
    if (!decisions) {
        return from_store_error(decisions.error());
    }
    json items = json::array();
    for (const auto& row : *rows) {
        if (decisions->at(row.id).allowed) {
            items.push_back(control::to_json(row));
        }
    }
    return ApiResult::success({{"items", std::move(items)}});
}

} // namespace

control::Result<control::Repository> create_repository_row(ApiContext& context,
                                                           control::NewRepository in) {
    if (!context.sync_store) {
        return context.store->create_repository(in);
    }
    if (in.id.empty()) {
        in.id = control::new_uuid_v4();
    }
    std::optional<control::Result<control::Repository>> row;
    const auto content = context.sync_store->create_repository_content(
        in.customer_id, in.workspace_id, in.id, [&]() -> SyncStoreResult<void> {
            row = context.store->create_repository(in);
            if (!*row) {
                return std::unexpected(SyncStoreError{row->error().code, row->error().message});
            }
            return {};
        });
    if (row && !*row) {
        return *row;
    }
    if (!content) {
        if (row) {
            // The row exists but its workspace store refused the repository: tombstone the row with
            // its retention ended, which frees the name, so the creation can simply be retried.
            static_cast<void>(context.store->delete_repository(
                (*row)->id, control::RepositoryDeletion{.deleted_at = timestamp_after(std::chrono::seconds::zero()),
                                                        .retention_until = timestamp_after(std::chrono::seconds::zero())}));
        }
        // Not `conflict`: callers read that as a taken name.
        const std::string code =
            content.error().code == "conflict" ? std::string("store_error") : content.error().code;
        return std::unexpected(control::StoreError{
            code, "the repository could not be created in its workspace store; retry: " +
                      content.error().message});
    }
    return *row;
}

std::expected<control::Workspace, ApiError> repository_workspace(ApiContext& context,
                                                                 const nlohmann::json& input) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string workspace_id =
        input.contains("workspace_id") && input.at("workspace_id").is_string()
            ? input.at("workspace_id").get<std::string>()
            : std::string();
    const std::string customer_id =
        input.contains("customer_id") && input.at("customer_id").is_string()
            ? input.at("customer_id").get<std::string>()
            : std::string();
    const auto workspace = workspace_id.empty()
        ? control::Result<control::Workspace>(
              std::unexpect, control::StoreError{"not_found", "no workspace"})
        : context.store->get_workspace(workspace_id);
    if (!workspace || workspace->desired == "deleted" || workspace->customer_id != customer_id) {
        return std::unexpected(ApiError{"invalid_input",
                                        "workspace_id must name a workspace of the repository's "
                                        "space; a repository belongs to one workspace",
                                        "/workspace_id"});
    }
    return *workspace;
}

namespace {

void register_server_info(Registry& registry, std::string_view version) {
    const std::string version_string(version);
    registry.add(Capability{
        .name = "server_info",
        .title = "Server info",
        .description = "Name, node, version and API level of this lake. No authentication.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"name", {{"type", "string"}}},
                            {"node", {{"type", "string"}}},
                            {"version", {{"type", "string"}}},
                            {"revision", {{"type", "string"}}},
                            {"api", {{"type", "string"}}},
                            {"store", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "server",
        .action = "read",
        .resolve_object = global_object,
        .handler = [version_string](const json&, ApiContext& ctx) {
            return ApiResult::success({{"name", "lake"},
                                       {"node", ctx.node_name},
                                       {"version", version_string},
                                       {"revision", build_revision_string()},
                                       {"api", "v1"},
                                       {"store", ctx.store ? ctx.store->kind() : "none"}});
        }});
}

void register_identity_capabilities(Registry& registry) {
    // ---- users ----
    registry.add(Capability{
        .name = "list_users",
        .title = "List users",
        .description =
            "List user accounts, newest first, to find someone to invite or grant access.",
        .input_schema = list_input(),
        .output_schema = list_schema(user_schema()),
        .kind = CapabilityKind::Read,
        .object = "user collection",
        .action = "read",
        .resolve_object = global_object,
        .authorizes_candidates = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const ApiResult authorization = authorize_user_collection(ctx);
            if (!authorization.ok) {
                return authorization;
            }
            return list_result(ctx.store->list_users(limit_of(in)));
        }});
    registry.add(
        Capability{.name = "get_user",
                   .title = "Get user",
                   .description = "Read one user account: identity, email and display name.",
                   .input_schema = id_input(),
                   .output_schema = user_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "user",
                   .action = "read",
                   .resolve_object = global_object,
                   .handler = [](const json& in, ApiContext& ctx) {
                       return ctx.store ? one_result(ctx.store->get_user(in["id"])) : no_store();
                   }});
    registry.add(Capability{
        .name = "create_user",
        .title = "Create user",
        .description = "Create a user (identity, owns nothing). "
                       "taap_id and email are optional and unique.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"taap_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = user_schema(),
        .kind = CapabilityKind::Action,
        .object = "user collection",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store)
                return no_store();
            return one_result(ctx.store->create_user(control::NewUser{
                in.value("taap_id", ""), in.value("email", ""), in.value("display_name", "")}));
        }});

    // ---- organizations ----
    registry.add(Capability{
        .name = "list_organizations",
        .title = "List organizations",
        .description = "List the organizations you can see, newest first.",
        .input_schema = list_input(),
        .output_schema = list_schema(organization_schema()),
        .kind = CapabilityKind::Read,
        .object = "organization collection",
        .action = "read",
        .resolve_object = global_object,
        .authorizes_candidates = true,
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? authorized_entity_list_result(
                                   ctx.store->list_organizations(limit_of(in)), ctx, "organization")
                             : no_store();
        }});
    registry.add(Capability{
        .name = "get_organization",
        .title = "Get organization",
        .description = "Read one organization: its name, the space it owns and when it was made.",
        .input_schema = id_input(),
        .output_schema = organization_schema(),
        .kind = CapabilityKind::Read,
        .object = "organization",
        .action = "read",
        .resolve_object = entity_object("organization", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? one_result(ctx.store->get_organization(in["id"])) : no_store();
        }});
    registry.add(Capability{.name = "create_organization",
                            .title = "Create organization",
                            .description =
                                "Create an organization (identity, owns nothing). taap_id "
                                "is optional and unique.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"taap_id", {{"type", "string"}}},
                                               {"display_name", {{"type", "string"}}},
                                               {"customer_id", {{"type", "string"}}}}},
                                             {"additionalProperties", false}},
                            .output_schema = organization_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "customer",
                            .action = "admin",
                            .resolve_object = optional_entity_object("customer", "customer_id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                if (!ctx.store)
                                    return no_store();
                                return one_result(ctx.store->create_organization(
                                    control::NewOrganization{in.value("taap_id", ""),
                                                             in.value("display_name", ""),
                                                             in.value("customer_id", "")}));
                            }});

    // ---- customers ----
    registry.add(Capability{
        .name = "list_customers",
        .title = "List customers",
        .description = "List the spaces (customers) you can see, newest first; a space owns "
                       "repositories, workspaces and services.",
        .input_schema = list_input(),
        .output_schema = list_schema(customer_schema()),
        .kind = CapabilityKind::Read,
        .object = "customer collection",
        .action = "read",
        .resolve_object = global_object,
        .authorizes_candidates = true,
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? authorized_entity_list_result(
                                   ctx.store->list_customers(limit_of(in)), ctx, "customer")
                             : no_store();
        }});
    // One name for a space across every capability: customer_id. `id` was get_customer's own
    // spelling and stays as a deprecated alias (lake#443, lake#450).
    registry.add(Capability{
        .name = "get_customer",
        .title = "Get customer",
        .description = "Read one space (customer): its handle, whether it belongs to a person or "
                       "an organization, and when it was made. Name it by `customer_id`.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = customer_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = [](const json& in,
                             ApiContext& context) -> std::expected<std::string, ApiError> {
            auto object = context.store->find_policy_object("customer", in.at("customer_id"));
            if (!object) {
                return std::unexpected(
                    ApiError{object.error().code, object.error().message, "/customer_id"});
            }
            return object->id;
        },
        .handler =
            [](const json& in, ApiContext& ctx) {
                if (!ctx.store) {
                    return no_store();
                }
                auto result = one_result(ctx.store->get_customer(in.at("customer_id")));
                if (!result.ok && result.error.path.empty()) {
                    result.error.path = "/customer_id";
                }
                return result;
            },
        .input_aliases = {{"id", "customer_id"}}});
    registry.add(Capability{
        .name = "create_customer",
        .title = "Create customer",
        .description = "Create the tenant for a user (customer_type individual + "
                       "user_id) or an organization "
                       "(customer_type organization + organization_id). name is "
                       "the handle — one global lowercase "
                       "namespace; omitted, a {adjective}-{noun}-{number} handle "
                       "is generated. Conflicts answer 409.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"customer_type",
                {{"type", "string"}, {"enum", json::array({"individual", "organization"})}}},
               {"user_id", {{"type", "string"}}},
               {"organization_id", {{"type", "string"}}},
               {"name", {{"type", "string"}, {"description", "handle; generated when omitted"}}}}},
             {"required", json::array({"customer_type"})},
             {"additionalProperties", false}},
        .output_schema = customer_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer collection",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store)
                return no_store();
            control::NewCustomer draft{in.value("customer_type", ""),
                                       in.value("user_id", ""),
                                       in.value("organization_id", ""),
                                       in.value("name", "")};
            if (!draft.name.empty())
                return one_result(ctx.store->create_customer(draft));
            // Generated handle: retry on a collision, then give up honestly.
            for (int attempt = 0; attempt < 8; ++attempt) {
                draft.name = control::generate_handle();
                auto created = ctx.store->create_customer(draft);
                if (created || created.error().code != "conflict" ||
                    created.error().message.find("name") == std::string::npos) {
                    return one_result(std::move(created));
                }
            }
            return ApiResult::failure("conflict",
                                      "could not find a free generated handle; pass name");
        }});
    const json session_issue_schema = {{"type", "object"},
                                       {"properties",
                                        {{"session", session_schema()},
                                         {"secret", {{"type", "string"}}},
                                         {"access_token", {{"type", "string"}}},
                                         {"token_type", {{"type", "string"}}},
                                         {"expires_in", {{"type", "integer"}}}}}};
    registry.add(Capability{
        .name = "sign_up_password",
        .title = "Sign up with password",
        .description = "Create a pool-scoped user with an Argon2id password "
                       "credential and issue a browser session. In the lake pool, atomically "
                       "provision one personal customer with an inherited admin ownership grant, "
                       "a generated immutable handle, and the configured default storage quota. "
                       "Duplicate email or identity returns conflict; sign in to resume.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"password", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"identity_document", {{"type", "string"}}}}},
                         {"required", json::array({"email", "password", "identity_document"})},
                         {"additionalProperties", false}},
        .output_schema = session_issue_schema,
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store)
                co_return no_store();
            const std::string pool_id = input.value("pool_id", "lake");
            if (const ApiResult origin =
                    origin_answer(co_await authorize_application_origin(context, pool_id));
                !origin.ok) {
                co_return origin;
            }
            co_return session_answer(
                co_await sign_up_password_session({pool_id,
                                                   input.value("email", ""),
                                                   input.value("password", ""),
                                                   input.value("display_name", ""),
                                                   input.value("device_name", "browser"),
                                                   input.value("identity_document", "")},
                                                  context));
        }});
    registry.add(
        Capability{.name = "sign_in_password",
                   .title = "Sign in with password",
                   .description = "Verify a password credential. A user with enrolled MFA "
                                  "receives mfa_required until TOTP is verified.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"pool_id", {{"type", "string"}}},
                                      {"email", {{"type", "string"}}},
                                      {"password", {{"type", "string"}}},
                                      {"device_name", {{"type", "string"}}}}},
                                    {"required", json::array({"email", "password"})},
                                    {"additionalProperties", false}},
                   .output_schema = session_issue_schema,
                   .kind = CapabilityKind::Action,
                   .requires_auth = false,
                   .object = "authentication",
                   .action = "write",
                   .resolve_object = global_object,
                   .async_handler = [](const json& input,
                                       ApiContext& context) -> liquid::runtime::task<ApiResult> {
                       if (!context.store)
                           co_return no_store();
                       const std::string pool_id = input.value("pool_id", "lake");
                       if (const ApiResult origin = origin_answer(
                               co_await authorize_application_origin(context, pool_id));
                           !origin.ok) {
                           co_return origin;
                       }
                       co_return session_answer(co_await issued_result(
                           context.store->sign_in_password({pool_id,
                                                            input.value("email", ""),
                                                            input.value("password", ""),
                                                            "",
                                                            input.value("device_name", "browser")}),
                           context));
                   }});
    registry.add(Capability{
        .name = "sign_in_taap",
        .title = "Sign in with TAAP key",
        .description = "Verify a fresh liquid identity assertion and issue a lake session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"identity_document", {{"type", "string"}}},
                           {"nonce", {{"type", "string"}}},
                           {"issued_at", {{"type", "integer"}}},
                           {"signature", {{"type", "object"}}},
                           {"device_name", {{"type", "string"}}}}},
                         {"required",
                          json::array({"identity_document", "nonce", "issued_at", "signature"})},
                         {"additionalProperties", false}},
        .output_schema = session_issue_schema,
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const std::string pool_id = input.value("pool_id", "lake");
            if (const ApiResult origin =
                    origin_answer(co_await authorize_application_origin(context, pool_id));
                !origin.ok) {
                co_return origin;
            }
            auto verified = control::identity_crypto::verify_taap_sign_in(
                input.at("identity_document").get<std::string>(),
                pool_id,
                input.at("nonce").get<std::string>(),
                input.at("issued_at").get<std::int64_t>(),
                input.at("signature"));
            if (!verified) {
                co_return ApiResult::failure("unauthorized", verified.error());
            }
            context.authentication_method = "taap";
            context.audit_details = {{"taap_id", verified->taap_id},
                                     {"assertion_id", verified->assertion_id}};
            co_return session_answer(co_await issued_result(
                context.store->sign_in_taap({pool_id,
                                             verified->taap_id,
                                             verified->assertion_id,
                                             input.value("device_name", "taap-key")}),
                context));
        }});
    registry.add(
        Capability{.name = "verify_mfa",
                   .title = "Verify TOTP",
                   .description = "Verify an enrolled TOTP factor together with the "
                                  "password and issue a browser session.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"pool_id", {{"type", "string"}}},
                                      {"email", {{"type", "string"}}},
                                      {"password", {{"type", "string"}}},
                                      {"code", {{"type", "string"}}},
                                      {"device_name", {{"type", "string"}}}}},
                                    {"required", json::array({"email", "password", "code"})},
                                    {"additionalProperties", false}},
                   .output_schema = session_issue_schema,
                   .kind = CapabilityKind::Action,
                   .requires_auth = false,
                   .object = "authentication",
                   .action = "write",
                   .resolve_object = global_object,
                   .async_handler = [](const json& input,
                                       ApiContext& context) -> liquid::runtime::task<ApiResult> {
                       if (!context.store)
                           co_return no_store();
                       const std::string pool_id = input.value("pool_id", "lake");
                       if (const ApiResult origin = origin_answer(
                               co_await authorize_application_origin(context, pool_id));
                           !origin.ok) {
                           co_return origin;
                       }
                       co_return session_answer(co_await issued_result(
                           context.store->sign_in_totp({pool_id,
                                                        input.value("email", ""),
                                                        input.value("password", ""),
                                                        input.value("code", ""),
                                                        input.value("device_name", "browser")}),
                           context));
                   }});
    registry.add(Capability{
        .name = "enroll_mfa",
        .title = "Enroll TOTP",
        .description = "Enroll TOTP MFA and return its otpauth URI and recovery codes once.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"otpauth_uri", {{"type", "string"}}},
                            {"recovery_codes",
                             {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .async_handler = [](const json&, ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store)
                co_return no_store();
            auto result = context.store->enroll_mfa(context.acting_user_id, context.pool_id);
            if (!result)
                co_return from_store_error(result.error());
            const ApiResult event = session_answer(
                co_await emit_application_mfa_change(context, context.pool_id, "totp", true));
            if (!event.ok) {
                co_return event;
            }
            co_return ApiResult::success({{"otpauth_uri",
                                           "otpauth://totp/lake:" + context.acting_user_id +
                                               "?secret=" + result->secret + "&issuer=lake"},
                                          {"recovery_codes", result->recovery_codes}});
        }});
    registry.add(Capability{
        .name = "disable_mfa",
        .title = "Disable MFA",
        .description = "Disable TOTP MFA after a fresh TOTP or recovery-code verification.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"code", {{"type", "string"}}}}},
                         {"required", json::array({"code"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const auto result = context.store->disable_mfa(
                context.acting_user_id, context.pool_id, input.value("code", ""));
            if (!result) {
                co_return from_store_error(result.error());
            }
            const ApiResult event = session_answer(
                co_await emit_application_mfa_change(context, context.pool_id, "totp", false));
            co_return event.ok ? ApiResult::success({{"disabled", true}}) : event;
        }});
    registry.add(Capability{
        .name = "begin_passkey_registration",
        .title = "Begin passkey registration",
        .description = "Create five-minute WebAuthn registration options for "
                       "the authenticated user.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"pool_id", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (context.public_host.empty()) {
                co_return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for passkey registration");
            }
            const std::string pool_id =
                context.operator_credential ? input.value("pool_id", "lake") : context.pool_id;
            const auto user = context.store->get_user(context.acting_user_id);
            if (!user) {
                co_return from_store_error(user.error());
            }
            auto completed_party = co_await webauthn_relying_party(context, pool_id);
            if (!completed_party) {
                co_return ApiResult::failure("store_error", completed_party.error().message);
            }
            auto& relying_party = *completed_party;
            if (!relying_party) {
                co_return relying_party.error();
            }
            const auto challenge = context.store->begin_passkey_registration(
                context.acting_user_id, pool_id, relying_party->first, relying_party->second);
            if (!challenge) {
                co_return from_store_error(challenge.error());
            }
            co_return ApiResult::success(
                {{"public_key",
                  {{"challenge", challenge->challenge},
                   {"rp", {{"id", challenge->rp_id}, {"name", "lake"}}},
                   {"user",
                    {{"id", control::identity_crypto::base64url_encode(user->id)},
                     {"name", user->email.empty() ? user->id : user->email},
                     {"displayName", user->display_name.empty() ? user->id : user->display_name}}},
                   {"pubKeyCredParams", json::array({{{"type", "public-key"}, {"alg", -7}}})},
                   {"timeout", 300000},
                   {"attestation", "none"}}}});
        }});
    registry.add(Capability{
        .name = "register_passkey",
        .title = "Register passkey",
        .description = "Record an attestation-less WebAuthn credential and a lake-signed TAAP "
                       "identity binding for the authenticated user.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"challenge", {{"type", "string"}}},
                           {"client_data_json", {{"type", "string"}}},
                           {"attestation_object", {{"type", "string"}}},
                           {"name", {{"type", "string"}}}}},
                         {"required",
                          json::array({"challenge", "client_data_json", "attestation_object"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store)
                co_return no_store();
            const std::string pool_id =
                context.operator_credential ? input.value("pool_id", "lake") : context.pool_id;
            auto result = context.store->register_passkey({context.acting_user_id,
                                                           pool_id,
                                                           input.value("challenge", ""),
                                                           input.value("client_data_json", ""),
                                                           input.value("attestation_object", ""),
                                                           input.value("name", "passkey"),
                                                           identity_options(context)});
            if (!result) {
                co_return from_store_error(result.error());
            }
            const ApiResult event = session_answer(
                co_await emit_application_mfa_change(context, pool_id, "passkey", true));
            co_return event.ok ? ApiResult::success({{"registered", true}}) : event;
        }});
    registry.add(Capability{
        .name = "begin_passkey_sign_in",
        .title = "Begin passkey sign-in",
        .description = "Create five-minute WebAuthn assertion options for a user pool.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"pool_id", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (context.public_host.empty()) {
                co_return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for passkey sign-in");
            }
            const std::string pool_id = input.value("pool_id", "lake");
            if (const ApiResult origin =
                    origin_answer(co_await authorize_application_origin(context, pool_id));
                !origin.ok) {
                co_return origin;
            }
            auto completed_party = co_await webauthn_relying_party(context, pool_id);
            if (!completed_party) {
                co_return ApiResult::failure("store_error", completed_party.error().message);
            }
            auto& relying_party = *completed_party;
            if (!relying_party) {
                co_return relying_party.error();
            }
            const auto challenge = context.store->begin_passkey_sign_in(
                pool_id, relying_party->first, relying_party->second);
            if (!challenge) {
                co_return from_store_error(challenge.error());
            }
            co_return ApiResult::success({{"public_key",
                                        {{"challenge", challenge->challenge},
                                         {"rpId", challenge->rp_id},
                                         {"timeout", 300000},
                                         {"userVerification", "preferred"}}}});
        }});
    registry.add(Capability{
        .name = "sign_in_passkey",
        .title = "Sign in with passkey",
        .description = "Complete a verified WebAuthn assertion and issue a browser session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"challenge", {{"type", "string"}}},
                           {"credential_id", {{"type", "string"}}},
                           {"client_data_json", {{"type", "string"}}},
                           {"authenticator_data", {{"type", "string"}}},
                           {"signature", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}}}},
                         {"required",
                          json::array({"challenge",
                                       "credential_id",
                                       "client_data_json",
                                       "authenticator_data",
                                       "signature"})},
                         {"additionalProperties", false}},
        .output_schema = session_issue_schema,
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const std::string pool_id = input.value("pool_id", "lake");
            if (const ApiResult origin =
                    origin_answer(co_await authorize_application_origin(context, pool_id));
                !origin.ok) {
                co_return origin;
            }
            co_return session_answer(co_await issued_result(
                context.store->sign_in_passkey({pool_id,
                                                input.value("challenge", ""),
                                                input.value("credential_id", ""),
                                                input.value("client_data_json", ""),
                                                input.value("authenticator_data", ""),
                                                input.value("signature", ""),
                                                input.value("device_name", "browser")}),
                context));
        }});
    registry.add(Capability{
        .name = "register_oauth_provider",
        .title = "Register OAuth provider",
        .description = "Register a pool-scoped OAuth or OpenID Connect provider.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"provider", {{"type", "string"}}},
                           {"issuer", {{"type", "string"}}},
                           {"client_id", {{"type", "string"}}},
                           {"client_secret", {{"type", "string"}}},
                           {"scopes", {{"type", "array"}}}}},
                         {"required",
                          json::array(
                              {"provider", "issuer", "client_id", "client_secret", "scopes"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"registered", {{"type", "boolean"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "OAuth provider registry",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const auto result = context.store->register_oauth_provider(
                {input.value("pool_id", "lake"),
                 input.value("provider", ""),
                 input.value("issuer", ""),
                 input.value("client_id", ""),
                 input.value("client_secret", ""),
                 input.value("scopes", std::vector<std::string>{})});
            return result ? ApiResult::success({{"registered", true}})
                          : from_store_error(result.error());
        }});
    registry.add(Capability{
        .name = "start_oauth",
        .title = "Start OAuth sign-in",
        .description = "Create a one-use OAuth authorization flow with PKCE and state.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"pool_id", {{"type", "string"}}},
                           {"provider", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"identity_document", {{"type", "string"}}}}},
                         {"required", json::array({"provider"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties", {{"authorization_url", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (context.public_host.empty()) {
                co_return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for OAuth sign-in");
            }
            const std::string pool_id = input.value("pool_id", "lake");
            if (const ApiResult origin =
                    origin_answer(co_await authorize_application_origin(context, pool_id));
                !origin.ok) {
                co_return origin;
            }
            const std::string provider_name = input.value("provider", "");
            const auto provider = context.store->get_oauth_provider(pool_id, provider_name);
            if (!provider) {
                co_return from_store_error(provider.error());
            }
            if (!oauth_endpoints_allowed(oauth_endpoints(*provider))) {
                co_return ApiResult::failure("invalid_input",
                                          "OAuth endpoints must use HTTPS outside loopback tests");
            }
            control::OAuthFlow flow{pool_id,
                                    provider_name,
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    input.value("device_name", "browser"),
                                    input.value("identity_document", "")};
            if (flow.state.empty() || flow.code_verifier.empty() || flow.nonce.empty()) {
                co_return ApiResult::failure("store_error", "could not generate OAuth flow material");
            }
            const auto started = context.store->begin_oauth(flow);
            if (!started) {
                co_return from_store_error(started.error());
            }
            co_return ApiResult::success(
                {{"authorization_url",
                  oauth_authorization_url(*provider, flow, context.public_host)}});
        }});
    registry.add(Capability{
        .name = "start_sso_oidc",
        .title = "Start enterprise OIDC sign-in",
        .description = "Create a one-use enterprise OIDC authorization-code flow with PKCE.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}}}},
                         {"required", json::array({"provider_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            if (context.public_host.empty()) {
                return ApiResult::failure("store_unavailable",
                                          "LAKE_PUBLIC_HOST is required for OIDC sign-in");
            }
            const std::string provider_id = input.at("provider_id");
            const auto provider = context.store->get_sso_oidc_provider(provider_id);
            if (!provider)
                return from_store_error(provider.error());
            if (!oauth_endpoints_allowed(oauth_endpoints(*provider))) {
                return ApiResult::failure("invalid_input",
                                          "OIDC endpoints must use HTTPS outside loopback tests");
            }
            control::OAuthFlow flow{provider->pool_id,
                                    provider->provider,
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    control::identity_crypto::random_secret(),
                                    input.value("device_name", "browser"),
                                    input.value("taap_id", "")};
            if (flow.state.empty() || flow.code_verifier.empty() || flow.nonce.empty()) {
                return ApiResult::failure("store_error", "could not generate OIDC flow material");
            }
            const auto started = context.store->begin_oauth(flow);
            if (!started)
                return from_store_error(started.error());
            return ApiResult::success(
                {{"authorization_url",
                  oauth_authorization_url(*provider, flow, context.public_host)}});
        }});
    registry.add(Capability{
        .name = "create_token",
        .title = "Create API token",
        .description = "Make an API key you can give a script, an agent or an MCP client. Name "
                       "what it may call in `scopes`: capability names, `*` for every "
                       "capability, or `registry_pull` to pull images; an unknown scope is "
                       "refused. It can never do more than the credential that makes it, it "
                       "expires after `expires_in_seconds` (30 days by default), and revoking "
                       "that credential revokes it too. A static key (`static`) needs a `reason`.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"name", {{"type", "string"}}},
                           {"scopes",
                            {{"type", "array"},
                             {"items", {{"type", "string"}}},
                             {"minItems", 1},
                             {"description",
                              "what the key may call: capability names, * for every capability, "
                              "or registry_pull to pull images (docs/api/auth.md#scopes)"}}},
                           {"expires_in_seconds", {{"type", "integer"}}},
                           {"static", {{"type", "boolean"}}},
                           {"never_expires",
                            {{"type", "boolean"},
                             {"description",
                              "a standing token with no expiry that survives sign-out; needs a "
                              "reason and named scopes, and the space's opt-in (lake#234)"}}},
                           {"reason", {{"type", "string"}}}}},
                         {"required", json::array({"name", "scopes"})},
                         {"additionalProperties", false}},
        .output_schema = session_issue_schema,
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            const auto token = attenuated_token(input, context);
            if (!token) {
                co_return ApiResult::failure(
                    token.error().code, token.error().message, token.error().path);
            }
            co_return session_answer(
                co_await issued_result(context.store->create_token(*token), context));
        }});
    registry.add(Capability{.name = "list_sessions",
                            .title = "List sessions",
                            .description = "List the authenticated user's sessions and tokens.",
                            .input_schema = {{"type", "object"},
                                             {"properties", json::object()},
                                             {"additionalProperties", false}},
                            .output_schema = list_schema(session_schema()),
                            .kind = CapabilityKind::Read,
                            .object = "principal",
                            .action = "read",
                            .resolve_object = self_object,
                            .authorizes_candidates = true,
                            .handler = [](const json&, ApiContext& context) {
                                const auto authorization = authorize_scope(
                                    context, "principal/" + context.acting_identity, "read");
                                if (!authorization) {
                                    return authorization_failure(authorization);
                                }
                                return list_result(context.store->list_sessions(
                                    context.acting_user_id, context.pool_id));
                            }});
    registry.add(Capability{
        .name = "revoke_session",
        .title = "Revoke session",
        .description = "Revoke one of the authenticated user's sessions or tokens.",
        .input_schema = id_input(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            auto result = context.store->revoke_session(context.acting_user_id, input["id"]);
            return result ? ApiResult::success({{"revoked", true}})
                          : from_store_error(result.error());
        }});
    registry.add(Capability{
        .name = "revoke_user_sessions",
        .title = "Revoke a user's sessions",
        .description = "Operator action: revoke every live session and token of one user, for "
                       "example after removing them from the identity provider. Derived tokens "
                       "stop with their parent. The user can sign in again unless their sign-in "
                       "is also removed.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"user_id", {{"type", "string"}}}}},
                         {"required", json::array({"user_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "user collection",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            const std::string user_id = input.at("user_id");
            int revoked = 0;
            for (const std::string pool : {"lake"}) {
                auto sessions = context.store->list_sessions(user_id, pool);
                if (!sessions)
                    return from_store_error(sessions.error());
                for (const auto& session : *sessions) {
                    if (!session.revoked_at.empty())
                        continue;
                    if (auto result = context.store->revoke_session(user_id, session.id)) {
                        ++revoked;
                    } else if (result.error().code != "not_found") {
                        return from_store_error(result.error());
                    }
                }
            }
            context.audit_details = {{"user_id", user_id}, {"revoked", revoked}};
            return ApiResult::success({{"user_id", user_id}, {"revoked", revoked}});
        }});
    registry.add(
        Capability{.name = "list_devices",
                   .title = "List devices",
                   .description = "List the authenticated user's devices and signed "
                                  "passkey bindings.",
                   .input_schema = {{"type", "object"},
                                    {"properties", json::object()},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(device_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "principal",
                   .action = "read",
                   .resolve_object = self_object,
                   .authorizes_candidates = true,
                   .handler = [](const json&, ApiContext& context) {
                       const auto authorization =
                           authorize_scope(context, "principal/" + context.acting_identity, "read");
                       if (!authorization) {
                           return authorization_failure(authorization);
                       }
                       return list_result(context.store->list_devices(context.acting_user_id));
                   }});
    registry.add(Capability{
        .name = "revoke_device",
        .title = "Revoke device",
        .description = "Revoke a device; attached sessions fail on their next request.",
        .input_schema = id_input(),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store)
                return no_store();
            auto result = context.store->revoke_device(context.acting_user_id, input["id"]);
            return result ? ApiResult::success({{"revoked", true}})
                          : from_store_error(result.error());
        }});
}

void register_sso_capabilities(Registry& registry) {
    const json provider_input = {
        {"type", "object"},
        {"properties",
         {{"organization_id", {{"type", "string"}}},
          {"pool_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"protocol", {{"type", "string"}}},
          {"issuer", {{"type", "string"}}},
          {"metadata", {{"type", "string"}}},
          {"signing_certificate", {{"type", "string"}}},
          {"client_id", {{"type", "string"}}},
          {"client_secret", {{"type", "string"}}},
          {"scim_token", {{"type", "string"}}},
          {"attribute_mapping", {{"type", "object"}}},
          {"jit_provisioning", {{"type", "boolean"}}}}},
        {"required", json::array({"organization_id", "name", "protocol", "issuer", "scim_token"})},
        {"additionalProperties", false},
    };
    registry.add(Capability{
        .name = "register_sso_provider",
        .title = "Register SSO provider",
        .description = "Register or update an organization SAML or OpenID Connect provider.",
        .input_schema = provider_input,
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            return one_result(context.store->register_sso_provider(
                {input.value("organization_id", ""),
                 input.value("pool_id", "lake"),
                 input.value("name", ""),
                 input.value("protocol", ""),
                 input.value("issuer", ""),
                 input.value("metadata", ""),
                 input.value("signing_certificate", ""),
                 input.value("client_id", ""),
                 input.value("client_secret", ""),
                 input.value("scim_token", ""),
                 input.value("attribute_mapping", json::object()),
                 input.value("jit_provisioning", false)}));
        }});
    registry.add(Capability{
        .name = "list_sso_providers",
        .title = "List SSO providers",
        .description = "List an organization's SSO provider configurations without secrets.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"organization_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            const auto authorization = authorize_entity_scope(
                context, "organization", input.at("organization_id"), "admin");
            if (!authorization) {
                return authorization_failure(authorization);
            }
            return list_result(
                context.store->list_sso_providers(input.at("organization_id"), limit_of(input)));
        }});
    registry.add(Capability{
        .name = "test_sso_provider",
        .title = "Test SSO provider",
        .description = "Validate that a provider has the configuration required by its protocol.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"provider_id", {{"type", "string"}}}}},
                         {"required", json::array({"provider_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = sso_provider_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->test_sso_provider(input.at("provider_id")))
                       : no_store();
        }});
    registry.add(Capability{
        .name = "create_federation_issuer",
        .title = "Allow federation issuer claim",
        .description = "Allow an issuer to assert one named claim for an organization.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"issuer", {{"type", "string"}}},
                           {"claim", {{"type", "string"}}}}},
                         {"required", json::array({"organization_id", "issuer", "claim"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->create_federation_issuer(
                             {input.at("organization_id"), input.at("issuer"), input.at("claim")}))
                       : no_store();
        }});
    registry.add(
        Capability{.name = "list_federation_issuers",
                   .title = "List federation issuer policy",
                   .description = "List the issuer and claim pairs accepted by an organization.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"organization_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"required", json::array({"organization_id"})},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Read,
                   .object = "organization",
                   .action = "admin",
                   .resolve_object = entity_object("organization", "organization_id"),
                   .authorizes_candidates = true,
                   .handler = [](const json& input, ApiContext& context) {
                       const auto authorization = authorize_entity_scope(
                           context, "organization", input.at("organization_id"), "admin");
                       if (!authorization) {
                           return authorization_failure(authorization);
                       }
                       return list_result(context.store->list_federation_issuers(
                           input.at("organization_id"), limit_of(input)));
                   }});
    registry.add(Capability{
        .name = "sign_in_sso",
        .title = "Sign in with SSO assertion",
        .description = "Issue a session after provider verification and federation issuer policy.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"issuer", {{"type", "string"}}},
                           {"subject", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}},
                           {"claims", {{"type", "object"}}}}},
                         {"required", json::array({"provider_id", "issuer", "subject", "claims"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            co_return session_answer(co_await issued_result(
                context.store->sign_in_sso({input.value("provider_id", ""),
                                            input.value("issuer", ""),
                                            input.value("subject", ""),
                                            input.value("email", ""),
                                            input.value("display_name", ""),
                                            input.value("taap_id", ""),
                                            input.value("device_name", "browser"),
                                            input.value("claims", json::object())}),
                context));
        }});
    registry.add(Capability{
        .name = "sign_in_saml",
        .title = "Sign in with signed SAML assertion",
        .description = "Verify the configured SAML signing certificate before issuing a session.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"saml_response", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"device_name", {{"type", "string"}}}}},
                         {"required", json::array({"provider_id", "saml_response"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            auto provider = context.store->get_sso_provider(input.at("provider_id"));
            if (!provider) {
                co_return from_store_error(provider.error());
            }
            if (provider->protocol != "saml") {
                co_return ApiResult::failure("invalid_input", "provider is not a SAML provider");
            }
            auto assertion =
                control::saml::verify_response(input.at("saml_response").get<std::string>(),
                                               provider->signing_certificate,
                                               provider->issuer);
            if (!assertion) {
                co_return ApiResult::failure("unauthorized", assertion.error());
            }
            co_return session_answer(co_await issued_result(
                context.store->sign_in_sso({provider->id,
                                            assertion->issuer,
                                            assertion->subject,
                                            assertion->email,
                                            assertion->display_name,
                                            input.value("taap_id", ""),
                                            input.value("device_name", "browser"),
                                            assertion->claims}),
                context));
        }});
    registry.add(Capability{
        .name = "upsert_scim_user",
        .title = "Provision SCIM user",
        .description =
            "Create, update, or deactivate a SCIM user and revoke sessions on deactivation.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"email", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"taap_id", {{"type", "string"}}},
                           {"active", {{"type", "boolean"}}},
                           {"attributes", {{"type", "object"}}}}},
                         {"required",
                          json::array({"provider_id", "external_id", "taap_id", "active"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = sso_provider_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store ? one_result(context.store->upsert_scim_user(
                                       {input.value("provider_id", ""),
                                        input.value("external_id", ""),
                                        input.value("email", ""),
                                        input.value("display_name", ""),
                                        input.value("taap_id", ""),
                                        input.value("active", true),
                                        input.value("attributes", json::object())}))
                                 : no_store();
        }});
    registry.add(Capability{
        .name = "scim_upsert_user",
        .title = "SCIM user resource",
        .description = "Authenticate a provider bearer token and create a SCIM user resource.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource", {{"type", "object"}}}}},
                         {"required", json::array({"bearer_token", "resource"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            auto user = new_scim_user(provider->id, input.at("resource"));
            if (!user) {
                return user.error();
            }
            const auto created = context.store->upsert_scim_user(*user);
            return created ? ApiResult::success(scim_user_document(*created))
                           : from_store_error(created.error());
        }});
    registry.add(Capability{
        .name = "scim_list_users",
        .title = "List SCIM users",
        .description = "Authenticate a provider bearer token and filter its SCIM users.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"filter", {{"type", "string"}}},
                           {"start_index", {{"type", "integer"}}},
                           {"count", {{"type", "integer"}}}}},
                         {"required", json::array({"bearer_token"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto users = context.store->list_scim_users(provider->id, 1000);
            if (!users) {
                return from_store_error(users.error());
            }
            std::vector<json> resources;
            for (const control::ScimUser& user : *users) {
                json resource = scim_user_document(user);
                const auto matches =
                    scim_filter_matches(resource, input.value("filter", std::string()));
                if (!matches) {
                    return ApiResult::failure("invalid_input", matches.error());
                }
                if (*matches) {
                    resources.push_back(std::move(resource));
                }
            }
            return scim_list_response(
                std::move(resources), input.value("start_index", 1), input.value("count", 100));
        }});
    registry.add(Capability{
        .name = "scim_get_user",
        .title = "Get SCIM user",
        .description = "Authenticate a provider bearer token and read one SCIM user.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource_id", {{"type", "string"}}}}},
                         {"required", json::array({"bearer_token", "resource_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto user = context.store->get_scim_user(provider->id, input.at("resource_id"));
            return user ? ApiResult::success(scim_user_document(*user))
                        : from_store_error(user.error());
        }});
    registry.add(Capability{
        .name = "scim_patch_user",
        .title = "Patch SCIM user",
        .description = "Apply SCIM PATCH operations and revoke sessions when active becomes false.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource_id", {{"type", "string"}}},
                           {"patch", {{"type", "object"}}}}},
                         {"required", json::array({"bearer_token", "resource_id", "patch"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto existing =
                context.store->get_scim_user(provider->id, input.at("resource_id"));
            if (!existing) {
                return from_store_error(existing.error());
            }
            auto patched = apply_scim_patch(scim_user_document(*existing), input.at("patch"));
            if (!patched) {
                return ApiResult::failure("invalid_input", patched.error());
            }
            (*patched)["externalId"] = existing->external_id;
            auto user = new_scim_user(provider->id, std::move(*patched));
            if (!user) {
                return user.error();
            }
            const auto updated = context.store->upsert_scim_user(*user);
            return updated ? ApiResult::success(scim_user_document(*updated))
                           : from_store_error(updated.error());
        }});
    registry.add(Capability{
        .name = "scim_delete_user",
        .title = "Delete SCIM user",
        .description = "Deactivate a SCIM user, remove team access, and revoke active sessions.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource_id", {{"type", "string"}}}}},
                         {"required", json::array({"bearer_token", "resource_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto user =
                context.store->delete_scim_user(provider->id, input.at("resource_id"));
            return user ? ApiResult::success({{"deleted", true}}) : from_store_error(user.error());
        }});
    registry.add(Capability{
        .name = "upsert_scim_group",
        .title = "Provision SCIM group",
        .description = "Create or update a SCIM group and map its users to a lake team.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"provider_id", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"team_name", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}},
                           {"member_external_ids", {{"type", "array"}}},
                           {"attributes", {{"type", "object"}}}}},
                         {"required",
                          json::array({"provider_id",
                                       "external_id",
                                       "team_name",
                                       "display_name",
                                       "member_external_ids"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = sso_provider_object,
        .handler = [](const json& input, ApiContext& context) {
            return context.store
                       ? one_result(context.store->upsert_scim_group(
                             {input.value("provider_id", ""),
                              input.value("external_id", ""),
                              input.value("team_name", ""),
                              input.value("display_name", ""),
                              input.value("member_external_ids", std::vector<std::string>{}),
                              input.value("attributes", json::object())}))
                       : no_store();
        }});
    registry.add(Capability{
        .name = "scim_upsert_group",
        .title = "SCIM group resource",
        .description = "Authenticate a provider bearer token and create a SCIM group resource.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource", {{"type", "object"}}}}},
                         {"required", json::array({"bearer_token", "resource"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            auto group = new_scim_group(*context.store, provider->id, input.at("resource"));
            if (!group) {
                return group.error();
            }
            const auto created = context.store->upsert_scim_group(*group);
            return created ? ApiResult::success(scim_group_document(*created))
                           : from_store_error(created.error());
        }});
    registry.add(Capability{
        .name = "scim_list_groups",
        .title = "List SCIM groups",
        .description = "Authenticate a provider bearer token and filter its SCIM groups.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"filter", {{"type", "string"}}},
                           {"start_index", {{"type", "integer"}}},
                           {"count", {{"type", "integer"}}}}},
                         {"required", json::array({"bearer_token"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto groups = context.store->list_scim_groups(provider->id, 1000);
            if (!groups) {
                return from_store_error(groups.error());
            }
            std::vector<json> resources;
            for (const control::ScimGroup& group : *groups) {
                json resource = scim_group_document(group);
                const auto matches =
                    scim_filter_matches(resource, input.value("filter", std::string()));
                if (!matches) {
                    return ApiResult::failure("invalid_input", matches.error());
                }
                if (*matches) {
                    resources.push_back(std::move(resource));
                }
            }
            return scim_list_response(
                std::move(resources), input.value("start_index", 1), input.value("count", 100));
        }});
    registry.add(Capability{
        .name = "scim_get_group",
        .title = "Get SCIM group",
        .description = "Authenticate a provider bearer token and read one SCIM group.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource_id", {{"type", "string"}}}}},
                         {"required", json::array({"bearer_token", "resource_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto group = context.store->get_scim_group(provider->id, input.at("resource_id"));
            return group ? ApiResult::success(scim_group_document(*group))
                         : from_store_error(group.error());
        }});
    registry.add(Capability{
        .name = "scim_patch_group",
        .title = "Patch SCIM group",
        .description = "Apply SCIM PATCH operations and replace the mapped team membership.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"bearer_token", {{"type", "string"}}},
                           {"resource_id", {{"type", "string"}}},
                           {"patch", {{"type", "object"}}}}},
                         {"required", json::array({"bearer_token", "resource_id", "patch"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .requires_auth = false,
        .object = "authentication",
        .action = "write",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            const auto provider = scim_provider(input, context);
            if (!provider) {
                return provider.error();
            }
            const auto existing =
                context.store->get_scim_group(provider->id, input.at("resource_id"));
            if (!existing) {
                return from_store_error(existing.error());
            }
            auto patched = apply_scim_patch(scim_group_document(*existing), input.at("patch"));
            if (!patched) {
                return ApiResult::failure("invalid_input", patched.error());
            }
            (*patched)["externalId"] = existing->external_id;
            auto group = new_scim_group(*context.store, provider->id, std::move(*patched));
            if (!group) {
                return group.error();
            }
            const auto updated = context.store->upsert_scim_group(*group);
            return updated ? ApiResult::success(scim_group_document(*updated))
                           : from_store_error(updated.error());
        }});
    registry.add(
        Capability{.name = "scim_delete_group",
                   .title = "Delete SCIM group",
                   .description = "Delete a SCIM group and its source-managed team access.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"bearer_token", {{"type", "string"}}},
                                      {"resource_id", {{"type", "string"}}}}},
                                    {"required", json::array({"bearer_token", "resource_id"})},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Action,
                   .requires_auth = false,
                   .object = "authentication",
                   .action = "write",
                   .resolve_object = global_object,
                   .handler = [](const json& input, ApiContext& context) {
                       const auto provider = scim_provider(input, context);
                       if (!provider) {
                           return provider.error();
                       }
                       const auto deleted =
                           context.store->delete_scim_group(provider->id, input.at("resource_id"));
                       return deleted ? ApiResult::success({{"deleted", true}})
                                      : from_store_error(deleted.error());
                   }});
    registry.add(Capability{.name = "list_sso_provisioning_logs",
                            .title = "List SSO provisioning logs",
                            .description = "List SCIM provisioning outcomes for an SSO provider.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"provider_id", {{"type", "string"}}},
                                               {"limit", {{"type", "integer"}}}}},
                                             {"required", json::array({"provider_id"})},
                                             {"additionalProperties", false}},
                            .output_schema = {{"type", "object"}},
                            .kind = CapabilityKind::Read,
                            .object = "organization",
                            .action = "admin",
                            .resolve_object = sso_provider_object,
                            .authorizes_candidates = true,
                            .handler = [](const json& input, ApiContext& context) {
                                if (!context.store) {
                                    return no_store();
                                }
                                const auto provider =
                                    context.store->get_sso_provider(input.at("provider_id"));
                                if (!provider) {
                                    return from_store_error(provider.error());
                                }
                                const auto authorization = authorize_entity_scope(
                                    context, "organization", provider->organization_id, "admin");
                                if (!authorization) {
                                    return authorization_failure(authorization);
                                }
                                return list_result(context.store->list_sso_provisioning_logs(
                                    provider->id, limit_of(input)));
                            }});
}

void register_repository_capabilities(Registry& registry) {
    // ---- repositories ----
    registry.add(
        Capability{.name = "list_repositories",
                   .title = "List repositories",
                   .description = "The most recent repositories, newest first; customer_id "
                                  "narrows to one space and workspace_id to one of its "
                                  "workspaces.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"customer_id", {{"type", "string"}}},
                                      {"workspace_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(repository_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "repository collection",
                   .action = "read",
                   .resolve_object = optional_entity_object("customer", "customer_id"),
                   .authorizes_candidates = true,
                   .allows_anonymous_public_read = true,
                   // Registry::call answers the routing rows alone (lake's own page routing);
                   // REST, MCP and call_async also read each workspace's descriptions.
                   .handler = [](const json& in, ApiContext& ctx) {
                       return ctx.store ? authorized_repository_list_result(
                                              ctx.store->list_repositories(
                                                  in.value("customer_id", ""),
                                                  limit_of(in),
                                                  in.value("workspace_id", "")),
                                              ctx)
                                        : no_store();
                   },
                   .async_handler = [](const json& in,
                                       ApiContext& ctx) -> liquid::runtime::task<ApiResult> {
                       if (!ctx.store) {
                           co_return no_store();
                       }
                       auto repositories = authorized_repositories(
                           ctx.store->list_repositories(in.value("customer_id", ""),
                                                        limit_of(in),
                                                        in.value("workspace_id", "")),
                           ctx);
                       if (!repositories) {
                           co_return repositories.error();
                       }
                       // One read per workspace, all started together (lake#315).
                       const auto unavailable = settled_fill(
                           co_await fill_repository_settings(ctx, *repositories), *repositories);
                       json items = json::array();
                       for (const control::Repository& repository : *repositories) {
                           items.push_back(repository_json(repository, unavailable));
                       }
                       co_return ApiResult::success({{"items", std::move(items)}});
                   }});
    registry.add(Capability{.name = "get_repository",
                            .title = "Get repository",
                            .description = "Read one repository: its name, space, workspace, "
                                           "visibility, description (from its workspace) and "
                                           "whether it is deleted.",
                            .input_schema = id_input(),
                            .output_schema = repository_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "repository",
                            .action = "read",
                            .resolve_object = entity_object("repository", "id"),
                            .allows_anonymous_public_read = true,
                            // Registry::call answers the routing row alone (lake's own
                            // page routing); REST, MCP and call_async also read the
                            // description from the repository's workspace (lake#315).
                            .handler = [](const json& in, ApiContext& ctx) {
                                const auto repository = readable_repository(in, ctx);
                                if (!repository) {
                                    return ApiResult::failure(repository.error().code,
                                                              repository.error().message,
                                                              repository.error().path);
                                }
                                return ApiResult::success(control::to_json(*repository));
                            },
                            .async_handler = [](const json& in, ApiContext& ctx)
                                -> liquid::runtime::task<ApiResult> {
                                const auto repository = readable_repository(in, ctx);
                                if (!repository) {
                                    co_return ApiResult::failure(repository.error().code,
                                                                 repository.error().message,
                                                                 repository.error().path);
                                }
                                std::vector<control::Repository> one{*repository};
                                const auto unavailable = settled_fill(
                                    co_await fill_repository_settings(ctx, one), one);
                                co_return ApiResult::success(
                                    repository_json(one.front(), unavailable));
                            }});
    registry.add(Capability{
        .name = "create_repository",
        .title = "Create repository",
        .description = "Create a repository owned by a customer, in one of its workspaces "
                       "(`workspace_id`, required, never changes): name is a slug "
                       "unique within that customer "
                       "(the address is <customer handle>/<name>); visibility "
                       "private (default), organization, or public.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}},
                           {"description", {{"type", "string"}}},
                           {"visibility",
                            {{"type", "string"},
                             {"enum", json::array({"private", "organization", "public"})}}}}},
                         {"required", json::array({"customer_id", "workspace_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = entity_object("customer", "customer_id"),
        // A description is the repository's settings record in its workspace (lake#315),
        // written in the same request: only the asynchronous call (REST, MCP, call_async) can.
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store)
                return no_store();
            if (!in.value("description", "").empty()) {
                return ApiResult::failure("invalid_input",
                                          "a description is written to the repository's "
                                          "workspace; create it through the asynchronous call",
                                          "/description");
            }
            const auto workspace = repository_workspace(ctx, in);
            if (!workspace) {
                return ApiResult::failure(
                    workspace.error().code, workspace.error().message, workspace.error().path);
            }
            const std::string name = in.value("name", "");
            auto repository = create_repository_row(ctx, control::NewRepository{
                in.value("customer_id", ""), name, "", in.value("visibility", ""), workspace->id});
            if (!repository && repository.error().code == "conflict") {
                ApiResult result = from_store_error(repository.error());
                result.error.message = "a repository named '" + name + "' already exists";
                result.error.path = "/name";
                return result;
            }
            return one_result(std::move(repository));
        },
        .async_handler = [](const json& in, ApiContext& ctx) -> liquid::runtime::task<ApiResult> {
            if (!ctx.store) {
                co_return no_store();
            }
            const auto workspace = repository_workspace(ctx, in);
            if (!workspace) {
                co_return ApiResult::failure(
                    workspace.error().code, workspace.error().message, workspace.error().path);
            }
            const std::string name = in.value("name", "");
            auto created = settled_create(co_await create_repository_with_settings(
                ctx,
                control::NewRepository{
                    in.value("customer_id", ""), name, "", in.value("visibility", ""),
                    workspace->id},
                in.value("description", "")));
            if (!created) {
                ApiResult result = created.error();
                if (result.error.code == "conflict") {
                    result.error.message = "a repository named '" + name + "' already exists";
                    result.error.path = "/name";
                }
                co_return result;
            }
            co_return ApiResult::success(control::to_json(*created));
        }});
    registry.add(Capability{
        .name = "rename_repository",
        .title = "Rename repository",
        .description = "Rename a live repository within its customer.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
                         {"required", json::array({"id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? one_result(ctx.store->rename_repository(in.at("id"), in.at("name")))
                             : no_store();
        }});
    registry.add(Capability{
        .name = "transfer_repository",
        .title = "Transfer repository",
        .description = "Refused: a repository stays in the space and workspace it was created "
                       "in (lake#315). Export it and import it into the other space's workspace "
                       "instead.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"customer_id", {{"type", "string"}}}}},
                         {"required", json::array({"id", "customer_id"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) {
                return no_store();
            }
            const auto repository = ctx.store->get_repository(in.at("id"));
            if (!repository) {
                return from_store_error(repository.error());
            }
            // The routing trigger of migration 0051 holds the space and workspace fixed.
            return ApiResult::failure("conflict",
                                      "a repository stays in the space and workspace it was "
                                      "created in; export it and import it into a workspace of "
                                      "the other space",
                                      "/customer_id");
        }});
    registry.add(Capability{
        .name = "set_repository_visibility",
        .title = "Set repository visibility",
        .description = "Set a live repository to private, organization, or public.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}},
                           {"visibility",
                            {{"type", "string"},
                             {"enum", json::array({"private", "organization", "public"})}}}}},
                         {"required", json::array({"id", "visibility"})},
                         {"additionalProperties", false}},
        .output_schema = repository_schema(),
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return ctx.store ? one_result(ctx.store->set_repository_visibility(in.at("id"),
                                                                               in.at("visibility")))
                             : no_store();
        }});
    registry.add(Capability{.name = "delete_repository",
                            .title = "Delete repository",
                            .description = "Tombstone a repository for 30 days before collection.",
                            .input_schema = id_input(),
                            .output_schema = repository_schema(),
                            .kind = CapabilityKind::Action,
                            .object = "repository",
                            .action = "write",
                            .resolve_object = entity_object("repository", "id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                if (!ctx.store) {
                                    return no_store();
                                }
                                ctx.audit_object = "repository/" + in.at("id").get<std::string>();
                                const control::RepositoryDeletion deletion{
                                    .deleted_at = timestamp_after(std::chrono::seconds::zero()),
                                    .retention_until = timestamp_after(std::chrono::days{30}),
                                };
                                return one_result(
                                    ctx.store->delete_repository(in.at("id"), deletion));
                            }});
    const json sync_repository_input = {
        {"type", "object"},
        {"properties", {{"repository_id", {{"type", "string"}}}}},
        {"required", json::array({"repository_id"})},
        {"additionalProperties", false},
    };
    const json sync_output = {
        {"type", "object"},
        {"properties", {{"bytes", {{"type", "integer"}}}}},
    };
    registry.add(Capability{
        .name = "repository_refs",
        .title = "List repository refs",
        .description = "List a repository's branches and tags and the change each points at.",
        .input_schema = id_input(),
        .output_schema = list_schema(repository_ref_schema()),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "id"),
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            return repository_content_list_result(
                ctx.sync_store->repository_refs(repository->customer_id, repository->id),
                repository_ref_json);
        }});
    registry.add(Capability{
        .name = "repository_tree",
        .title = "Read a repository tree",
        .description =
            "List every file and folder of a repository at a branch, tag or change, to browse it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"repository", {{"type", "string"}}},
                           {"revision", {{"type", "string"}}},
                           {"recursive", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .output_schema = list_schema(repository_tree_entry_schema()),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = repository_content_object,
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx, true);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            return repository_content_list_result(
                ctx.sync_store->repository_tree(repository->customer_id,
                                                repository->id,
                                                repository_input_value(in, "ref", "revision")),
                repository_tree_entry_json);
        }});
    registry.add(Capability{
        .name = "repository_file",
        .title = "Read a repository file",
        .description =
            "Read one file of a repository at a branch, tag or change, with its contents and size.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"repository", {{"type", "string"}}},
                           {"revision", {{"type", "string"}}},
                           {"path", {{"type", "string"}}}}},
                         {"required", json::array({"path"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"path", {{"type", "string"}}},
                            {"blob", {{"type", "string"}}},
                            {"contents", {{"type", "string"}}},
                            {"content_base64", {{"type", "string"}}},
                            {"size", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = repository_content_object,
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx, true);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            const auto file =
                ctx.sync_store->repository_file(repository->customer_id,
                                                repository->id,
                                                repository_input_value(in, "ref", "revision"),
                                                in.at("path").get<std::string>());
            if (!file) {
                return from_sync_store_error(file.error());
            }
            return ApiResult::success(repository_file_json(*file));
        }});
    registry.add(Capability{
        .name = "list_changes",
        .title = "List repository changes",
        .description = "List a repository's change history, newest first, with author and message.",
        .input_schema = id_input(),
        .output_schema = list_schema(repository_change_schema()),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "id"),
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            return repository_content_list_result(
                ctx.sync_store->repository_log(repository->customer_id, repository->id),
                repository_change_json);
        }});
    registry.add(Capability{
        .name = "get_change",
        .title = "Get repository change",
        .description = "Read one change in a repository's history: its author, message, parents "
                       "and when it landed.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"change_id", {{"type", "string"}}}}},
                         {"required", json::array({"id", "change_id"})},
                         {"additionalProperties", false}},
        .output_schema = repository_change_schema(),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "id"),
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            const auto changes =
                ctx.sync_store->repository_log(repository->customer_id, repository->id);
            if (!changes) {
                return from_sync_store_error(changes.error());
            }
            const auto change_id = in.at("change_id").get<std::string>();
            const auto found = std::ranges::find(*changes, change_id, &RepositoryChange::id);
            if (found == changes->end()) {
                return ApiResult::failure("not_found", "no change " + change_id);
            }
            return ApiResult::success(repository_change_json(*found));
        }});
    registry.add(Capability{
        .name = "change_diff",
        .title = "Diff a repository change",
        .description = "The structural and text diff between a change and its first parent.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"change_id", {{"type", "string"}}}}},
                         {"required", json::array({"id", "change_id"})},
                         {"additionalProperties", false}},
        .output_schema = list_schema(repository_diff_entry_schema()),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "id"),
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            return repository_content_list_result(
                ctx.sync_store->repository_diff(
                    repository->customer_id, repository->id, in.at("change_id").get<std::string>()),
                repository_diff_entry_json);
        }});
    registry.add(Capability{
        .name = "list_conflicts",
        .title = "List repository conflicts",
        .description = "Unresolved conflicts with their kind, region, explanation, and values.",
        .input_schema = id_input(),
        .output_schema = list_schema(repository_conflict_schema()),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "id"),
        .authorizes_candidates = true,
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            return repository_content_list_result(
                ctx.sync_store->repository_conflicts(repository->customer_id, repository->id),
                repository_conflict_json);
        }});
    registry.add(Capability{.name = "list_artifacts",
                            .title = "List repository artifacts",
                            .description = "List a repository's build results, filtered by action, "
                                           "output or content, a page at a time.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"id", {{"type", "string"}}},
                                               {"action", {{"type", "string"}}},
                                               {"output", {{"type", "string"}}},
                                               {"content", {{"type", "string"}}},
                                               {"cursor", {{"type", "string"}}},
                                               {"limit", {{"type", "integer"}}}}},
                                             {"required", json::array({"id"})},
                                             {"additionalProperties", false}},
                            .output_schema = {{"type", "object"},
                                              {"properties",
                                               {{"items", {{"type", "array"}}},
                                                {"next_cursor", {{"type", "string"}}},
                                                {"done", {{"type", "boolean"}}}}}},
                            .kind = CapabilityKind::Read,
                            .object = "repository",
                            .action = "read",
                            .resolve_object = entity_object("repository", "id"),
                            .authorizes_candidates = true,
                            .allows_anonymous_public_read = true,
                            .handler = [](const json& in, ApiContext& ctx) {
                                const auto repository = readable_repository(in, ctx);
                                if (!repository) {
                                    return ApiResult::failure(repository.error().code,
                                                              repository.error().message,
                                                              repository.error().path);
                                }
                                if (!ctx.sync_store) {
                                    return no_repository_store();
                                }
                                const auto page = ctx.sync_store->repository_artifacts(
                                    repository->customer_id,
                                    repository->id,
                                    {.action = in.value("action", ""),
                                     .output = in.value("output", ""),
                                     .content = in.value("content", ""),
                                     .cursor = in.value("cursor", ""),
                                     .limit = static_cast<std::uint64_t>(limit_of(in))});
                                if (!page) {
                                    return from_sync_store_error(page.error());
                                }
                                json items = json::array();
                                for (const RepositoryArtifact& artifact : page->items) {
                                    items.push_back(repository_artifact_json(artifact));
                                }
                                return ApiResult::success({{"items", std::move(items)},
                                                           {"next_cursor", page->next_cursor},
                                                           {"done", page->done}});
                            }});
    registry.add(Capability{
        .name = "get_artifact",
        .title = "Get repository artifact",
        .description = "One result with outputs, timing, cache state, attestations, and trace.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}}, {"result_id", {{"type", "string"}}}}},
                         {"required", json::array({"id", "result_id"})},
                         {"additionalProperties", false}},
        .output_schema = repository_artifact_schema(),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "id"),
        .allows_anonymous_public_read = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto repository = readable_repository(in, ctx);
            if (!repository) {
                return ApiResult::failure(
                    repository.error().code, repository.error().message, repository.error().path);
            }
            if (!ctx.sync_store) {
                return no_repository_store();
            }
            const auto artifact = ctx.sync_store->repository_artifact(
                repository->customer_id, repository->id, in.at("result_id"));
            if (!artifact) {
                return from_sync_store_error(artifact.error());
            }
            RepositoryArtifact value = *artifact;
            if (ctx.search) {
                const auto members =
                    ctx.search->artifact_members(repository->customer_id, repository->id, value.id);
                if (!members) {
                    return ApiResult::failure(members.error().code, members.error().message);
                }
                for (const search::ArtifactMember& member : *members) {
                    value.members.push_back({member.artifact,
                                             member.version,
                                             member.path,
                                             member.digest,
                                             member.indexed,
                                             member.reason});
                }
            }
            return ApiResult::success(repository_artifact_json(value));
        }});
    registry.add(Capability{
        .name = "refs",
        .title = "Repository refs",
        .description = "List the refs of one live repository, as its liquid repository holds them.",
        .input_schema = sync_repository_input,
        .output_schema = list_schema(sync_ref_schema()),
        .kind = CapabilityKind::Read,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "repository_id"),
        .handler = [](const json& input, ApiContext& context) {
            auto repository = live_sync_repository(input, context);
            if (!repository) {
                return repository.error();
            }
            auto references = context.sync_store->refs(repository->customer_id, repository->id);
            if (!references) {
                return from_sync_error(context, references.error());
            }
            // Liquid's repository is the authority for refs; lake keeps no copy (lake#319).
            json items = json::array();
            for (const auto& [name, head] : *references) {
                items.push_back({{"repository_id", repository->id}, {"name", name}, {"head", head}});
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "find_missing",
        .title = "Find missing repository objects",
        .description = "Answer liquid's binary missing-object negotiation for one repository.",
        .input_schema = sync_repository_input,
        .output_schema = sync_output,
        .kind = CapabilityKind::Action,
        .transport = CapabilityTransport::LiquidSync,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "repository_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (auto wire = require_sync_wire(context); !wire.ok) {
                return wire;
            }
            auto repository = live_sync_repository(input, context);
            if (!repository) {
                return repository.error();
            }
            auto response = context.sync_store->find_missing(
                repository->customer_id, repository->id, context.sync_request);
            if (!response) {
                return from_sync_error(context, response.error());
            }
            context.sync_response = std::move(*response);
            record_transfer(
                context, *repository, context.sync_request.size(), context.sync_response.size());
            return ApiResult::success({{"bytes", context.sync_response.size()}});
        }});
    registry.add(Capability{
        .name = "push",
        .title = "Push repository metadata",
        .description = "Verify and atomically import a liquid metadata stream.",
        .input_schema = sync_repository_input,
        .output_schema = sync_output,
        .kind = CapabilityKind::Action,
        .transport = CapabilityTransport::LiquidSync,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "repository_id"),
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (auto wire = require_sync_wire(context); !wire.ok) {
                co_return wire;
            }
            auto repository = live_sync_repository(input, context);
            if (!repository) {
                co_return repository.error();
            }
            context.audit_details["workspace_id"] = repository->workspace_id;
            const auto quota = storage_quota_for(repository->customer_id, context);
            if (!quota) {
                co_return quota.error();
            }
            // Branch protection is read from the repository's workspace before the push, which
            // runs synchronously inside the repository store (lake#341).
            auto loaded = co_await load_branch_protection(context, *repository);
            std::expected<BranchProtectionRecords, SyncStoreError> protection =
                loaded ? std::move(*loaded)
                       : std::unexpected(SyncStoreError{"store_error", loaded.error().message,
                                                        "branch protection", {}});
            const auto pool_failure = [](std::string message) -> SyncStoreResult<std::vector<std::byte>> {
                return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
            };
            auto response = co_await blocking_call(
                [&] {
                    return context.sync_store->push(
                        repository->customer_id,
                        repository->id,
                        context.sync_request,
                        sync_change_authorizer(context, *repository, std::move(protection)),
                        *quota);
                },
                pool_failure);
            if (!response) {
                if (response.error().rule == "storage_quota") {
                    context.audit_details["quota"] = {{"status", "throttled"},
                                                      {"quota_bytes", *quota}};
                }
                co_return from_sync_error(context, response.error());
            }
            if (context.search) {
                const auto indexed = co_await blocking_call(
                    [&] { return context.search->reindex_artifacts(repository->customer_id, repository->id); },
                    [](std::string message) -> search::Result<search::ReindexResult> {
                        return std::unexpected(search::Error{"store_error", std::move(message)});
                    });
                if (!indexed) {
                    context.audit_details["artifact_index"] = indexed.error().message;
                }
            }
            context.sync_response = std::move(*response);
            record_transfer(
                context, *repository, context.sync_request.size(), context.sync_response.size());
            co_return ApiResult::success({{"bytes", context.sync_response.size()}});
        }});
    registry.add(
        Capability{.name = "pull",
                   .title = "Pull repository metadata",
                   .description = "Export a liquid metadata stream after the client's cursor.",
                   .input_schema = sync_repository_input,
                   .output_schema = sync_output,
                   .kind = CapabilityKind::Action,
                   .transport = CapabilityTransport::LiquidSync,
                   .object = "repository",
                   .action = "read",
                   .resolve_object = entity_object("repository", "repository_id"),
                   .handler = [](const json& input, ApiContext& context) {
                       if (auto wire = require_sync_wire(context); !wire.ok) {
                           return wire;
                       }
                       auto repository = live_sync_repository(input, context);
                       if (!repository) {
                           return repository.error();
                       }
                       auto response = context.sync_store->pull(
                           repository->customer_id, repository->id, context.sync_request);
                       if (!response) {
                           return from_sync_error(context, response.error());
                       }
                       context.sync_response = std::move(*response);
                       record_transfer(context,
                                       *repository,
                                       context.sync_request.size(),
                                       context.sync_response.size());
                       return ApiResult::success({{"bytes", context.sync_response.size()}});
                   }});
    registry.add(Capability{
        .name = "upload_chunks",
        .title = "Upload repository chunks",
        .description = "Verify and store liquid pack-entry frames in the customer store.",
        .input_schema = sync_repository_input,
        .output_schema = sync_output,
        .kind = CapabilityKind::Action,
        .transport = CapabilityTransport::LiquidSync,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "repository_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (auto wire = require_sync_wire(context); !wire.ok) {
                return wire;
            }
            auto repository = live_sync_repository(input, context);
            if (!repository) {
                return repository.error();
            }
            const auto quota = storage_quota_for(repository->customer_id, context);
            if (!quota) {
                return quota.error();
            }
            auto stored = context.sync_store->upload_chunks(
                repository->customer_id, repository->id, context.sync_request, *quota);
            if (!stored) {
                if (stored.error().rule == "storage_quota") {
                    context.audit_details["quota"] = {{"status", "throttled"},
                                                      {"quota_bytes", *quota}};
                }
                return from_sync_error(context, stored.error());
            }
            record_transfer(context, *repository, context.sync_request.size(), 0);
            context.audit_details["metering"]["stored_bytes"] = *stored;
            return ApiResult::success({{"bytes", context.sync_request.size()}});
        }});
    registry.add(Capability{
        .name = "read_chunk",
        .title = "Read a repository chunk range",
        .description = "Read verified logical bytes from one customer-store object.",
        .input_schema =
            {
                {"type", "object"},
                {"properties",
                 {{"repository_id", {{"type", "string"}}},
                  {"digest", {{"type", "string"}}},
                  {"offset", {{"type", "integer"}}},
                  {"length", {{"type", "integer"}}}}},
                {"required", json::array({"repository_id", "digest", "offset", "length"})},
                {"additionalProperties", false},
            },
        .output_schema = sync_output,
        .kind = CapabilityKind::Read,
        .transport = CapabilityTransport::LiquidSync,
        .object = "repository",
        .action = "read",
        .resolve_object = entity_object("repository", "repository_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (auto wire = require_sync_wire(context); !wire.ok) {
                return wire;
            }
            const std::int64_t offset = input.at("offset");
            const std::int64_t length = input.at("length");
            constexpr std::int64_t maximum_range = 64 * 1024 * 1024;
            if (offset < 0 || length < 0 || length > maximum_range) {
                return ApiResult::failure("invalid_input", "invalid chunk range");
            }
            auto repository = live_sync_repository(input, context);
            if (!repository) {
                return repository.error();
            }
            auto response = context.sync_store->read_chunk(repository->customer_id,
                                                           repository->id,
                                                           input.at("digest"),
                                                           static_cast<std::uint64_t>(offset),
                                                           static_cast<std::uint64_t>(length));
            if (!response) {
                return from_sync_error(context, response.error());
            }
            context.sync_response = std::move(*response);
            record_transfer(context, *repository, 0, context.sync_response.size());
            return ApiResult::success({{"bytes", context.sync_response.size()}});
        }});
}

// The pending invitations addressed to the signed-in person: their email, or the handle of a
// personal space they hold. Each item carries the organization's display name, which the
// invitee cannot otherwise read before joining.
ApiResult own_invitations(ApiContext& ctx, int limit) {
    const auto user = ctx.store->get_user(ctx.acting_user_id);
    if (!user) {
        return from_store_error(user.error());
    }
    std::vector<std::string> handles;
    if (const auto customers = ctx.store->list_customers(10, ctx.acting_user_id)) {
        for (const control::Customer& customer : *customers) {
            handles.push_back(customer.name);
        }
    }
    const auto rows = ctx.store->list_invitee_invitations(user->email, handles, limit);
    if (!rows) {
        return from_store_error(rows.error());
    }
    json items = json::array();
    for (const control::Invitation& invitation : *rows) {
        json item = control::to_json(invitation);
        const auto organization = ctx.store->get_organization(invitation.organization_id);
        item["organization_name"] = organization ? organization->display_name : std::string();
        items.push_back(std::move(item));
    }
    return ApiResult::success({{"items", std::move(items)}});
}

json organization_space_json(const control::OrganizationSpace& space) {
    json value{{"organization", control::to_json(space.organization)},
               {"customer", control::to_json(space.customer)},
               {"membership", control::to_json(space.owner)}};
    if (!space.personal.id.empty()) {
        value["personal_customer"] = control::to_json(space.personal);
    }
    return value;
}

std::string trim_copy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

void register_organization_capabilities(Registry& registry) {
    // ---- teams and membership ----
    registry.add(Capability{
        .name = "list_teams",
        .title = "List teams",
        .description = "List an organization's teams, newest first, to grant one access.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"organization_id"})},
                         {"additionalProperties", false}},
        .output_schema = list_schema(team_schema()),
        .kind = CapabilityKind::Read,
        .object = "organization",
        .action = "read",
        .resolve_object = entity_object("organization", "organization_id"),
        .authorizes_candidates = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto authorization =
                authorize_entity_scope(ctx, "organization", in.at("organization_id"), "read");
            if (!authorization) {
                return authorization_failure(authorization);
            }
            return list_result(ctx.store->list_teams(in.at("organization_id"), limit_of(in)));
        }});
    registry.add(Capability{
        .name = "get_team",
        .title = "Get team",
        .description =
            "Read one team of an organization: its name and description, to grant it access.",
        .input_schema = id_input(),
        .output_schema = team_schema(),
        .kind = CapabilityKind::Read,
        .object = "team",
        .action = "read",
        .resolve_object = entity_object("team", "id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->get_team(in.at("id")));
        }});
    registry.add(Capability{
        .name = "create_team",
        .title = "Create team",
        .description = "Create a team under an organization. Organization admins "
                       "may delegate it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"organization_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}}}},
                         {"required", json::array({"organization_id", "name"})},
                         {"additionalProperties", false}},
        .output_schema = team_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization",
        .action = "admin",
        .resolve_object = entity_object("organization", "organization_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->create_team(control::NewTeam{in.at("organization_id"),
                                                                      in.at("name"),
                                                                      in.value("display_name", ""),
                                                                      ctx.acting_identity,
                                                                      ctx.request_id}));
        }});
    registry.add(
        Capability{.name = "list_memberships",
                   .title = "List memberships",
                   .description = "Organization or team memberships with their TAAP "
                                  "attestation digests. Without organization_id, the "
                                  "signed-in person's own memberships in every organization.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"organization_id", {{"type", "string"}}},
                                      {"team_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(membership_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "organization or team",
                   .action = "read",
                   .resolve_object = scope_object,
                   .authorizes_candidates = true,
                   .handler = [](const json& in, ApiContext& ctx) {
                       if (!ctx.store) {
                           return no_store();
                       }
                       const std::string organization_id = in.value("organization_id", "");
                       const std::string team_id = in.value("team_id", "");
                       if (organization_id.empty()) {
                           if (!team_id.empty() || ctx.acting_user_id.empty()) {
                               return ApiResult::failure(
                                   "invalid_input",
                                   "organization_id is required unless a signed-in person lists "
                                   "their own memberships",
                                   "/organization_id");
                           }
                           return list_result(
                               ctx.store->list_user_memberships(ctx.acting_user_id, limit_of(in)));
                       }
                       const auto authorization = authorize_entity_scope(
                           ctx,
                           team_id.empty() ? "organization" : "team",
                           team_id.empty() ? organization_id : team_id,
                           "read");
                       if (!authorization) {
                           return authorization_failure(authorization);
                       }
                       return list_result(
                           ctx.store->list_memberships(organization_id, team_id, limit_of(in)));
                   }});
    registry.add(Capability{
        .name = "create_membership",
        .title = "Create membership",
        .description = "Grant owner, admin, or member in an organization or team "
                       "using TAAP evidence.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"organization_id", {{"type", "string"}}},
               {"team_id", {{"type", "string"}}},
               {"principal_type",
                {{"type", "string"}, {"enum", json::array({"user", "workload"})}}},
               {"principal_id", {{"type", "string"}}},
               {"principal_taap_id", {{"type", "string"}}},
               {"role", {{"type", "string"}, {"enum", json::array({"owner", "admin", "member"})}}},
               {"attestation_digest", {{"type", "string"}}}}},
             {"required",
              json::array({"organization_id",
                           "principal_type",
                           "principal_id",
                           "principal_taap_id",
                           "role",
                           "attestation_digest"})},
             {"additionalProperties", false}},
        .output_schema = membership_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization or team",
        .action = "admin",
        .resolve_object = scope_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->create_membership(control::NewMembership{in.at("organization_id"),
                                                                    in.value("team_id", ""),
                                                                    in.at("principal_type"),
                                                                    in.at("principal_id"),
                                                                    in.at("principal_taap_id"),
                                                                    in.at("role"),
                                                                    in.at("attestation_digest"),
                                                                    ctx.acting_identity,
                                                                    ctx.request_id}));
        }});
    registry.add(
        Capability{.name = "get_membership",
                   .title = "Get membership",
                   .description = "Read one organization or team membership and its TAAP evidence.",
                   .input_schema = id_input(),
                   .output_schema = membership_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "organization or team",
                   .action = "read",
                   .resolve_object = membership_scope_object,
                   .handler = [](const json& in, ApiContext& ctx) {
                       return one_result(ctx.store->get_membership(in.at("id")));
                   }});
    registry.add(Capability{
        .name = "set_member_role",
        .title = "Set member role",
        .description = "Change an organization or team membership role while preserving an owner.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "string"}}},
                           {"role",
                            {{"type", "string"},
                             {"enum", json::array({"owner", "admin", "member"})}}}}},
                         {"required", json::array({"id", "role"})},
                         {"additionalProperties", false}},
        .output_schema = membership_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization or team",
        .action = "admin",
        .resolve_object = membership_scope_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->set_member_role(in.at("id"), in.at("role"), ctx.acting_identity));
        }});
    registry.add(Capability{
        .name = "remove_member",
        .title = "Remove member",
        .description =
            "Remove a membership and its derived access grant while preserving an owner.",
        .input_schema = id_input(),
        .output_schema = {{"type", "object"},
                          {"properties", {{"removed", {{"type", "boolean"}}}}},
                          {"required", json::array({"removed"})}},
        .kind = CapabilityKind::Action,
        .object = "organization or team",
        .action = "admin",
        .resolve_object = membership_scope_object,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto removed = ctx.store->remove_member(in.at("id"), ctx.acting_identity);
            return removed ? ApiResult::success({{"removed", true}})
                           : from_store_error(removed.error());
        }});

    // ---- invitations ----
    registry.add(
        Capability{.name = "list_invitations",
                   .title = "List invitations",
                   .description = "Invitations in an organization or team, including expiry "
                                  "and acceptance. Without organization_id, the pending "
                                  "invitations addressed to the signed-in person, by email or "
                                  "by the handle of their personal space, with organization_name.",
                   .input_schema = {{"type", "object"},
                                    {"properties",
                                     {{"organization_id", {{"type", "string"}}},
                                      {"team_id", {{"type", "string"}}},
                                      {"limit", {{"type", "integer"}}}}},
                                    {"additionalProperties", false}},
                   .output_schema = list_schema(invitation_schema()),
                   .kind = CapabilityKind::Read,
                   .object = "organization or team",
                   .action = "admin",
                   .resolve_object = scope_object,
                   .authorizes_candidates = true,
                   .handler = [](const json& in, ApiContext& ctx) {
                       if (!ctx.store) {
                           return no_store();
                       }
                       const std::string organization_id = in.value("organization_id", "");
                       const std::string team_id = in.value("team_id", "");
                       if (organization_id.empty()) {
                           if (!team_id.empty() || ctx.acting_user_id.empty()) {
                               return ApiResult::failure(
                                   "invalid_input",
                                   "organization_id is required unless a signed-in person lists "
                                   "the invitations addressed to them",
                                   "/organization_id");
                           }
                           return own_invitations(ctx, limit_of(in));
                       }
                       const auto authorization = authorize_entity_scope(
                           ctx,
                           team_id.empty() ? "organization" : "team",
                           team_id.empty() ? organization_id : team_id,
                           "admin");
                       if (!authorization) {
                           return authorization_failure(authorization);
                       }
                       return list_result(
                           ctx.store->list_invitations(organization_id, team_id, limit_of(in)));
                   }});
    registry.add(Capability{
        .name = "create_invitation",
        .title = "Create invitation",
        .description = "Invite an email address or handle into an organization or team.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"organization_id", {{"type", "string"}}},
               {"team_id", {{"type", "string"}}},
               {"invitee_type", {{"type", "string"}, {"enum", json::array({"email", "handle"})}}},
               {"invitee", {{"type", "string"}}},
               {"role", {{"type", "string"}, {"enum", json::array({"owner", "admin", "member"})}}},
               {"expires_at", {{"type", "string"}}}}},
             {"required",
              json::array({"organization_id", "invitee_type", "invitee", "role", "expires_at"})},
             {"additionalProperties", false}},
        .output_schema = invitation_schema(),
        .kind = CapabilityKind::Action,
        .object = "organization or team",
        .action = "admin",
        .resolve_object = scope_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->create_invitation(control::NewInvitation{in.at("organization_id"),
                                                                    in.value("team_id", ""),
                                                                    in.at("invitee_type"),
                                                                    in.at("invitee"),
                                                                    in.at("role"),
                                                                    in.at("expires_at"),
                                                                    ctx.acting_identity,
                                                                    ctx.request_id}));
        }});
    registry.add(Capability{
        .name = "accept_invitation",
        .title = "Accept invitation",
        .description = "Accept a pending invitation into an attested membership.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"invitation_id", {{"type", "string"}}},
                           {"principal_type",
                            {{"type", "string"}, {"enum", json::array({"user", "workload"})}}},
                           {"principal_id", {{"type", "string"}}},
                           {"principal_taap_id", {{"type", "string"}}},
                           {"attestation_digest", {{"type", "string"}}}}},
                         {"required",
                          json::array({"invitation_id",
                                       "principal_type",
                                       "principal_id",
                                       "principal_taap_id",
                                       "attestation_digest"})},
                         {"additionalProperties", false}},
        .output_schema = membership_schema(),
        .kind = CapabilityKind::Action,
        .object = "invitation scope",
        .action = "admin",
        .resolve_object = invitation_object,
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(
                ctx.store->accept_invitation(control::AcceptInvitation{in.at("invitation_id"),
                                                                       in.at("principal_type"),
                                                                       in.at("principal_id"),
                                                                       in.at("principal_taap_id"),
                                                                       in.at("attestation_digest"),
                                                                       ctx.acting_identity,
                                                                       ctx.request_id}));
        }});

    // ---- organizations a person runs themselves ----
    const json space_output = {{"type", "object"},
                               {"properties",
                                {{"organization", organization_schema()},
                                 {"customer", customer_schema()},
                                 {"membership", membership_schema()},
                                 {"personal_customer", customer_schema()}}},
                               {"required", json::array({"organization", "customer", "membership"})}};
    registry.add(Capability{
        .name = "create_organization_space",
        .title = "Create organization",
        .description = "Start an organization as the signed-in person: the organization, its own "
                       "space (handle generated when name is omitted), and the caller as its "
                       "owner, in one step. Invite colleagues next with create_invitation.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"display_name", {{"type", "string"}}},
                           {"name",
                            {{"type", "string"}, {"description", "handle; generated when omitted"}}}}},
                         {"required", json::array({"display_name"})},
                         {"additionalProperties", false}},
        .output_schema = space_output,
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) {
                return no_store();
            }
            if (ctx.acting_user_id.empty()) {
                return ApiResult::failure("unauthorized",
                                          "an organization is started by a signed-in person; "
                                          "an operator uses create_organization");
            }
            if (trim_copy(in.at("display_name").get<std::string>()).empty()) {
                return ApiResult::failure(
                    "invalid_input", "name the organization", "/display_name");
            }
            auto space = ctx.store->create_organization_space(
                control::NewOrganizationSpace{trim_copy(in.at("display_name").get<std::string>()),
                                              in.value("name", ""),
                                              ctx.acting_user_id,
                                              ctx.acting_identity,
                                              ctx.request_id});
            if (!space) {
                return from_store_error(space.error());
            }
            ctx.audit_customer_id = space->customer.id;
            ctx.audit_object = "organization/" + space->organization.id;
            return ApiResult::success(organization_space_json(*space));
        }});
    registry.add(Capability{
        .name = "convert_personal_space",
        .title = "Turn a personal space into an organization",
        .description = "Turn the caller's personal space into an organization's space. The space "
                       "keeps its id, handle, repositories, workspaces and apps; the caller "
                       "becomes the organization's owner and receives a fresh personal space.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"display_name", {{"type", "string"}}}}},
                         {"required", json::array({"customer_id", "display_name"})},
                         {"additionalProperties", false}},
        .output_schema = space_output,
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = entity_object("customer", "customer_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) {
                return no_store();
            }
            if (ctx.acting_user_id.empty()) {
                return ApiResult::failure(
                    "unauthorized", "only the person whose personal space this is can convert it");
            }
            if (trim_copy(in.at("display_name").get<std::string>()).empty()) {
                return ApiResult::failure(
                    "invalid_input", "name the organization", "/display_name");
            }
            auto space = ctx.store->convert_personal_space(
                control::ConvertPersonalSpace{in.at("customer_id"),
                                              trim_copy(in.at("display_name").get<std::string>()),
                                              ctx.acting_user_id,
                                              ctx.acting_identity,
                                              ctx.request_id});
            if (!space) {
                return from_store_error(space.error());
            }
            ctx.audit_customer_id = space->customer.id;
            ctx.audit_object = "organization/" + space->organization.id;
            return ApiResult::success(organization_space_json(*space));
        }});
    registry.add(Capability{
        .name = "join_organization",
        .title = "Join organization",
        .description = "Accept, as the signed-in person, a pending invitation addressed to their "
                       "email or to the handle of their personal space. The membership records "
                       "the invitation as its evidence and its inviter as the grantor.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"invitation_id", {{"type", "string"}}}}},
                         {"required", json::array({"invitation_id"})},
                         {"additionalProperties", false}},
        .output_schema = membership_schema(),
        .kind = CapabilityKind::Action,
        .object = "principal",
        .action = "write",
        .resolve_object = self_object,
        .handler = [](const json& in, ApiContext& ctx) {
            if (!ctx.store) {
                return no_store();
            }
            if (ctx.acting_user_id.empty()) {
                return ApiResult::failure(
                    "unauthorized", "an invitation is accepted by the person it is addressed to");
            }
            const std::string invitation_id = in.at("invitation_id");
            const auto invitation = ctx.store->get_invitation(invitation_id);
            if (!invitation) {
                return from_store_error(invitation.error());
            }
            const auto addressed = own_invitations(ctx, 500);
            if (!addressed.ok) {
                return addressed;
            }
            const json& items = addressed.value.at("items");
            if (std::ranges::none_of(items, [&](const json& item) {
                    return item.value("id", "") == invitation_id;
                })) {
                return ApiResult::failure(
                    invitation->status == "pending" ? "unauthorized" : "conflict",
                    invitation->status == "pending"
                        ? "this invitation is addressed to someone else, or has expired"
                        : "invitation is not pending",
                    "/invitation_id");
            }
            ctx.audit_object = "organization/" + invitation->organization_id;
            return one_result(ctx.store->accept_invitation(
                control::AcceptInvitation{invitation_id,
                                          "user",
                                          ctx.acting_user_id,
                                          ctx.acting_identity,
                                          "invitation/" + invitation_id,
                                          invitation->created_by_taap_id,
                                          ctx.request_id}));
        }});
}

void register_policy_capabilities(Registry& registry) {
    // ---- object graph and grants ----
    registry.add(Capability{
        .name = "list_objects",
        .title = "List objects",
        .description = "Objects under a parent, optionally narrowed by object type.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"parent_id", {{"type", "string"}}},
                           {"object_type", {{"type", "string"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"additionalProperties", false}},
        .output_schema = list_schema(object_schema()),
        .kind = CapabilityKind::Read,
        .object = "policy object collection",
        .action = "read",
        .resolve_object = [](const json& in, ApiContext&) -> std::expected<std::string, ApiError> {
            return in.value("parent_id", "");
        },
        .authorizes_candidates = true,
        .handler =
            [](const json& in, ApiContext& ctx) {
                return authorized_object_list_result(
                    ctx.store->list_policy_objects(
                        in.value("parent_id", ""), in.value("object_type", ""), limit_of(in)),
                    ctx);
            }});
    registry.add(Capability{.name = "get_object",
                            .title = "Get object",
                            .description =
                                "Read one object of the access-control tree (a space, repository, "
                                "team…) and its parent, to see where its grants come from.",
                            .input_schema = id_input(),
                            .output_schema = object_schema(),
                            .kind = CapabilityKind::Read,
                            .object = "policy object",
                            .action = "read",
                            .resolve_object = direct_object("id"),
                            .handler = [](const json& in, ApiContext& ctx) {
                                return one_result(ctx.store->get_policy_object(in.at("id")));
                            }});
    registry.add(Capability{
        .name = "create_object",
        .title = "Create object",
        .description = "Add a path, environment, secret mount, or secret to the "
                       "object graph.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"object_type", {{"type", "string"}}},
                           {"external_id", {{"type", "string"}}},
                           {"parent_id", {{"type", "string"}}},
                           {"inherit", {{"type", "boolean"}}}}},
                         {"required", json::array({"object_type", "external_id", "parent_id"})},
                         {"additionalProperties", false}},
        .output_schema = object_schema(),
        .kind = CapabilityKind::Action,
        .object = "policy object",
        .action = "admin",
        .resolve_object = direct_object("parent_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->create_policy_object(
                control::NewPolicyObject{in.at("object_type"),
                                         in.at("external_id"),
                                         in.at("parent_id"),
                                         ctx.acting_identity,
                                         ctx.request_id,
                                         in.value("inherit", true)}));
        }});
    registry.add(Capability{
        .name = "list_grants",
        .title = "List grants",
        .description = "List who has been granted access directly on one object, and with which "
                       "role, to audit or revoke it.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"object_id", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"object_id"})},
                         {"additionalProperties", false}},
        .output_schema = list_schema(grant_schema()),
        .kind = CapabilityKind::Read,
        .object = "policy object grants",
        .action = "admin",
        .resolve_object = direct_object("object_id"),
        .authorizes_candidates = true,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto authorization =
                authorize_scope(ctx, in.at("object_id").get<std::string>(), "admin");
            if (!authorization) {
                return authorization_failure(authorization);
            }
            return list_result(ctx.store->list_grants(
                {in.at("object_id").get<std::string>()}, std::nullopt, limit_of(in)));
        }});
    registry.add(Capability{
        .name = "create_grant",
        .title = "Create grant",
        .description = "Grant a policy role on an object using a TAAP attestation digest.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"principal_taap_id", {{"type", "string"}}},
               {"object_id", {{"type", "string"}}},
               {"role",
                {{"type", "string"},
                 {"enum", json::array({"read", "write", "admin", "deploy", "review", "share"})}}},
               {"attestation_digest", {{"type", "string"}}}}},
             {"required",
              json::array({"principal_taap_id", "object_id", "role", "attestation_digest"})},
             {"additionalProperties", false}},
        .output_schema = grant_schema(),
        .kind = CapabilityKind::Action,
        .object = "policy object",
        .action = "admin",
        .resolve_object = direct_object("object_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            return one_result(ctx.store->create_grant(control::NewGrant{in.at("principal_taap_id"),
                                                                        in.at("object_id"),
                                                                        in.at("role"),
                                                                        in.at("attestation_digest"),
                                                                        ctx.acting_identity,
                                                                        ctx.request_id}));
        }});
    registry.add(Capability{
        .name = "effective_access",
        .title = "Effective access",
        .description = "Who can access an object, which actions they have, and "
                       "whether each grant is inherited.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"object_id", {{"type", "string"}}}}},
                         {"required", json::array({"object_id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}, {"properties", {{"items", {{"type", "array"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "effective access",
        .action = "share",
        .resolve_object = direct_object("object_id"),
        .handler = [](const json& in, ApiContext& ctx) {
            auto entries =
                control::effective_access(*ctx.store, in.at("object_id"), ctx.authorization_cache);
            if (!entries) {
                return from_store_error(entries.error());
            }
            json items = json::array();
            for (const auto& entry : *entries) {
                items.push_back(control::to_json(entry));
            }
            return ApiResult::success({{"items", std::move(items)}});
        }});
    registry.add(Capability{
        .name = "entity_effective_access",
        .title = "Effective entity access",
        .description = "Effective access for a repository, organization, or team by external id.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"object_type",
                            {{"type", "string"},
                             {"enum", json::array({"repository", "organization", "team"})}}},
                           {"id", {{"type", "string"}}}}},
                         {"required", json::array({"object_type", "id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"object", object_schema()}, {"items", {{"type", "array"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "effective access",
        .action = "share",
        .resolve_object = entity_type_object,
        .handler = [](const json& in, ApiContext& ctx) {
            const auto object = ctx.store->find_policy_object(in.at("object_type"), in.at("id"));
            if (!object) {
                return from_store_error(object.error());
            }
            const auto entries =
                control::effective_access(*ctx.store, object->id, ctx.authorization_cache);
            if (!entries) {
                return from_store_error(entries.error());
            }
            json items = json::array();
            for (const auto& entry : *entries) {
                items.push_back(control::to_json(entry));
            }
            return ApiResult::success(
                {{"object", control::to_json(*object)}, {"items", std::move(items)}});
        }});
}

void register_search_capability(Registry& registry) {
    registry.add(Capability{
        .name = "search",
        .title = "Search repositories",
        .description = "Ranked BM25F search; quoted text is a substring query and "
                       "/expression/ is a regex query. Historical changes use scan "
                       "mode. Append in:artifact:<name>[@<version>] to search indexed "
                       "members of one published artifact.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"query", {{"type", "string"}}},
                           {"customer", {{"type", "string"}}},
                           {"repository", {{"type", "string"}}},
                           {"ref", {{"type", "string"}}},
                           {"change", {{"type", "string"}}},
                           {"history", {{"type", "boolean"}}},
                           {"limit", {{"type", "integer"}}}}},
                         {"required", json::array({"query"})},
                         {"additionalProperties", false}},
        .output_schema = search_output_schema(),
        .kind = CapabilityKind::Read,
        .object = "repository search results",
        .action = "read",
        .resolve_object = global_object,
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.search) {
                return no_search();
            }
            if (!context.store) {
                return no_store();
            }
            auto result = context.search->query(search::Query{input.at("query").get<std::string>(),
                                                              input.value("customer", ""),
                                                              input.value("repository", ""),
                                                              input.value("ref", ""),
                                                              limit_of(input),
                                                              input.value("change", ""),
                                                              input.value("history", false)},
                                                context.acting_identity,
                                                *context.store);
            if (result) {
                context.audit_object =
                    input.value("repository", "").empty()
                        ? "repository/search"
                        : "repository/" + input.at("repository").get<std::string>();
                context.audit_details = {{"mode", search::to_string(result->mode)},
                                         {"scanned_changes", result->scanned_changes}};
            }
            return search_result(std::move(result));
        }});
    registry.add(Capability{
        .name = "reindex_artifacts",
        .title = "Reindex published artifacts",
        .description = "Decompose every published artifact through libliquid's format registry "
                       "and rebuild its artifact-member search index.",
        .input_schema = {{"type", "object"},
                         {"properties", {{"id", {{"type", "string"}}}}},
                         {"required", json::array({"id"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"artifacts", {{"type", "integer"}}},
                            {"members", {{"type", "integer"}}},
                            {"indexed", {{"type", "integer"}}},
                            {"binary", {{"type", "integer"}}},
                            {"truncated", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "repository",
        .action = "write",
        .resolve_object = entity_object("repository", "id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.search) {
                return no_search();
            }
            if (!context.store) {
                return no_store();
            }
            const auto repository = context.store->get_repository(input.at("id"));
            if (!repository) {
                return from_store_error(repository.error());
            }
            const auto result =
                context.search->reindex_artifacts(repository->customer_id, repository->id);
            if (!result) {
                return ApiResult::failure(result.error().code, result.error().message);
            }
            context.audit_object = "repository/" + repository->id;
            context.audit_details = {{"artifacts", result->artifacts},
                                     {"members", result->members},
                                     {"indexed", result->indexed},
                                     {"binary", result->binary},
                                     {"truncated", result->truncated}};
            return ApiResult::success(context.audit_details);
        }});
}

} // namespace

// ---- instance identity and audit ----
namespace {

// The logs one audit read covers (decision 52, docs/api/audit.md#which-log). `spaces`: the spaces
// whose workspaces a caller may read when the input names none (empty for the operator's `audit`:
// every space).
std::expected<AuditScope, ApiError>
audit_scope(const json& input, ApiContext& context, const std::vector<std::string>& spaces) {
    AuditScope scope;
    if (input.contains("workspace_id")) {
        // Registry::call has checked that it names a workspace (of `customer_id`, when given).
        if (input.contains("after_id")) {
            return std::unexpected(ApiError{"invalid_input",
                                            "after_id pages the central log; export_audit reads "
                                            "a workspace's whole audit",
                                            "/after_id"});
        }
        const auto workspace = context.store->get_workspace(input.at("workspace_id"));
        if (!workspace) {
            return std::unexpected(
                ApiError{"invalid_input", "workspace_id must name a workspace", "/workspace_id"});
        }
        if (!spaces.empty() && std::ranges::find(spaces, workspace->customer_id) == spaces.end()) {
            return std::unexpected(ApiError{
                "unauthorized", "the workspace is not in one of the caller's spaces"});
        }
        scope.central = false;
        scope.workspaces.emplace_back(workspace->customer_id, workspace->id);
        return scope;
    }
    if (input.value("log", "all") == "central" || !context.workspaces) {
        return scope;
    }
    std::vector<std::string> customers;
    if (input.contains("customer_id")) {
        const std::string customer_id = input.at("customer_id");
        if (!spaces.empty() && std::ranges::find(spaces, customer_id) == spaces.end()) {
            return std::unexpected(
                ApiError{"unauthorized", "the space is not one of the caller's", "/customer_id"});
        }
        customers.push_back(customer_id);
    } else if (!spaces.empty()) {
        customers = spaces;
    } else if (context.operator_credential || context.acting_identity == "admin") {
        if (const auto all = context.store->list_customers(1000)) {
            for (const control::Customer& customer : *all) {
                customers.push_back(customer.id);
            }
        }
    }
    scope.workspaces = live_workspaces_of(context, customers);
    return scope;
}

// The synchronous form of the audit reads: the central log, the only log a lake without
// workspace Zettas has. A lake with workspaces reads through Registry::call_async.
ApiResult central_audit_only(const json& input, ApiContext& context, const audit::Filter& filter) {
    if (!context.store) {
        return no_store();
    }
    if (context.workspaces && (input.contains("workspace_id") ||
                               input.value("log", "all") != "central")) {
        return ApiResult::failure("store_error",
                                  "the workspaces' audits are read through call_async");
    }
    auto events = context.store->list_audit_events(filter);
    if (!events) {
        return from_store_error(events.error());
    }
    AuditRead read;
    for (audit::Event& event : *events) {
        read.events.push_back(LoggedEvent{.event = std::move(event)});
    }
    return logged_audit_items(read, context);
}

ApiResult verify_central_audit(ApiContext& context) {
    if (!context.store) {
        return no_store();
    }
    auto events = context.store->list_audit_events(audit::Filter{.limit = 1000000});
    if (!events) {
        return from_store_error(events.error());
    }
    auto verified = audit::verify(*context.store, *events);
    if (!verified) {
        return ApiResult::failure(verified.error().code, verified.error().message);
    }
    return ApiResult::success({{"verified", true}, {"log", "central"}, {"events", events->size()}});
}

} // namespace

void register_audit_capabilities(Registry& registry) {
    registry.add(
        Capability{.name = "identity",
                   .title = "Lake identity",
                   .description = "The public TAAP/1 identity document for this lake instance.",
                   .input_schema = {{"type", "object"},
                                    {"properties", json::object()},
                                    {"additionalProperties", false}},
                   .output_schema = {{"type", "object"}},
                   .kind = CapabilityKind::Read,
                   .requires_auth = false,
                   .object = "server",
                   .action = "read",
                   .resolve_object = global_object,
                   .handler = [](const json&, ApiContext& context) {
                       if (!context.store) {
                           return no_store();
                       }
                       auto value = audit::identity(*context.store, identity_options(context));
                       if (!value) {
                           return ApiResult::failure(value.error().code, value.error().message);
                       }
                       return ApiResult::success(audit::identity_document(*value));
                   }});
    registry.add(Capability{
        .name = "rotate_identity",
        .title = "Rotate lake identity",
        .description = "Rotate the lake TAAP/1 Ed25519 identity with a "
                       "dual-signed rotation document.",
        .input_schema = {{"type", "object"},
                         {"properties", json::object()},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "server",
        .action = "admin",
        .resolve_object = global_object,
        .handler = [](const json&, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            auto value = audit::rotate_identity(*context.store, identity_options(context));
            if (!value) {
                return ApiResult::failure(value.error().code, value.error().message);
            }
            return ApiResult::success(audit::identity_document(*value));
        }});
    registry.add(Capability{
        .name = "audit",
        .title = "Query audit events",
        .description = "Query signed audit events by principal, object, outcome and time, newest "
                       "first unless `order` is `oldest` or `after_id` continues a page. Each "
                       "event names its principal (`principal_name`) and, for a workspace call, "
                       "the inner `capability`. Reads the central log and every workspace's audit "
                       "(decision 52): `workspace_id` reads one workspace's, `customer_id` limits "
                       "the workspaces to one space's, and `log: central` reads the central log "
                       "alone. Each event says its `log`.",
        .input_schema = audit_filter_input(),
        .output_schema = list_schema(audit_event_schema()),
        .kind = CapabilityKind::Read,
        .object = "audit log",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            return central_audit_only(input, context, audit_filter_of(input, context));
        },
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            auto scope = audit_scope(input, context, {});
            if (!scope) {
                co_return ApiResult::failure(scope.error().code, scope.error().message,
                                             scope.error().path);
            }
            auto read = settled_read(
                co_await read_audit_logs(context, audit_filter_of(input, context), *scope));
            if (!read) {
                co_return ApiResult::failure(read.error().code, read.error().message);
            }
            co_return logged_audit_items(*read, context);
        }});
    registry.add(Capability{
        .name = "my_audit",
        .title = "Query my audit events",
        .description =
            "The signed-in principal's own signed audit events, filtered like audit, newest first, "
            "from the central log and the audits of the workspaces of the principal's spaces. "
            "Successful reads (the polling a browser does) are left out unless `include_reads` "
            "is true; failed and denied reads always appear.",
        .input_schema = audit_filter_input(),
        .output_schema = list_schema(audit_event_schema()),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .handler = [](const json& input, ApiContext& context) {
            if (context.acting_identity.empty()) {
                return ApiResult::failure("unauthorized", "no signed-in principal");
            }
            audit::Filter filter =
                audit_filter_of(input, context, {.newest_first = true, .include_reads = false});
            filter.principal = context.acting_identity;
            return central_audit_only(input, context, filter);
        },
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (context.acting_identity.empty()) {
                co_return ApiResult::failure("unauthorized", "no signed-in principal");
            }
            audit::Filter filter =
                audit_filter_of(input, context, {.newest_first = true, .include_reads = false});
            filter.principal = context.acting_identity;
            // The person's own spaces' workspaces; only their own events are read from them.
            std::vector<std::string> spaces;
            if (!context.acting_user_id.empty()) {
                if (const auto customers =
                        context.store->list_customers(500, context.acting_user_id)) {
                    for (const control::Customer& customer : *customers) {
                        spaces.push_back(customer.id);
                    }
                }
            }
            auto scope = audit_scope(input, context, spaces);
            if (!scope) {
                co_return ApiResult::failure(scope.error().code, scope.error().message,
                                             scope.error().path);
            }
            auto read = settled_read(co_await read_audit_logs(context, filter, *scope));
            if (!read) {
                co_return ApiResult::failure(read.error().code, read.error().message);
            }
            co_return logged_audit_items(*read, context);
        }});
    registry.add(Capability{
        .name = "export_audit",
        .title = "Export audit events",
        .description = "Export matching signed audit events as JSON lines: the central log in "
                       "append order, or with `workspace_id` that workspace's whole audit (copied "
                       "central events, then its chain by place), each line naming its `log`.",
        .input_schema = audit_filter_input(),
        .output_schema = {{"type", "object"}, {"properties", {{"jsonl", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "audit log",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            if (input.contains("workspace_id")) {
                return ApiResult::failure("store_error",
                                          "a workspace's audit is read asynchronously");
            }
            auto events = context.store->list_audit_events(
                audit_filter_of(input, context, {.newest_first = false, .include_reads = true}));
            if (!events) {
                return from_store_error(events.error());
            }
            return ApiResult::success({{"jsonl", audit::json_lines(*events)}});
        },
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!context.store) {
                co_return no_store();
            }
            if (!input.contains("workspace_id")) {
                auto events = context.store->list_audit_events(audit_filter_of(
                    input, context, {.newest_first = false, .include_reads = true}));
                if (!events) {
                    co_return from_store_error(events.error());
                }
                co_return ApiResult::success({{"jsonl", audit::json_lines(*events)}});
            }
            const auto workspace = context.store->get_workspace(input.at("workspace_id"));
            if (!workspace) {
                co_return from_store_error(workspace.error());
            }
            auto events = settled_read(co_await export_workspace_audit(
                context, workspace->customer_id, workspace->id));
            if (!events) {
                co_return ApiResult::failure(events.error().code, events.error().message);
            }
            const audit::Filter filter =
                audit_filter_of(input, context, {.newest_first = false, .include_reads = true});
            std::string jsonl;
            int written = 0;
            for (const LoggedEvent& logged : *events) {
                if (filter.limit > 0 && written >= filter.limit) {
                    break;
                }
                if (!audit_filter_matches(filter, logged.event)) {
                    continue;
                }
                json line = audit::to_json(logged.event);
                line["log"] = "workspace";
                line["workspace_id"] = logged.workspace_id;
                if (logged.sequence > 0) {
                    line["sequence"] = logged.sequence;
                }
                jsonl += line.dump();
                jsonl += '\n';
                ++written;
            }
            co_return ApiResult::success({{"jsonl", std::move(jsonl)}});
        }});
    registry.add(Capability{
        .name = "verify_audit",
        .title = "Verify audit chain",
        .description = "Check that an audit log is complete and untampered: every event's "
                       "signature and the hash chain linking them. The central log, or with "
                       "`workspace_id` that workspace's own chain (decision 52).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"workspace_id", {{"type", "string"}}},
                           {"customer_id", {{"type", "string"}}}}},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"verified", {{"type", "boolean"}}},
                            {"log", {{"type", "string"}}},
                            {"workspace_id", {{"type", "string"}}},
                            {"events", {{"type", "integer"}}},
                            {"chained", {{"type", "integer"}}},
                            {"copied", {{"type", "integer"}}},
                            {"last_sequence", {{"type", "integer"}}}}}},
        .kind = CapabilityKind::Read,
        .object = "audit log",
        .action = "read",
        .resolve_object = global_object,
        .handler = [](const json& input, ApiContext& context) {
            if (input.contains("workspace_id")) {
                return ApiResult::failure("store_error",
                                          "a workspace's audit is verified asynchronously");
            }
            return verify_central_audit(context);
        },
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (!input.contains("workspace_id")) {
                co_return verify_central_audit(context);
            }
            if (!context.store) {
                co_return no_store();
            }
            const auto workspace = context.store->get_workspace(input.at("workspace_id"));
            if (!workspace) {
                co_return from_store_error(workspace.error());
            }
            auto verified = co_await verify_workspace_audit_log(context, workspace->customer_id,
                                                                workspace->id);
            if (!verified) {
                co_return ApiResult::failure("store_unavailable", verified.error().message);
            }
            co_return std::move(*verified);
        }});
    registry.add(Capability{
        .name = "set_audit_retention",
        .title = "Set audit retention",
        .description = "Set a space's retention of the central audit log, at or above the "
                       "400-day floor; it can never be reduced. The space's workspace audits are "
                       "kept until their workspace is deleted (decision 52, lake#318).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}}, {"days", {{"type", "integer"}}}}},
                         {"required", json::array({"customer_id", "days"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"customer_id", {{"type", "string"}}},
                            {"days", {{"type", "integer"}}},
                            {"workspace_audit_retention", {{"type", "string"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "admin",
        .resolve_object = entity_object("customer", "customer_id"),
        .handler = [](const json& input, ApiContext& context) {
            if (!context.store) {
                return no_store();
            }
            const int days = input["days"];
            auto valid = audit::validate_retention_days(days);
            if (!valid) {
                return ApiResult::failure(valid.error().code, valid.error().message);
            }
            auto updated = context.store->set_audit_retention_days(input["customer_id"], days);
            if (!updated) {
                return from_store_error(updated.error());
            }
            // Each workspace's audit is the collection lake_audit_events of its Zetta, whose
            // retention_ms is 0: kept until the workspace is deleted, never shorter than `days`.
            return ApiResult::success(
                {{"customer_id", input["customer_id"]},
                 {"days", days},
                 {"workspace_audit_retention", "kept until the workspace is deleted"}});
        }});
}

void register_core_capabilities(Registry& registry, std::string_view version) {
    register_server_info(registry, version);
    register_identity_capabilities(registry);
    register_sso_capabilities(registry);
    register_repository_capabilities(registry);
    register_organization_capabilities(registry);
    register_policy_capabilities(registry);
    register_search_capability(registry);
    register_audit_capabilities(registry);
    register_secret_capabilities(registry);
    register_offline_recovery_capabilities(registry);
    register_forge_capabilities(registry);
    register_export_capabilities(registry);
    register_github_import_capabilities(registry);
    register_pki_capabilities(registry);
    register_metering_capabilities(registry);
    register_drive_capabilities(registry);
    register_model_capabilities(registry);
    register_transparency_capabilities(registry);
    register_timestamp_capabilities(registry);
    register_replication_capabilities(registry);
    register_application_capabilities(registry);
    register_agent_capabilities(registry);
    register_platform_capabilities(registry);
    register_web_app_capabilities(registry);
    register_build_capabilities(registry);
    register_image_capabilities(registry);
    register_stream_capabilities(registry);
    register_experiment_capabilities(registry);
    register_volume_capabilities(registry);
    register_workspace_capabilities(registry);
    register_workspace_record_capabilities(registry);
    register_workspace_store_capabilities(registry);
}

} // namespace lake::api
