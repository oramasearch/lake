// Deleting what a workspace holds (lake#645, docs/api/deletion.md): its datasets, trained models,
// runs and search indexes, which live in the workspace's engine, and a person's Ask Orama
// conversations, which are lake's records in the workspace Zetta. Each is a named capability with
// `dry_run`, so a caller first sees what would go; the event goes to the workspace's own audit
// chain (decision 52). The organization, invitation, grant and secret mount deletions live beside
// their siblings (capabilities.cpp, secrets_capabilities.cpp).

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/api/datasets.hpp"
#include "lake/control/agent_task_records.hpp"
#include "lake/control/ask_messages.hpp"
#include "lake/control/store.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

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

json workspace_input(json properties, json required) {
    properties["customer_id"] = {{"type", "string"}};
    properties["workspace_id"] = {{"type", "string"}};
    properties["dry_run"] = {{"type", "boolean"}};
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

// One engine deletion: what lake calls it, what the engine calls it, and what it deletes, in the
// words a refusal from an older engine uses.
struct EngineDeletion {
    std::string name;
    std::string title;
    std::string description;
    std::string what;  // "datasets", "models", ...
    std::string field; // the engine's input field for the id
};

// The engine's own answer, with `deleted` added. An engine that does not know the capability yet
// (one older than zetta#1257) answers `not_supported` with what to do, never a generic failure.
liquid::runtime::task<ApiResult>
delete_in_engine(const EngineDeletion& deletion, const json& input, ApiContext& context) {
    if (!context.workspaces) {
        co_return ApiResult::failure("store_unavailable",
                                     "workspaces are not configured on this Lake");
    }
    const std::string customer_id = input.at("customer_id");
    const bool dry_run = input.value("dry_run", false);
    json arguments{{"dry_run", dry_run}};
    const std::string id = input.at("id");
    if (deletion.name == "delete_dataset") {
        const auto ref = datasets::parse_id(id);
        if (!ref) {
            co_return ApiResult::failure("invalid_input", "id is <database>/<dataset>", "/id");
        }
        arguments["dataset"] = ref->name;
        if (!ref->database.empty()) {
            arguments["database"] = ref->database;
        }
    } else {
        arguments[deletion.field] = id;
    }
    context.audit_customer_id = customer_id;
    context.audit_kind = dry_run ? "workspace.delete.plan" : "workspace.delete";
    context.audit_object = "customer/" + customer_id + "/workspace";
    context.audit_details = {{"capability", deletion.name}, {"id", id}};
    auto called = co_await call_workspace(context, customer_id, deletion.name, false, arguments);
    if (!called) {
        co_return ApiResult::failure("store_unavailable", called.error().message);
    }
    if (!*called) {
        const control::ZettaControlError& refused = called->error();
        if (refused.code == "unknown_capability") {
            co_return ApiResult::failure("not_supported",
                                         "this workspace's engine cannot delete " + deletion.what +
                                             " yet; it needs a Zetta with " + deletion.name +
                                             " (zetta#1257). Nothing was deleted.");
        }
        auto failed = workspace_failure(refused, input);
        if (failed.error.code == "not_found" && failed.error.path.empty()) {
            failed.error.path = "/id";
        }
        co_return failed;
    }
    json value = (**called).is_object() ? std::move(**called) : json::object();
    value["id"] = id;
    value["dry_run"] = dry_run;
    value["deleted"] = !dry_run;
    if (value.contains("removed")) {
        context.audit_details["removed"] = value["removed"];
    }
    co_return ApiResult::success(std::move(value));
}

void register_engine_deletion(Registry& registry,
                              EngineDeletion deletion,
                              std::string id_description,
                              std::vector<std::pair<std::string, std::string>> aliases) {
    const EngineDeletion bound = deletion;
    registry.add(Capability{
        .name = deletion.name,
        .title = deletion.title,
        .description = deletion.description,
        .input_schema = workspace_input(
            {{"id", {{"type", "string"}, {"minLength", 1}, {"description", id_description}}}},
            json::array({"id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [bound](const json& input,
                                 ApiContext& context) -> liquid::runtime::task<ApiResult> {
            co_return co_await delete_in_engine(bound, input, context);
        },
        .input_aliases = std::move(aliases)});
}

// A task's status in the words get_agent_task uses.
std::string task_status(const control::AgentTask& task) {
    if (task.phase.empty()) {
        return "pending";
    }
    std::string status = task.phase;
    for (char& c : status) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return status;
}

// Removes each task's record from the workspace Zetta; a record already gone is not an error.
liquid::runtime::task<control::ZettaControlResult<void>>
remove_task_records(control::WorkspaceRecords& records,
                    const std::vector<control::AgentTask>& tasks) {
    for (const control::AgentTask& task : tasks) {
        auto removed = control::settled(
            co_await records.remove(control::AgentTaskContentCodec::collection, task.id));
        if (!removed && removed.error().code != "not_found") {
            co_return control::ZettaControlResult<void>(std::unexpected(removed.error()));
        }
    }
    co_return control::ZettaControlResult<void>();
}

// delete_ask_conversation: every message of one of the caller's conversations in the workspace,
// removed from the workspace Zetta by key, and the agent tasks the conversation started (and the
// tasks those asked for, lake#673): each task's record (its request, which repeats the person's
// message, and the agent's answer) in the workspace Zetta, then its content-free dispatch row in
// PostgreSQL. The records go first, so a deletion that fails part way is finished by calling it
// again; they are removed once more after the rows, for an event an agent recorded in between.
liquid::runtime::task<ApiResult> delete_ask_conversation(const json& input, ApiContext& context) {
    if (!context.store || context.acting_user_id.empty()) {
        co_return ApiResult::failure("unauthorized", "deleting a conversation needs a person");
    }
    const bool dry_run = input.value("dry_run", false);
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string conversation_id = input.at("conversation_id");
    context.audit_customer_id = customer_id;
    context.audit_kind = dry_run ? "ask_conversation.delete.plan" : "ask_conversation.delete";
    context.audit_object = "ask_conversation/" + conversation_id;
    context.audit_details = {{"conversation_id", conversation_id}};
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return ApiResult::failure(
            records.error().code, records.error().message, records.error().path);
    }
    control::AskMessages conversation(*records);
    auto messages = control::settled(co_await conversation.list(
        control::ask_conversation_lookup(conversation_id, context.acting_user_id), 0));
    if (!messages) {
        co_return ApiResult::failure(
            messages.error().code, messages.error().message, messages.error().path);
    }
    auto tasks = context.store->list_conversation_agent_tasks(
        customer_id, workspace_id, conversation_id, context.acting_user_id);
    if (!tasks) {
        co_return ApiResult::failure(tasks.error().code, tasks.error().message);
    }
    if (messages->empty() && tasks->empty()) {
        co_return ApiResult::failure("not_found",
                                     "you have no conversation " + conversation_id +
                                         " in this workspace",
                                     "/conversation_id");
    }
    if (!dry_run) {
        if (auto removed = control::settled(co_await remove_task_records(*records, *tasks));
            !removed) {
            co_return ApiResult::failure(removed.error().code, removed.error().message);
        }
        for (const control::AskMessage& message : *messages) {
            auto removed = control::settled(
                co_await records->remove(control::AskMessageCodec::collection, message.id));
            if (!removed && removed.error().code != "not_found") {
                co_return ApiResult::failure(removed.error().code, removed.error().message);
            }
        }
        if (!tasks->empty()) {
            std::vector<std::string> ids;
            ids.reserve(tasks->size());
            for (const control::AgentTask& task : *tasks) {
                ids.push_back(task.id);
            }
            auto dropped = context.store->delete_agent_tasks(customer_id, ids);
            if (!dropped) {
                co_return ApiResult::failure(dropped.error().code, dropped.error().message);
            }
            if (auto again = control::settled(co_await remove_task_records(*records, *tasks));
                !again) {
                co_return ApiResult::failure(again.error().code, again.error().message);
            }
        }
    }
    json listed = json::array();
    for (const control::AgentTask& task : *tasks) {
        listed.push_back({{"task_id", task.id},
                          {"agent", task.agent},
                          {"kind", task.kind},
                          {"status", task_status(task)},
                          {"created_at", task.created_at}});
    }
    const auto count = static_cast<std::int64_t>(messages->size());
    context.audit_details["messages"] = count;
    context.audit_details["agent_tasks"] = static_cast<std::int64_t>(tasks->size());
    co_return ApiResult::success(
        {{"conversation_id", conversation_id},
         {"messages", count},
         {"first_message",
          messages->empty() ? std::string() : messages->front().content.substr(0, 200)},
         {"agent_tasks", std::move(listed)},
         {"dry_run", dry_run},
         {"deleted", !dry_run}});
}

} // namespace

void register_deletion_capabilities(Registry& registry) {
    register_engine_deletion(
        registry,
        {"delete_dataset",
         "Delete a dataset",
         "Delete a dataset of the workspace, its rows and every search index built on it, from "
         "the workspace's engine; it cannot be recovered. Name it by the <database>/<dataset> id "
         "list_datasets returns. `dry_run: true` deletes nothing and answers what would go (rows, "
         "size, indexes). Refused, naming them, while a run or an experiment is still reading "
         "it.",
         "datasets",
         "dataset"},
        "the dataset's <database>/<dataset> id, as list_datasets returns it",
        {});
    register_engine_deletion(
        registry,
        {"delete_model",
         "Delete a model",
         "Delete a trained model from the workspace's engine; it stops being served if it was "
         "published, and cannot be recovered. The run that trained it is kept. `dry_run: true` "
         "deletes nothing and answers what would go.",
         "models",
         "model_id"},
        "the model's id, as Ship → Models shows it",
        {{"model_id", "id"}});
    register_engine_deletion(
        registry,
        {"delete_run",
         "Delete a run",
         "Delete a finished run and its report from the workspace's engine. The models it "
         "trained are kept (delete them with delete_model). Refused while the run is pending or "
         "running. `dry_run: true` deletes nothing and answers what would go and what is kept.",
         "runs",
         "run_id"},
        "the run's id, as list_runs returns it",
        {{"run_id", "id"}});
    register_engine_deletion(
        registry,
        {"delete_index",
         "Delete a search index",
         "Delete a search index from the workspace's engine; it stops being served if it was "
         "published. The dataset it indexes is kept. `dry_run: true` deletes nothing and answers "
         "what would go.",
         "search indexes",
         "index_id"},
        "the index's id, as list_indexes returns it",
        {{"index_id", "id"}});
    registry.add(Capability{
        .name = "delete_ask_conversation",
        .title = "Delete an Ask Orama conversation",
        .description =
            "Delete one of your conversations with Ask Orama or the Agent Factory in one "
            "workspace: every message is removed from the workspace Zetta and cannot be "
            "recovered, and so are the agent tasks it started (and the tasks those asked for): "
            "their requests and answers in the workspace Zetta and their dispatch rows. Only the "
            "person who had it can delete it. `dry_run: true` deletes nothing and answers how "
            "many messages, and which agent tasks, would go.",
        .input_schema = workspace_input({{"conversation_id", {{"type", "string"}}}},
                                        json::array({"conversation_id"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"conversation_id", {{"type", "string"}}},
                            {"messages", {{"type", "integer"}}},
                            {"first_message", {{"type", "string"}}},
                            {"agent_tasks",
                             {{"type", "array"},
                              {"items",
                               {{"type", "object"},
                                {"properties",
                                 {{"task_id", {{"type", "string"}}},
                                  {"agent", {{"type", "string"}}},
                                  {"kind", {{"type", "string"}}},
                                  {"status", {{"type", "string"}}},
                                  {"created_at", {{"type", "string"}}}}},
                                {"required", json::array({"task_id", "agent", "status"})},
                                {"additionalProperties", false}}}}},
                            {"dry_run", {{"type", "boolean"}}},
                            {"deleted", {{"type", "boolean"}}}}},
                          {"required",
                           json::array({"conversation_id", "messages", "agent_tasks", "dry_run", "deleted"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = delete_ask_conversation});
}

} // namespace lake::api
