#include "lake/api/capabilities.hpp"

#include <expected>
#include <map>
#include <string>
#include <string_view>

#include "forge_records_api.hpp"
#include "repository_records_api.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/store.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

ApiResult sync_error(const SyncStoreError& error) {
    return ApiResult::failure(error.code, error.message);
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

json names(const json& values, std::string_view field) {
    json output = json::array();
    if (!values.is_array()) {
        return output;
    }
    for (const json& value : values) {
        if (value.is_object() && value.contains(field) && value.at(field).is_string()) {
            output.push_back(value.at(field));
        }
    }
    return output;
}

json wrap_replacements(const json& workflow) {
    json output = json::array();
    for (const auto& [name, job] : workflow.value("jobs", json::object()).items()) {
        for (const json& step : job.value("steps", json::array())) {
            const std::string run = step.value("run", "");
            if (run.find("cmake") != std::string::npos || run.find("make") != std::string::npos ||
                run.find("configure") != std::string::npos) {
                output.push_back({{"job", name},
                                  {"step", step.value("name", "run")},
                                  {"command", run},
                                  {"replacement", "liquid wrap"}});
            }
        }
    }
    return output;
}

liquid::runtime::task<ApiResult> import_github_repository(const json& input,
                                                          ApiContext& context) {
    if (!context.store || !context.sync_store) {
        co_return ApiResult::failure("store_unavailable",
                                  "control and repository stores are required");
    }
    const json& recording = input.at("recording");
    const auto workspace = repository_workspace(context, input);
    if (!workspace) {
        co_return ApiResult::failure(
            workspace.error().code, workspace.error().message, workspace.error().path);
    }
    // The recorded description is the repository's settings record in its workspace (lake#315).
    const auto created = settled_create(co_await create_repository_with_settings(
        context,
        {input.at("customer_id"), input.at("name"), "", "private", workspace->id},
        recording.value("description", "")));
    if (!created) {
        co_return created.error();
    }
    const auto history = co_await blocking_call(
        [&] {
            return context.sync_store->import_git(created->customer_id, created->id,
                                                  input.at("git_directory"));
        },
        [](std::string message) -> SyncStoreResult<RepositoryGitImport> {
            return std::unexpected(SyncStoreError{"store_unavailable", std::move(message)});
        });
    if (!history) {
        co_return sync_error(history.error());
    }
    // The recorded forge responses go to the new repository's workspace (lake#341).
    auto forge = RepositoryForge::open(context, *created);
    if (!forge) {
        co_return ApiResult::failure(forge.error().code, forge.error().message,
                                     forge.error().path);
    }
    std::map<std::int64_t, std::string> issues;
    json mapped = json::array({"history"});
    json unsupported = json::array();
    for (const json& issue : recording.value("issues", json::array())) {
        const auto record = control::settled(co_await forge->create(
            "issue",
            {{"source", "github"},
             {"number", issue.value("number", 0)},
             {"title", issue.value("title", "")},
             {"body", issue.value("body", "")},
             {"state", issue.value("state", "open")},
             {"labels", names(issue.value("labels", json::array()), "name")},
             {"assignees", names(issue.value("assignees", json::array()), "login")},
             {"references", json::array()}}));
        if (!record) {
            co_return forge_records_failure(record.error());
        }
        issues.emplace(issue.value("number", std::int64_t{}), record->id);
        mapped.push_back("issue");
    }
    for (const json& comment : recording.value("issue_comments", json::array())) {
        const auto parent = issues.find(comment.value("issue_number", std::int64_t{}));
        if (parent == issues.end()) {
            unsupported.push_back("issue comment without recorded issue");
            continue;
        }
        const auto record = control::settled(co_await forge->create(
            "issue_comment",
            {{"issue_id", parent->second},
             {"body", comment.value("body", "")},
             {"author", comment.value("user", json::object()).value("login", "")},
             {"source", "github"}}));
        if (!record) {
            co_return forge_records_failure(record.error());
        }
        mapped.push_back("issue_comment");
    }
    std::map<std::int64_t, std::string> reviews;
    for (const json& pull : recording.value("pulls", json::array())) {
        const std::string sha = pull.value("head", json::object()).value("sha", "");
        const std::string change = history->commit_changes.value(sha, "");
        if (change.empty()) {
            unsupported.push_back("pull request head is absent from imported Git history");
            continue;
        }
        const auto record = control::settled(co_await forge->create(
            "review",
            {{"source", "github"},
             {"number", pull.value("number", 0)},
             {"change_id", change},
             {"state", pull.value("state", "open") == "closed" ? "closed" : "open"},
             {"reviewers", json::array()}}));
        if (!record) {
            co_return forge_records_failure(record.error());
        }
        reviews.emplace(pull.value("number", std::int64_t{}), record->id);
        mapped.push_back("review");
    }
    for (const json& comment : recording.value("review_comments", json::array())) {
        const auto parent = reviews.find(comment.value("pull_number", std::int64_t{}));
        if (parent == reviews.end()) {
            unsupported.push_back("review comment without mapped pull request");
            continue;
        }
        const std::string sha = comment.value("commit_id", "");
        const std::string change = history->commit_changes.value(sha, "");
        if (change.empty()) {
            unsupported.push_back("review comment commit is absent from imported Git history");
            continue;
        }
        const auto record = control::settled(co_await forge->create(
            "review_comment",
            {{"review_id", parent->second},
             {"body", comment.value("body", "")},
             {"author", comment.value("user", json::object()).value("login", "")},
             {"anchor",
              {{"change", change},
               {"path", comment.value("path", "")},
               {"line", comment.value("line", 0)},
               {"column", 0}}},
             {"source", "github"}}));
        if (!record) {
            co_return forge_records_failure(record.error());
        }
        mapped.push_back("review_comment");
    }
    json workflows = json::array();
    for (const json& workflow : recording.value("workflows", json::array())) {
        workflows.push_back({{"path", workflow.value("path", "")},
                             {"document", workflow.value("document", "")},
                             {"wrap_replacements", wrap_replacements(workflow)}});
    }
    if (!workflows.empty()) {
        mapped.push_back("workflow_document");
    }
    const json fidelity = {
        {"format", "github_import_fidelity/1"},
        {"source", recording.value("repository", "github")},
        {"history", {{"commits", history->commits}, {"blobs", history->blobs}}},
        {"mapped", mapped},
        {"not_mapped", unsupported},
        {"workflows", workflows},
        {"limits",
         json::array(
             {"GitHub reactions, checks, and Actions execution history are not imported."})}};
    const auto report =
        control::settled(co_await forge->create("github_import", fidelity));
    if (!report) {
        co_return forge_records_failure(report.error());
    }
    context.audit_customer_id = created->customer_id;
    context.audit_object = "repository/" + created->id;
    context.audit_details = {{"forge_kind", "github_import"},
                             {"record_id", report->id},
                             {"workspace_id", created->workspace_id}};
    co_return ApiResult::success({{"repository", control::to_json(*created)},
                               {"fidelity", fidelity},
                               {"record", control::to_json(*report)}});
}

} // namespace

void register_github_import_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "import_github_repository",
        .title = "Import GitHub repository",
        .description = "Import Git history and recorded GitHub forge responses as a new "
                       "repository in one workspace (`workspace_id`, required) of the "
                       "customer's space.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"customer_id", {{"type", "string"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"name", {{"type", "string"}}},
                           {"git_directory", {{"type", "string"}}},
                           {"recording", {{"type", "object"}}}}},
                         {"required",
                          json::array({"customer_id",
                                       "workspace_id",
                                       "name",
                                       "git_directory",
                                       "recording"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = import_github_repository});
}

} // namespace lake::api
