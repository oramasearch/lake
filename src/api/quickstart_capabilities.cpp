// SPDX-License-Identifier: MIT

// The quickstart process (lake#1128, docs/api/quickstart.md): a new person drops a dataset and Lake
// takes them to search and model answers on their own website. Four capabilities compose the ones
// that already exist, so permissions, the balance gate, audit and usage metering apply unchanged:
//
// - start_quickstart: the account's first workspace when none is named (start_workspace), the
//   dataset (upload_dataset, or an existing one), its search index (create_index) and publishing it
//   (expose_index);
// - quickstart_status: each step's state, with the ids, the training's progress (get_run) and,
//   when asked, what the paid steps cost against the prepaid balance;
// - train_quickstart: a standalone model on the same dataset (train_model), then publishing it
//   (expose_model) once it is trained;
// - quickstart_snippets: the website instructions (a plain web page, React, curl) with the hosts
//   and ids filled in, and a browser key bound to the index (and model) and the website's origin
//   (create_browser_key, lake#1152): made by this call when `origins` is sent, otherwise named as
//   the call that makes it.
//
// Every write is a nested Registry call as the same person. The quickstart's own state is one
// sealed record per quickstart in the workspace Zetta (`lake_quickstarts`); nothing is added to
// lake's PostgreSQL control plane. A step that fails is recorded with why and is tried again by the
// next call; a call refused for the balance (402 payment_required) changes nothing.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/api/datasets.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "quickstart_columns.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;
namespace qc = quickstart_columns;

constexpr control::RecordCollection quickstart_records{"quickstarts", 1};
// The newest quickstarts quickstart_status lists beside the one it answers.
constexpr std::size_t listed_quickstarts = 20;
// The methodology and held-out share Train → Models' New Model starts with.
constexpr std::string_view default_methodology = "M-1";
constexpr double default_holdout = 0.2;
// Where the website snippets put the browser key create_browser_key answers.
constexpr std::string_view key_placeholder = "YOUR_LAKE_SEARCH_KEY";
// The origin the key's create call names until the caller says which website it is for.
constexpr std::string_view example_site = "https://www.example.com";

std::string lookup_of(const std::string& customer_id) {
    return "customer:" + customer_id;
}

json string_schema(std::string_view description = {}) {
    json schema{{"type", "string"}, {"minLength", 1}};
    if (!description.empty()) {
        schema["description"] = description;
    }
    return schema;
}

json input_schema(json properties, json required) {
    properties["customer_id"] = string_schema("the account the quickstart is for");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
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

std::string text(const json& value, const char* key) {
    if (!value.is_object()) {
        return {};
    }
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string now_text() {
    const auto now = std::chrono::system_clock::now();
    return control::record_instant(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

json error_json(const ApiError& error) {
    return {{"code", error.code}, {"message", error.message}};
}

json error_json(const control::ZettaControlError& error) {
    return {{"code", error.code}, {"message", error.message}};
}

ApiResult failed(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

// A finished nested task: a runtime failure is a store error in the task's own terms.
template <class T> struct Taken;
template <> struct Taken<ApiResult> {
    static ApiResult failure(const std::string& message) {
        return ApiResult::failure("store_error", message);
    }
};
template <> struct Taken<json> {
    static json failure(const std::string&) {
        return nullptr;
    }
};
template <> struct Taken<std::optional<ApiResult>> {
    static std::optional<ApiResult> failure(const std::string& message) {
        return ApiResult::failure("store_error", message);
    }
};
template <class V> struct Taken<std::expected<V, ApiResult>> {
    static std::expected<V, ApiResult> failure(const std::string& message) {
        return std::unexpected(ApiResult::failure("store_error", message));
    }
};
template <class V> struct Taken<std::expected<V, std::string>> {
    static std::expected<V, std::string> failure(const std::string& message) {
        return std::unexpected(message);
    }
};
template <class V> struct Taken<control::ZettaControlResult<V>> {
    static control::ZettaControlResult<V> failure(const std::string& message) {
        return std::unexpected(control::ZettaControlError{"store_error", message});
    }
};
template <class T> T take(std::expected<T, liquid::error> completed) {
    if (completed) {
        return std::move(*completed);
    }
    return Taken<T>::failure(completed.error().message);
}

// Another capability, called as the same person through the registry, so its authorization,
// balance gate, audit and metering are its own. A copy of the context keeps this call's own audit
// record as it is.
liquid::runtime::task<ApiResult> nested(ApiContext& context, std::string name, json input) {
    if (context.registry == nullptr) {
        co_return ApiResult::failure("store_unavailable", "no registry to call " + name);
    }
    ApiContext inner = context;
    auto done = co_await context.registry->call_async(name, input, inner);
    co_return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

// A read of the workspace engine for this quickstart's own bookkeeping (the training's run, the
// model it made, an existing dataset's summary): the path every engine read takes, under the
// quickstart call's own authorization and audit.
liquid::runtime::task<control::ZettaControlResult<json>>
engine_read(ApiContext& context,
            const std::string& customer_id,
            const std::string& workspace_id,
            std::string capability,
            json input) {
    if (!context.workspaces) {
        co_return control::ZettaControlResult<json>(std::unexpected(
            control::ZettaControlError{"workspace_not_ready", "a workspace Zetta is required"}));
    }
    ApiContext bound = context;
    bound.workspace_id = workspace_id;
    co_return control::settled(
        co_await call_workspace(bound, customer_id, std::move(capability), true, std::move(input)));
}

// The workspace engine's own profile of the dataset's columns (analyze_dataset), with the
// snapshot date declared as its `as_of` and, once chosen, the column to predict as its `label`
// (for the engine's leakage checks).
liquid::runtime::task<control::ZettaControlResult<json>> analyze(ApiContext& context,
                                                                 const std::string& customer_id,
                                                                 const std::string& workspace_id,
                                                                 const json& dataset,
                                                                 const std::string& as_of,
                                                                 const std::string& label) {
    json input{{"dataset_path", text(dataset, "database") + "." + text(dataset, "name")},
               {"sample_rows", qc::analysis_sample_rows}};
    if (!as_of.empty()) {
        input["as_of"] = as_of;
    }
    if (!label.empty()) {
        input["label"] = label;
    }
    co_return co_await engine_read(
        context, customer_id, workspace_id, "analyze_dataset", std::move(input));
}

// ---- the record ----

json new_record(const std::string& id,
                const std::string& customer_id,
                const std::string& workspace_id,
                const ApiContext& context) {
    const std::string now = now_text();
    return {{"id", id},
            {"customer_id", customer_id},
            {"workspace_id", workspace_id},
            {"created_by",
             context.acting_user_id.empty() ? context.acting_identity : context.acting_user_id},
            {"created_at", now},
            {"updated_at", now},
            {"dataset", nullptr},
            {"index", nullptr},
            {"training", nullptr},
            {"model", nullptr},
            {"errors", json::object()}};
}

bool present(const json& record, const char* key) {
    return record.contains(key) && record.at(key).is_object();
}

void set_error(json& record, const char* step, json error) {
    if (!record["errors"].is_object()) {
        record["errors"] = json::object();
    }
    record["errors"][step] = std::move(error);
}

void clear_error(json& record, const char* step) {
    if (record["errors"].is_object()) {
        record["errors"].erase(step);
    }
}

json error_of(const json& record, const char* step) {
    const json errors = record.value("errors", json::object());
    return errors.is_object() ? errors.value(step, json(nullptr)) : json(nullptr);
}

// Writes the record: a new one only if its key is absent, a known one at the revision it was read.
class Saved {
public:
    Saved(control::WorkspaceRecords records,
          std::optional<std::int64_t> revision,
          std::int64_t lake_revision)
        : records_(std::move(records)), revision_(revision), lake_revision_(lake_revision) {}

    liquid::runtime::task<control::ZettaControlResult<void>> save(json& record) {
        record["updated_at"] = now_text();
        const std::string id = record.at("id");
        const std::string customer = record.at("customer_id");
        const control::RecordCondition condition =
            revision_ ? control::RecordCondition{.if_revision = revision_}
                      : control::RecordCondition{.if_absent = true};
        auto written = control::settled(co_await records_.put(quickstart_records,
                                                              id,
                                                              lookup_of(customer),
                                                              record,
                                                              condition,
                                                              revision_ ? lake_revision_ + 1 : 1));
        if (!written) {
            co_return control::ZettaControlResult<void>(std::unexpected(written.error()));
        }
        revision_ = written->revision;
        lake_revision_ = written->lake_revision;
        co_return control::ZettaControlResult<void>();
    }

private:
    control::WorkspaceRecords records_;
    std::optional<std::int64_t> revision_;
    std::int64_t lake_revision_ = 1;
};

// The quickstarts of a workspace, newest last.
liquid::runtime::task<control::ZettaControlResult<std::vector<control::StoredRecord>>>
list_records(control::WorkspaceRecords& records, const std::string& customer_id) {
    co_return control::settled(
        co_await records.list(quickstart_records, lookup_of(customer_id), listed_quickstarts));
}

// ---- status ----

// The training's progress as the workspace engine reports it: {state, status, model_id, error}.
// `state` is running, done or failed; read live, never stored, so a read changes nothing.
liquid::runtime::task<json> training_progress(ApiContext& context,
                                              const std::string& customer_id,
                                              const std::string& workspace_id,
                                              const json& record) {
    const json training = record.value("training", json(nullptr));
    const std::string run_id = text(training, "run_id");
    if (run_id.empty()) {
        // No run: the engine refused to start it, and the model step holds why (lake#1204).
        co_return json(nullptr);
    }
    auto run = take(
        co_await engine_read(context, customer_id, workspace_id, "get_run", {{"run_id", run_id}}));
    if (!run) {
        co_return json{{"state", "unknown"},
                       {"status", nullptr},
                       {"model_id", nullptr},
                       {"error", error_json(run.error())}};
    }
    std::string status = text(*run, "status");
    std::ranges::transform(
        status, status.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    json progress{{"status", status},
                  {"model_id", nullptr},
                  {"error", nullptr},
                  {"started_at", text(training, "started_at")},
                  {"updated_at", text(*run, "updated")}};
    if (status == "succeeded" || status == "success" || status == "completed") {
        progress["state"] = "done";
        // The model the run made: the version list_models names with this run.
        auto models = take(co_await engine_read(
            context, customer_id, workspace_id, "list_models", json::object()));
        if (models && models->contains("models") && models->at("models").is_array()) {
            for (const json& model : models->at("models")) {
                if (text(model, "run_id") == run_id || text(model, "run") == run_id) {
                    progress["model_id"] =
                        text(model, "id").empty() ? text(model, "model_id") : text(model, "id");
                    break;
                }
            }
        }
        if (progress["model_id"].is_null()) {
            progress["state"] = "running";
            progress["note"] = "the run finished; its model is not listed yet";
        }
    } else if (status == "failed" || status == "error" || status == "cancelled" ||
               status == "canceled") {
        progress["state"] = "failed";
        std::string reason = text(*run, "error");
        if (reason.empty() && run->contains("logs") && (*run)["logs"].is_array()) {
            // The engine writes why a run failed to its log, not always to `error`.
            for (const json& line : (*run)["logs"]) {
                if (text(line, "level") == "error" && !text(line, "message").empty()) {
                    reason = text(line, "message");
                }
            }
        }
        if (reason.empty()) {
            reason = "the training run " + status;
        }
        // In plain words (lake#1181), with the engine's own reason beside it for an agent.
        const qc::Choice choice{text(training, "target"), text(training, "target_value"), {}};
        progress["error"] = {{"code", "run_failed"},
                             {"message",
                              qc::explain_run_failure(
                                  reason, choice, text(training, "methodology"))},
                             {"detail", reason}};
    } else {
        progress["state"] = "running";
    }
    co_return progress;
}

json step(std::string_view name,
          std::string_view title,
          std::string_view state,
          json error = nullptr) {
    return {{"step", name}, {"title", title}, {"state", state}, {"error", std::move(error)}};
}

// ---- the conversation (lake#1182) ----

// The most of the columns' description the conversation's first turn carries.
constexpr std::size_t max_page_columns_text = 6000;

// What the Ask Orama conversation about this quickstart knows from the first turn on, as
// send_ask_message's `page` (its values are text): where the person is, the dataset, its row
// count, each column with its detected kind and whether it can be predicted, and the search index.
// So the agent starts from "here is what you could build with this dataset", never "which
// dataset?".
//
// It also says where the Quickstart is (lake#1204), so Ask Orama follows it after a confirm:
// `quickstart_state` (deciding, training, failed, trained or done), `model` (what is training, or
// trained), `model_problem` (why it failed, in plain words) and `quickstart_next` (the person's
// one next step). `progress` is training_progress's answer, or null.
json conversation_page(const json& record, const json& progress = nullptr) {
    const json dataset = record.value("dataset", json(nullptr));
    const json index = record.value("index", json(nullptr));
    json page{{"path", "/"},
              {"quickstart", "model step: decide what to build on the dataset just added"},
              {"quickstart_id", text(record, "id")}};
    const json training = record.value("training", json(nullptr));
    const json model = record.value("model", json(nullptr));
    const bool clustering = qc::label_free(text(training, "methodology"));
    const std::string what =
        clustering ? std::string("a clustering model (M-3) that groups the rows into segments")
        : !text(training, "target").empty()
            ? "a model that predicts " + text(training, "target") +
                  (text(training, "target_value").empty()
                       ? std::string()
                       : " is " + text(training, "target_value"))
            : std::string("a model");
    json problem = error_of(record, "model");
    if (!problem.is_object() && progress.is_object() && progress.value("state", "") == "failed" &&
        progress["error"].is_object()) {
        problem = progress["error"];
    }
    if (model.is_object() && !text(model, "id").empty()) {
        const bool done = model.value("published", false) || model.value("serves", true) == false;
        page["quickstart_state"] = done ? "done" : "trained";
        page["quickstart"] = "done: the model is trained";
        page["model"] = what + ", trained (model " + text(model, "id") + ")";
        page["quickstart_next"] =
            clustering ? "See the segments in Train → Models; the search is on the website step."
            : done     ? "Search on the page shows the model's answer beside each result; copy the "
                         "website code to put both on the website."
                       : "Lake is publishing the model for the website.";
    } else if (problem.is_object()) {
        page["quickstart_state"] = "failed";
        page["quickstart"] = "model step: training failed";
        page["model"] = what;
        page["model_problem"] = text(problem, "message");
        page["quickstart_next"] = "Propose the corrected train_quickstart as a card the person "
                                  "confirms, or the person presses Try again.";
    } else if (training.is_object() && !text(training, "run_id").empty()) {
        page["quickstart_state"] = "training";
        page["quickstart"] = "model step: training";
        page["model"] = what + ", training since " + text(training, "started_at");
        page["quickstart_next"] =
            clustering ? "Wait for training to finish (quickstart_status follows it); then the "
                         "segments are in Train → Models."
                       : "Wait for training to finish (quickstart_status follows it); then Lake "
                         "publishes the model and search shows its answers.";
    } else {
        page["quickstart_state"] = "deciding";
    }
    if (dataset.is_object()) {
        page["dataset"] = text(dataset, "name");
        page["dataset_id"] = text(dataset, "id");
        page["dataset_path"] = text(dataset, "database") + "." + text(dataset, "name");
        if (dataset.contains("rows") && dataset["rows"].is_number()) {
            page["rows"] = qc::value_text(dataset["rows"]);
        }
        std::string columns;
        std::string predictable;
        const json listed = dataset.value("columns", json::array());
        for (const json& column : listed.is_array() ? listed : json::array()) {
            const std::string name = text(column, "name");
            std::string said = name;
            if (!text(column, "title").empty()) {
                said += ": " + text(column, "title");
            } else if (!text(column, "type").empty()) {
                said += ": " + text(column, "type");
            }
            if (column.contains("predict") && column["predict"].value("can", false)) {
                said += " (can be predicted)";
                predictable += (predictable.empty() ? "" : ", ") + name;
            }
            if (columns.size() + said.size() + 2 > max_page_columns_text) {
                columns += "; and more columns (list_datasets has them all)";
                break;
            }
            columns += (columns.empty() ? "" : "; ") + said;
        }
        if (!columns.empty()) {
            page["columns"] = columns;
        }
        page["predictable_columns"] =
            predictable.empty() ? std::string("none: no column is a yes-or-no answer or a short "
                                              "category")
                                : predictable;
    }
    if (index.is_object()) {
        page["search_index"] = text(index, "id");
        page["search_index_published"] = index.value("published", false) ? "yes" : "no";
    }
    if (!text(record, "snapshot_date").empty()) {
        page["snapshot_date"] = text(record, "snapshot_date");
    }
    return page;
}

// The conversation as quickstart_status names it: its id, its first turn, the page every turn
// sends, and the calls that read and continue it. Null until it is started.
json conversation_of(const json& record, const json& progress) {
    const json conversation = record.value("conversation", json(nullptr));
    if (!conversation.is_object()) {
        return nullptr;
    }
    const std::string customer_id = text(record, "customer_id");
    const std::string workspace_id = text(record, "workspace_id");
    const std::string id = text(conversation, "id");
    const json page = conversation_page(record, progress);
    return {{"conversation_id", id},
            {"task_id",
             text(conversation, "task_id").empty() ? json(nullptr)
                                                   : json(text(conversation, "task_id"))},
            {"started_at", conversation.value("started_at", json(nullptr))},
            {"page", page},
            {"read",
             {{"capability", "get_ask_conversation"},
              {"input",
               {{"customer_id", customer_id},
                {"workspace_id", workspace_id},
                {"conversation_id", id}}}}},
            {"reply",
             {{"capability", "send_ask_message"},
              {"input",
               {{"customer_id", customer_id},
                {"workspace_id", workspace_id},
                {"conversation_id", id},
                {"page", page},
                {"message", ""}}}}}};
}

// The person's first message when they write none.
std::string opening_message(const json& record) {
    const json dataset = record.value("dataset", json(nullptr));
    const std::string name = dataset.is_object() ? text(dataset, "name") : std::string();
    return "I just added the dataset " + (name.empty() ? std::string("in the Quickstart") : name) +
           ". What could I build with it? Which ideas are the most interesting?";
}

// The answer every quickstart capability gives: its steps in order with their state, the ids, the
// calls that reach what it made, and what to do next.
json status_of(const json& record, const json& progress) {
    const std::string customer_id = record.at("customer_id");
    const std::string workspace_id = record.at("workspace_id");
    const json dataset = record.value("dataset", json(nullptr));
    const json index = record.value("index", json(nullptr));
    const json training = record.value("training", json(nullptr));
    json model = record.value("model", json(nullptr));
    const bool have_dataset = dataset.is_object();
    const bool index_published = index.is_object() && index.value("published", false);
    std::string model_id = text(model, "id");
    if (model_id.empty() && progress.is_object() && progress["model_id"].is_string()) {
        model_id = progress["model_id"];
    }
    const bool model_published = model.is_object() && model.value("published", false);
    // A segmentation / clustering model answers no question per row, so it is not published for
    // the website (lake#1182): its model answers step is skipped and the quickstart ends there.
    const bool model_serves = !(model.is_object() && model.value("serves", true) == false);
    const json conversation = record.value("conversation", json(nullptr));

    json steps = json::array();
    steps.push_back(step("workspace", "Workspace", "done"));
    steps.push_back(step("dataset",
                         "Dataset",
                         have_dataset                              ? "done"
                         : error_of(record, "dataset").is_object() ? "failed"
                                                                   : "pending",
                         error_of(record, "dataset")));
    steps.push_back(step("search_index",
                         "Search Index",
                         index_published                                ? "done"
                         : error_of(record, "search_index").is_object() ? "failed"
                         : index.is_object()                            ? "running"
                                                                        : "pending",
                         error_of(record, "search_index")));
    steps.push_back(
        step("website", "Search on Your Website", index_published ? "done" : "pending"));
    std::string model_state = "pending";
    json model_error = error_of(record, "model");
    if (!model_id.empty()) {
        model_state = "done";
    } else if (model_error.is_object()) {
        model_state = "failed";
    } else if (progress.is_object()) {
        model_state = progress.value("state", std::string("running"));
        if (model_state == "unknown") {
            model_state = "running";
        }
        if (model_state == "failed") {
            model_error = progress["error"];
        }
    }
    json model_step = step("model", "Model", model_state, model_error);
    if (progress.is_object()) {
        model_step["progress"] = progress;
    }
    steps.push_back(std::move(model_step));
    json answers_step = step("model_answers",
                             "Model Answers on Your Website",
                             model_published                                 ? "done"
                             : !model_serves                                 ? "skipped"
                             : error_of(record, "model_answers").is_object() ? "failed"
                                                                             : "pending",
                             error_of(record, "model_answers"));
    if (!model_serves) {
        answers_step["note"] = "A clustering model groups rows into segments; it answers no "
                               "question per search result, so it is not published for the "
                               "website. Train → Models shows its segments.";
    }
    steps.push_back(std::move(answers_step));

    std::string state = "in_progress";
    std::string next;
    for (const json& entry : steps) {
        if (entry["state"] == "failed") {
            state = "failed";
            if (entry["step"] == "model" && entry["error"].is_object()) {
                // What to choose, in plain words (lake#1181), then the call that trains again.
                next = text(entry["error"], "message") +
                       (qc::label_free(text(training, "methodology"))
                            ? " Then call train_quickstart again with this quickstart_id to train "
                              "the same clustering again (its choices are kept), or send "
                              "methodology and the columns to train another model."
                            : " Then call train_quickstart again with this quickstart_id, "
                              "sending target, target_value, entity_column or as_of to change "
                              "them.");
                break;
            }
            next = "Call " +
                   std::string(entry["step"] == "model" || entry["step"] == "model_answers"
                                   ? "train_quickstart"
                                   : "start_quickstart") +
                   " again with this quickstart_id to try the " + entry["step"].get<std::string>() +
                   " step again.";
            break;
        }
    }
    if (state != "failed") {
        if (model_published) {
            state = "done";
            next = "Done: quickstart_snippets answers the website code with search results and "
                   "model answers. start_quickstart without a quickstart_id starts another.";
        } else if (!model_serves && !model_id.empty()) {
            state = "done";
            next = "Done: the clustering model is trained (Train → Models shows its segments) and "
                   "quickstart_snippets answers the website's search code. start_quickstart "
                   "without a quickstart_id starts another.";
        } else if (model_state == "running") {
            state = "training";
            next = "The model is training: read quickstart_status until the model step is done, "
                   "then call train_quickstart again to publish it.";
        } else if (model_state == "done") {
            state = "trained";
            next = "Call train_quickstart again to publish the model.";
        } else if (index_published && !conversation.is_object()) {
            state = "search_ready";
            next = "Search works: quickstart_snippets answers the website code. Next, decide what "
                   "to build on this dataset: start_quickstart_conversation opens an Ask Orama "
                   "conversation about it that proposes a model, opportunities, an app, an "
                   "analysis or a generated corpus; or call train_quickstart with the columns "
                   "yourself (paid: it runs on the workspace's compute).";
        } else if (index_published) {
            state = "search_ready";
            next = "Search works, and the Ask Orama conversation about this dataset is open: read "
                   "it with get_ask_conversation, reply with send_ask_message (send `page` from "
                   "`conversation.page`), and confirm what it proposes with confirm_ask_proposal; "
                   "confirming a model runs train_quickstart. Or call train_quickstart with the "
                   "columns yourself.";
        } else if (have_dataset) {
            next = "Call start_quickstart again with this quickstart_id to build and publish the "
                   "search index.";
        } else {
            next = "Call start_quickstart with the file's content and format, or a dataset_id.";
        }
    }

    json out{{"quickstart_id", record.at("id")},
             {"customer_id", customer_id},
             {"workspace_id", workspace_id},
             {"state", state},
             {"next", next},
             {"steps", std::move(steps)},
             {"dataset", dataset},
             {"index", index},
             {"training", training},
             {"model", nullptr},
             {"search", nullptr},
             {"predict", nullptr},
             {"conversation", conversation_of(record, progress)},
             {"created_at", record.value("created_at", json(nullptr))},
             {"updated_at", record.value("updated_at", json(nullptr))}};
    // lake#1181: the date the dataset's dates are read as of, and the columns the console offers
    // first (the detected kinds are on dataset.columns).
    out["snapshot_date"] =
        text(record, "snapshot_date").empty() ? json(nullptr) : json(text(record, "snapshot_date"));
    out["suggested"] = have_dataset && qc::detected(dataset.value("columns", json::array()))
                           ? qc::suggest(dataset["columns"])
                           : json(nullptr);
    if (!model_id.empty()) {
        out["model"] = {{"id", model_id}, {"published", model_published}};
        if (!model_serves) {
            out["model"]["serves"] = false;
        }
    }
    if (index_published) {
        out["search"] = {{"capability", "search_index"},
                         {"input",
                          {{"customer_id", customer_id},
                           {"workspace_id", workspace_id},
                           {"index_id", text(index, "id")},
                           {"query", ""}}}};
    }
    if (model_published) {
        out["predict"] = {{"capability", "predict"},
                          {"input",
                           {{"customer_id", customer_id},
                            {"workspace_id", workspace_id},
                            {"model_id", model_id},
                            {"rows", json::array()}}}};
    }
    return out;
}

// The answer while the workspace is not ready: nothing is stored yet, because the quickstart's
// record lives in the workspace's own Zetta.
json waiting_for(const std::string& customer_id,
                 const std::string& workspace_id,
                 const json& workspace_state) {
    json steps = json::array();
    steps.push_back(step("workspace", "Workspace", "running"));
    for (const auto& [name, title] : std::to_array<std::pair<std::string_view, std::string_view>>(
             {{"dataset", "Dataset"},
              {"search_index", "Search Index"},
              {"website", "Search on Your Website"},
              {"model", "Model"},
              {"model_answers", "Model Answers on Your Website"}})) {
        steps.push_back(step(name, title, "pending"));
    }
    return {{"quickstart_id", nullptr},
            {"customer_id", customer_id},
            {"workspace_id", workspace_id},
            {"state", "waiting_for_workspace"},
            {"next",
             "The workspace is starting. Call start_quickstart again with the same input and this "
             "workspace_id once quickstart_status reports the workspace ready; nothing was kept "
             "yet."},
            {"workspace", workspace_state},
            {"steps", std::move(steps)},
            {"dataset", nullptr},
            {"index", nullptr},
            {"training", nullptr},
            {"model", nullptr},
            {"search", nullptr},
            {"predict", nullptr},
            {"conversation", nullptr},
            {"snapshot_date", nullptr},
            {"suggested", nullptr},
            {"created_at", nullptr},
            {"updated_at", nullptr}};
}

json summary_of(const control::StoredRecord& stored) {
    const json& body = stored.body;
    const json dataset = body.value("dataset", json(nullptr));
    return {{"quickstart_id", stored.id},
            {"dataset_id", dataset.is_object() ? json(text(dataset, "id")) : json(nullptr)},
            {"created_at", body.value("created_at", json(nullptr))},
            {"updated_at", body.value("updated_at", json(nullptr))}};
}

// The workspace's state; `ready` once lake can call its Zetta.
using Refusable = std::expected<json, ApiResult>;

liquid::runtime::task<Refusable> workspace_state(ApiContext& context,
                                                 const std::string& customer_id,
                                                 const std::string& workspace_id) {
    if (!context.workspaces) {
        co_return Refusable(std::unexpected(
            ApiResult::failure("store_unavailable", "workspaces are not configured")));
    }
    auto state = co_await context.workspaces->state(customer_id, workspace_id);
    if (!state) {
        co_return Refusable(
            std::unexpected(ApiResult::failure("store_unavailable", state.error().message)));
    }
    if (!*state) {
        co_return Refusable(std::unexpected(
            ApiResult::failure((*state).error().code, (*state).error().message, "/workspace_id")));
    }
    co_return Refusable((**state).to_json());
}

// One quickstart's record by id, or the newest in the workspace when `id` is empty. nullopt when
// there is none.
struct Loaded {
    std::optional<control::StoredRecord> record;
    std::vector<control::StoredRecord> listed;
};

using LoadResult = std::expected<Loaded, ApiResult>;

liquid::runtime::task<LoadResult> load(control::WorkspaceRecords& records,
                                       const std::string& customer_id,
                                       const std::string& id,
                                       bool list_all) {
    Loaded loaded;
    if (!id.empty()) {
        auto got = control::settled(co_await records.get(quickstart_records, id));
        if (!got) {
            co_return LoadResult(std::unexpected(failed(got.error())));
        }
        if (!*got || text((**got).body, "customer_id") != customer_id) {
            co_return LoadResult(std::unexpected(ApiResult::failure(
                "not_found", "no quickstart " + id + " in this workspace", "/quickstart_id")));
        }
        loaded.record = std::move(**got);
    }
    if (id.empty() || list_all) {
        auto listed = take(co_await list_records(records, customer_id));
        if (!listed) {
            co_return LoadResult(std::unexpected(failed(listed.error())));
        }
        loaded.listed = std::move(*listed);
        if (id.empty() && !loaded.listed.empty()) {
            loaded.record = loaded.listed.back();
        }
    }
    co_return LoadResult(std::move(loaded));
}

// ---- costs ----

// What the paid steps are billed by, at the list in force, and the account's balance: what the
// console shows before a paid step runs. Only where billing is on are rates and a balance read;
// the amount a step costs depends on how long its work runs, so each dimension's rate is given.
liquid::runtime::task<json> costs_for(ApiContext& context, const std::string& customer_id) {
    const bool billing =
        context.registry != nullptr && context.registry->find("price_list") != nullptr;
    json prices = json::array();
    json balance = nullptr;
    std::string currency = "USD";
    if (billing) {
        const ApiResult listed = take(co_await nested(context, "price_list", json::object()));
        if (listed.ok && listed.value["list"].is_object()) {
            prices = listed.value["list"].value("prices", json::array());
            currency = listed.value["list"].value("currency", currency);
        }
        if (context.registry->find("account_balance") != nullptr) {
            const ApiResult read =
                take(co_await nested(context, "account_balance", {{"customer_id", customer_id}}));
            if (read.ok) {
                balance = read.value;
                for (const char* history : {"credits", "charges"}) {
                    balance.erase(history);
                }
            }
        }
    }
    const auto rate_of = [&](std::string_view dimension) -> json {
        for (const json& price : prices) {
            if (text(price, "dimension") == dimension && text(price, "class").empty()) {
                return {{"unit_micros", price.value("unit_micros", 0)},
                        {"per", price.value("per", 1)}};
            }
        }
        return nullptr;
    };
    const auto billed_by = [&](const std::vector<std::string_view>& capabilities) {
        json dimensions = json::array();
        json hows = json::array();
        for (const std::string_view capability : capabilities) {
            for (const CapabilityMeter& meter : capability_meters()) {
                if (meter.capability != capability) {
                    continue;
                }
                hows.push_back({{"capability", capability}, {"how", meter.how}});
                for (const std::string_view dimension : meter.dimensions) {
                    const bool listed = std::ranges::any_of(dimensions, [&](const json& d) {
                        return text(d, "dimension") == dimension;
                    });
                    if (listed) {
                        continue;
                    }
                    const UsageDimension* known = find_usage_dimension(dimension);
                    dimensions.push_back(
                        {{"dimension", dimension},
                         {"title", known ? std::string(known->title) : std::string(dimension)},
                         {"unit", known ? std::string(known->unit) : std::string()},
                         {"rate", rate_of(dimension)}});
                }
            }
        }
        return std::pair{std::move(dimensions), std::move(hows)};
    };
    struct PaidStep {
        std::string_view name;
        std::vector<std::string_view> capabilities;
        std::string_view note;
    };
    const std::vector<PaidStep> paid{
        {"search_index",
         {"upload_dataset", "create_index", "expose_index"},
         "storing the dataset and building its index run on the workspace engine, billed by its "
         "CPU, RAM and volume while it runs"},
        {"website",
         {"search_index", "predict"},
         "each search and each model answer your website asks for"},
        {"model",
         {"train_model", "expose_model"},
         "training runs on the workspace engine for as long as it takes, which depends on the "
         "dataset's size"}};
    json steps = json::array();
    for (const PaidStep& entry : paid) {
        auto [dimensions, hows] = billed_by(entry.capabilities);
        steps.push_back({{"step", entry.name},
                         {"capabilities", json(entry.capabilities)},
                         {"billed_by", std::move(dimensions)},
                         {"how", std::move(hows)},
                         {"note", entry.note}});
    }
    co_return json{{"billing", billing},
                   {"currency", currency},
                   {"balance", std::move(balance)},
                   {"steps", std::move(steps)}};
}

// The dataset's columns in the file's order, with their types, as get_dataset answers them (the
// index lists its fields sorted); empty when they cannot be read.
json columns_of(const json& summary) {
    json columns = json::array();
    if (!summary.is_object() || !summary.contains("columns") || !summary["columns"].is_array()) {
        return columns;
    }
    for (const json& column : summary["columns"]) {
        if (!text(column, "name").empty()) {
            columns.push_back({{"name", text(column, "name")}, {"type", text(column, "type")}});
        }
    }
    return columns;
}

// ---- start_quickstart ----

liquid::runtime::task<ApiResult> start_quickstart(const json& input, ApiContext& context) {
    if (!context.store || !context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    std::string workspace_id = input.value("workspace_id", std::string());
    const std::string quickstart_id = input.value("quickstart_id", std::string());
    const bool has_content = input.contains("content");
    const bool has_dataset = input.contains("dataset_id");
    if (has_content && has_dataset) {
        co_return ApiResult::failure(
            "invalid_input", "send the file's content or a dataset_id, not both", "/dataset_id");
    }
    if (has_content && !input.contains("format")) {
        co_return ApiResult::failure(
            "invalid_input", "say how the content is laid out: format csv or jsonl", "/format");
    }
    if (has_dataset && !datasets::parse_id(input.at("dataset_id").get<std::string>())) {
        co_return ApiResult::failure("invalid_input",
                                     "dataset_id is <database>/<name>, as list_datasets answers it",
                                     "/dataset_id");
    }
    if (input.contains("name")) {
        if (const std::string problem = datasets::name_problem(input.at("name").get<std::string>());
            !problem.empty()) {
            co_return ApiResult::failure("invalid_input", "name: " + problem, "/name");
        }
    }

    // The workspace: the one named, or the account's first, made when it has none.
    json workspace = nullptr;
    if (workspace_id.empty()) {
        const ApiResult started =
            take(co_await nested(context, "start_workspace", {{"customer_id", customer_id}}));
        if (!started.ok) {
            co_return started;
        }
        workspace = started.value;
        workspace_id = text(workspace, "workspace_id");
        if (workspace_id.empty()) {
            co_return ApiResult::failure("store_error", "start_workspace named no workspace");
        }
    } else {
        auto state = take(co_await workspace_state(context, customer_id, workspace_id));
        if (!state) {
            co_return std::move(state.error());
        }
        workspace = std::move(*state);
    }
    context.audit_workspace_id = workspace_id;
    context.audit_details["workspace_id"] = workspace_id;
    if (!workspace.value("ready", false)) {
        co_return ApiResult::success(waiting_for(customer_id, workspace_id, workspace));
    }

    auto opened = workspace_records(context, customer_id, workspace_id);
    if (!opened) {
        co_return failed(opened.error());
    }
    json record;
    std::optional<std::int64_t> revision;
    std::int64_t lake_revision = 1;
    if (!quickstart_id.empty()) {
        auto loaded = take(co_await load(*opened, customer_id, quickstart_id, false));
        if (!loaded) {
            co_return std::move(loaded.error());
        }
        record = loaded->record->body;
        revision = loaded->record->revision;
        lake_revision = loaded->record->lake_revision;
    } else {
        record = new_record(control::new_uuid_v4(), customer_id, workspace_id, context);
    }
    context.audit_details["quickstart_id"] = record.at("id");
    Saved saved(*opened, revision, lake_revision);
    const auto answer = [&]() -> ApiResult {
        return ApiResult::success(status_of(record, json(nullptr)));
    };
    // A step that failed is kept with why; the call answers the quickstart, failed step and all. A
    // refusal for the balance changes nothing and is answered as it is, with the top-up link.
    const auto step_failed =
        [&](const char* name,
            const ApiResult& result) -> liquid::runtime::task<std::optional<ApiResult>> {
        if (result.error.code == "payment_required") {
            co_return result;
        }
        set_error(record, name, error_json(result.error));
        auto written = take(co_await saved.save(record));
        if (!written) {
            co_return failed(written.error());
        }
        co_return answer();
    };

    // 1. The dataset.
    if (!present(record, "dataset")) {
        json dataset;
        if (has_dataset) {
            const auto ref = *datasets::parse_id(input.at("dataset_id").get<std::string>());
            auto summary =
                take(co_await engine_read(context,
                                          customer_id,
                                          workspace_id,
                                          "get_dataset",
                                          {{"dataset", ref.name}, {"database", ref.database}}));
            if (!summary) {
                ApiResult refused = workspace_failure(summary.error(), input);
                if (refused.error.code == "not_found") {
                    refused.error.path = "/dataset_id";
                }
                if (auto done = take(co_await step_failed("dataset", refused))) {
                    co_return std::move(*done);
                }
            }
            dataset = {{"id", datasets::id_of(ref.database, ref.name)},
                       {"database", ref.database},
                       {"name", ref.name},
                       {"rows", summary->value("rows", summary->value("row_count", json(nullptr)))},
                       {"columns", columns_of(*summary)},
                       {"source", "existing"}};
        } else if (has_content) {
            json upload{{"customer_id", customer_id},
                        {"workspace_id", workspace_id},
                        {"content", input.at("content")},
                        {"format", input.at("format")},
                        {"table_name", input.value("name", std::string("quickstart"))},
                        {"if_exists", "rename"}};
            const ApiResult uploaded =
                take(co_await nested(context, "upload_dataset", std::move(upload)));
            if (!uploaded.ok) {
                ApiResult refused = uploaded;
                if (refused.error.path == "/table_name") {
                    refused.error.path = "/name";
                }
                if (auto done = take(co_await step_failed("dataset", refused))) {
                    co_return std::move(*done);
                }
            }
            const std::string database = text(uploaded.value, "database").empty()
                                             ? std::string("datasets")
                                             : text(uploaded.value, "database");
            const std::string table = text(uploaded.value, "table");
            dataset = {{"id",
                        text(uploaded.value, "id").empty() ? datasets::id_of(database, table)
                                                           : text(uploaded.value, "id")},
                       {"database", database},
                       {"name", table},
                       {"rows", uploaded.value.value("rows", json(nullptr))},
                       {"source", "upload"}};
            if (uploaded.value.contains("renamed_from")) {
                dataset["renamed_from"] = uploaded.value["renamed_from"];
            }
        } else {
            co_return ApiResult::failure(
                "invalid_input",
                "send the file's content (with format csv or jsonl) or a dataset_id; the "
                "quickstart has no dataset yet",
                "/content");
        }
        if (!dataset.contains("columns")) {
            auto summary = take(co_await engine_read(
                context,
                customer_id,
                workspace_id,
                "get_dataset",
                {{"dataset", dataset["name"]}, {"database", dataset["database"]}}));
            dataset["columns"] = summary ? columns_of(*summary) : json::array();
        }
        // The snapshot date the dataset's dates are read as of: when it was uploaded, or when the
        // quickstart took an existing dataset (lake#1181). train_quickstart's `as_of` changes it.
        if (text(record, "snapshot_date").empty()) {
            record["snapshot_date"] =
                text(dataset, "source") == "upload" ? text(record, "created_at") : now_text();
        }
        // Each column's kind, from the engine's own profile. Without it (an engine that cannot
        // profile) the columns keep their storage types and training detects them again.
        auto analysis = take(co_await analyze(
            context, customer_id, workspace_id, dataset, text(record, "snapshot_date"), {}));
        if (analysis) {
            dataset["columns"] = qc::detect_columns(*analysis, dataset["columns"]);
        }
        record["dataset"] = std::move(dataset);
        clear_error(record, "dataset");
        auto written = take(co_await saved.save(record));
        if (!written) {
            co_return failed(written.error());
        }
    } else if (has_content || has_dataset) {
        co_return ApiResult::failure("invalid_input",
                                     "this quickstart already has its dataset; start another "
                                     "without a quickstart_id",
                                     has_content ? "/content" : "/dataset_id");
    }

    // 2. Its search index.
    if (!present(record, "index")) {
        json create{{"customer_id", customer_id},
                    {"workspace_id", workspace_id},
                    {"table", record["dataset"]["name"]},
                    {"database", record["dataset"]["database"]}};
        if (input.contains("fields")) {
            create["fields"] = input.at("fields");
        }
        const ApiResult created = take(co_await nested(context, "create_index", std::move(create)));
        if (!created.ok) {
            if (auto done = take(co_await step_failed("search_index", created))) {
                co_return std::move(*done);
            }
        }
        const std::string index_id = text(created.value, "index_id").empty()
                                         ? text(created.value, "id")
                                         : text(created.value, "index_id");
        record["index"] = {{"id", index_id},
                           {"fields", created.value.value("fields", json::array())},
                           {"published", created.value.value("exposed", false)}};
        auto written = take(co_await saved.save(record));
        if (!written) {
            co_return failed(written.error());
        }
    }

    // 3. Published, so the website can search it.
    if (!record["index"].value("published", false)) {
        const ApiResult exposed = take(co_await nested(context,
                                                       "expose_index",
                                                       {{"customer_id", customer_id},
                                                        {"workspace_id", workspace_id},
                                                        {"index_id", record["index"]["id"]}}));
        if (!exposed.ok) {
            if (auto done = take(co_await step_failed("search_index", exposed))) {
                co_return std::move(*done);
            }
        }
        record["index"]["published"] = true;
        clear_error(record, "search_index");
        auto written = take(co_await saved.save(record));
        if (!written) {
            co_return failed(written.error());
        }
    }
    co_return answer();
}

// ---- quickstart_status ----

liquid::runtime::task<ApiResult> quickstart_status(const json& input, ApiContext& context) {
    if (!context.store || !context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    auto state = take(co_await workspace_state(context, customer_id, workspace_id));
    if (!state) {
        co_return std::move(state.error());
    }
    json out;
    json listed = json::array();
    if (!state->value("ready", false)) {
        out = waiting_for(customer_id, workspace_id, *state);
        out["next"] = "The workspace is starting; read quickstart_status again in a few seconds.";
    } else {
        auto opened = workspace_records(context, customer_id, workspace_id);
        if (!opened) {
            co_return failed(opened.error());
        }
        auto loaded = take(
            co_await load(*opened, customer_id, input.value("quickstart_id", std::string()), true));
        if (!loaded) {
            co_return std::move(loaded.error());
        }
        for (auto it = loaded->listed.rbegin(); it != loaded->listed.rend(); ++it) {
            listed.push_back(summary_of(*it));
        }
        if (!loaded->record) {
            out = waiting_for(customer_id, workspace_id, *state);
            out["state"] = "not_started";
            out["steps"][0]["state"] = "done";
            out["next"] = "Call start_quickstart with the file's content and format, or a "
                          "dataset_id.";
        } else {
            json record = loaded->record->body;
            const json progress =
                present(record, "training") && !present(record, "model")
                    ? take(co_await training_progress(context, customer_id, workspace_id, record))
                    : json(nullptr);
            // A quickstart started before columns were detected (lake#1181): detect them for this
            // answer, without writing; train_quickstart keeps them.
            if (present(record, "dataset") &&
                !qc::detected(record["dataset"].value("columns", json::array()))) {
                if (text(record, "snapshot_date").empty()) {
                    record["snapshot_date"] = text(record, "created_at");
                }
                auto analysis = take(co_await analyze(context,
                                                      customer_id,
                                                      workspace_id,
                                                      record["dataset"],
                                                      text(record, "snapshot_date"),
                                                      {}));
                if (analysis) {
                    record["dataset"]["columns"] = qc::detect_columns(
                        *analysis, record["dataset"].value("columns", json::array()));
                }
            }
            out = status_of(record, progress);
        }
        out["workspace"] = *state;
    }
    out["quickstarts"] = std::move(listed);
    if (input.value("include_costs", false)) {
        out["costs"] = take(co_await costs_for(context, customer_id));
    }
    co_return ApiResult::success(std::move(out));
}

// ---- train_quickstart ----

// The rows a training copy is made from, at most this many, read this many at a time.
constexpr std::int64_t max_prepared_rows = 1'000'000;
constexpr std::int64_t read_page_rows = 5000;

// The dataset's rows, read as the caller through the engine's SQL (the read every engine read
// takes), a page at a time. Refused in plain words.
liquid::runtime::task<std::expected<std::vector<json>, std::string>>
read_rows(ApiContext& context,
          const std::string& customer_id,
          const std::string& workspace_id,
          const json& dataset) {
    using Rows = std::expected<std::vector<json>, std::string>;
    const std::string table = text(dataset, "database") + "." + text(dataset, "name");
    const bool plain = std::ranges::all_of(
        table, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '.'; });
    if (!plain || std::ranges::count(table, '.') != 1) {
        co_return Rows(std::unexpected(
            "Lake cannot read " + text(dataset, "id") +
            " to prepare its columns: SQL cannot name it. Upload the file again under a name of "
            "letters, digits and underscores."));
    }
    auto counted = take(co_await engine_read(context,
                                             customer_id,
                                             workspace_id,
                                             "sql",
                                             {{"statement", "SELECT COUNT(*) FROM " + table}}));
    if (!counted) {
        co_return Rows(std::unexpected("Lake could not read " + text(dataset, "id") +
                                       " to prepare its columns: " + counted.error().message));
    }
    std::int64_t total = 0;
    const json counts = counted->value("rows", json::array());
    if (!counts.empty() && counts[0].is_object()) {
        for (const auto& [key, value] : counts[0].items()) {
            if (value.is_number()) {
                total = value.get<std::int64_t>();
            }
        }
    }
    total = std::min(total, max_prepared_rows);
    std::vector<json> rows;
    rows.reserve(static_cast<std::size_t>(total));
    for (std::int64_t offset = 0; offset < total; offset += read_page_rows) {
        auto page = take(co_await engine_read(
            context,
            customer_id,
            workspace_id,
            "sql",
            {{"statement",
              "SELECT * FROM " + table + " LIMIT " + std::to_string(read_page_rows) + " OFFSET " +
                  std::to_string(offset)}}));
        if (!page) {
            co_return Rows(std::unexpected("Lake could not read " + text(dataset, "id") +
                                           " to prepare its columns: " + page.error().message));
        }
        const json got = page->value("rows", json::array());
        for (const json& row : got) {
            rows.push_back(row);
        }
        if (got.size() < static_cast<std::size_t>(read_page_rows)) {
            break;
        }
    }
    co_return Rows(std::move(rows));
}

liquid::runtime::task<ApiResult> train_quickstart(const json& input, ApiContext& context) {
    if (!context.store || !context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    auto state = take(co_await workspace_state(context, customer_id, workspace_id));
    if (!state) {
        co_return std::move(state.error());
    }
    if (!state->value("ready", false)) {
        co_return ApiResult::failure("workspace_not_ready",
                                     "the workspace is not ready yet; read quickstart_status",
                                     "/workspace_id");
    }
    auto opened = workspace_records(context, customer_id, workspace_id);
    if (!opened) {
        co_return failed(opened.error());
    }
    auto loaded = take(
        co_await load(*opened, customer_id, input.value("quickstart_id", std::string()), false));
    if (!loaded) {
        co_return std::move(loaded.error());
    }
    if (!loaded->record) {
        co_return ApiResult::failure(
            "invalid_input",
            "this workspace has no quickstart yet; call start_quickstart first",
            "/quickstart_id");
    }
    json record = loaded->record->body;
    context.audit_details["quickstart_id"] = record.at("id");
    if (!present(record, "dataset")) {
        co_return ApiResult::failure(
            "invalid_input",
            "the quickstart has no dataset yet; call start_quickstart first",
            "/quickstart_id");
    }
    Saved saved(*opened, loaded->record->revision, loaded->record->lake_revision);
    const auto save = [&]() -> liquid::runtime::task<std::optional<ApiResult>> {
        auto written = take(co_await saved.save(record));
        if (!written) {
            co_return failed(written.error());
        }
        co_return std::nullopt;
    };

    // Already trained and published: nothing to do.
    if (present(record, "model") && record["model"].value("published", false)) {
        co_return ApiResult::success(status_of(record, json(nullptr)));
    }

    std::string model_id = present(record, "model") ? text(record["model"], "id") : std::string();
    json progress = nullptr;
    if (model_id.empty() && present(record, "training")) {
        progress = take(co_await training_progress(context, customer_id, workspace_id, record));
        // A training the engine refused to start has no run (lake#1204): nothing to read.
        const std::string training_state =
            progress.is_object() ? progress.value("state", std::string()) : std::string();
        if (training_state == "running" || training_state == "unknown") {
            co_return ApiResult::success(status_of(record, progress));
        }
        if (training_state == "done") {
            model_id = progress.value("model_id", std::string());
            record["model"] = {{"id", model_id}, {"published", false}};
            clear_error(record, "model");
            if (auto failure = take(co_await save())) {
                co_return std::move(*failure);
            }
        }
        // A failed run is trained again below, with the settings sent now or the ones before.
    }

    if (model_id.empty()) {
        const json before = record.value("training", json::object());
        const int attempt = before.is_object() ? before.value("attempt", 0) + 1 : 1;
        std::string name = input.value("name", std::string());
        if (name.empty()) {
            name = text(record["dataset"], "name") + " quickstart " +
                   record.at("id").get<std::string>().substr(0, 8);
            if (attempt > 1) {
                name += " (" + std::to_string(attempt) + ")";
            }
        }
        const auto setting = [&](const char* key, json fallback) {
            if (input.contains(key)) {
                return input.at(key);
            }
            if (before.is_object() && before.contains(key) && !before.at(key).is_null()) {
                return before.at(key);
            }
            return fallback;
        };
        const auto setting_text = [&](const char* key) {
            const json value = setting(key, nullptr);
            return value.is_string() ? value.get<std::string>() : std::string();
        };

        // The snapshot date the dataset's dates are read as of (lake#1181): the one sent, else the
        // quickstart's, else when it was made.
        if (input.contains("as_of")) {
            const std::string as_of = qc::snapshot_date(input.at("as_of").get<std::string>());
            if (as_of.empty()) {
                co_return ApiResult::failure(
                    "invalid_input",
                    "as_of is a date such as 2026-10-24, or an ISO-8601 time such as "
                    "2026-10-24T09:30:00Z",
                    "/as_of");
            }
            record["snapshot_date"] = as_of;
        } else if (text(record, "snapshot_date").empty()) {
            record["snapshot_date"] = text(record, "created_at");
        }
        const std::string as_of = text(record, "snapshot_date");

        const std::string methodology = setting_text("methodology").empty()
                                            ? std::string(default_methodology)
                                            : setting_text("methodology");
        qc::Choice choice{setting_text("target"), {}, setting_text("entity_column")};
        // The value to look for belongs to its column: choosing another column drops it.
        if (input.contains("target_value")) {
            choice.target_value = input.at("target_value").get<std::string>();
        } else if (!input.contains("target") || text(before, "target") == choice.target) {
            choice.target_value = text(before, "target_value");
        }
        const bool chosen = input.contains("target");
        const std::string source_path = record["dataset"]["database"].get<std::string>() + "." +
                                        record["dataset"]["name"].get<std::string>();

        // A refusal Lake could not fix, or a choice it cannot train: kept on the model step in
        // plain words, never the engine's code.
        // The choices are kept as a training with no run (lake#1204), so Try again trains the same
        // model (a clustering stays a clustering) and the page shows the failure on the model
        // step instead of the conversation that proposed it.
        std::string dataset_path = source_path;
        json derived_kept = nullptr;
        const auto refuse = [&](json error) -> liquid::runtime::task<ApiResult> {
            record["training"] = {
                {"name", name},
                {"methodology", methodology},
                {"target", choice.target.empty() ? json(nullptr) : json(choice.target)},
                {"target_value",
                 choice.target_value.empty() ? json(nullptr) : json(choice.target_value)},
                {"entity_column",
                 choice.entity_column.empty() ? json(nullptr) : json(choice.entity_column)},
                {"holdout_fraction",
                 qc::label_free(methodology) ? json(nullptr)
                                             : setting("holdout_fraction", default_holdout)},
                {"as_of", as_of},
                {"dataset_path", dataset_path},
                {"preparation", nullptr},
                {"derived_target", derived_kept},
                {"attempt", attempt},
                {"run_id", nullptr},
                {"refused_at", now_text()}};
            set_error(record, "model", std::move(error));
            if (auto failure = take(co_await save())) {
                co_return std::move(*failure);
            }
            co_return ApiResult::success(status_of(record, json(nullptr)));
        };

        json preparation = nullptr;
        std::optional<ApiResult> started;
        if (methodology == qc::prepared_methodology) {
            // Each column's kind from the engine's profile, read again with the column to predict
            // as its label, so the engine's leakage checks run (lake#1181).
            json detected = record["dataset"].value("columns", json::array());
            // A target derived from a column (lake#1182): sent now, or kept from the training
            // before unless another column to predict is sent.
            const json derived_input = input.contains("derived_target") ? input.at("derived_target")
                                       : chosen                         ? json(nullptr)
                                       : before.is_object()
                                           ? before.value("derived_target", json(nullptr))
                                           : json(nullptr);
            const bool derived_sent = input.contains("derived_target");
            if ((choice.target.empty() || !derived_input.is_null()) && !qc::detected(detected)) {
                auto first = take(co_await analyze(
                    context, customer_id, workspace_id, record["dataset"], as_of, {}));
                if (first) {
                    detected = qc::detect_columns(*first, detected);
                }
            }
            std::optional<qc::Derived> derived;
            std::optional<std::vector<json>> derived_rows;
            if (!derived_input.is_null()) {
                if (!qc::detected(detected)) {
                    co_return ApiResult::failure("engine_error",
                                                 "Lake could not read this dataset's columns to "
                                                 "derive a column to predict from them.",
                                                 "/derived_target");
                }
                auto parsed = qc::parse_derived(derived_input, detected);
                if (!parsed.derived) {
                    if (derived_sent) {
                        co_return ApiResult::failure(
                            "invalid_input", parsed.problem.message, parsed.problem.path);
                    }
                    co_return co_await refuse(
                        {{"code", "choose_columns"}, {"message", parsed.problem.message}});
                }
                auto rows =
                    take(co_await read_rows(context, customer_id, workspace_id, record["dataset"]));
                if (!rows) {
                    co_return co_await refuse(
                        {{"code", "cannot_prepare"}, {"message", rows.error()}});
                }
                qc::Derive made = qc::derive_target(detected, *rows, *parsed.derived);
                if (!made.problem.empty()) {
                    if (derived_sent) {
                        co_return ApiResult::failure(
                            "invalid_input", made.problem, "/derived_target");
                    }
                    co_return co_await refuse(
                        {{"code", "choose_columns"}, {"message", made.problem}});
                }
                derived = std::move(parsed.derived);
                derived_kept = derived_input;
                derived_kept["name"] = derived->name;
                derived_kept["rule"] = qc::derived_rule(*derived);
                choice.target = derived->name;
                choice.target_value = "yes";
                detected = std::move(made.columns);
                derived_rows = std::move(made.rows);
            }
            if (choice.target.empty() && qc::detected(detected)) {
                const json suggested = qc::suggest(detected);
                if (suggested["target"].is_string()) {
                    choice.target = suggested["target"];
                    if (choice.target_value.empty() && suggested["target_value"].is_string()) {
                        choice.target_value = suggested["target_value"];
                    }
                }
            }
            json leakage = json::array();
            // A derived column is not in the dataset, so the engine cannot check it for leakage;
            // the column it is derived from is left out instead (derive_target).
            if (!derived) {
                auto analysis = take(co_await analyze(
                    context, customer_id, workspace_id, record["dataset"], as_of, choice.target));
                if (analysis) {
                    detected = qc::detect_columns(*analysis, detected);
                    leakage = analysis->value("leakage_candidates", json::array());
                }
            }
            if (qc::detected(detected)) {
                if (!derived) {
                    record["dataset"]["columns"] = detected;
                }
                if (choice.entity_column.empty() && !input.contains("entity_column")) {
                    const json suggested = qc::suggest(detected);
                    if (suggested["entity_column"].is_string() &&
                        suggested["entity_column"] != choice.target) {
                        choice.entity_column = suggested["entity_column"];
                    }
                }
                const json* target_column = qc::find(detected, choice.target);
                if (choice.target_value.empty() && target_column != nullptr &&
                    target_column->contains("predict") &&
                    (*target_column)["predict"].value("choose_value", false)) {
                    const json values = (*target_column)["predict"].value("values", json::array());
                    if (!values.empty()) {
                        choice.target_value = values[0];
                    }
                }
                if (const qc::Problem problem = qc::check_choice(detected, choice);
                    !problem.message.empty()) {
                    if (chosen || input.contains("target_value") ||
                        input.contains("entity_column")) {
                        co_return ApiResult::failure(
                            "invalid_input", problem.message, problem.path);
                    }
                    co_return co_await refuse(
                        {{"code", "choose_columns"}, {"message", problem.message}});
                }
                // A column the engine says may give the answer away is left out too.
                json usable = detected;
                for (json& column : usable) {
                    for (const json& leak : leakage) {
                        if (text(leak, "column") == text(column, "name") &&
                            text(column, "name") != choice.target &&
                            text(column, "name") != choice.entity_column) {
                            column["feature"] = {
                                {"use", false},
                                {"reason",
                                 "it may give the answer away (" + text(leak, "reason") + ")"}};
                        }
                    }
                }

                qc::Fixes fixes;
                json applied = json::array();
                for (int pass = 0; pass < 2; ++pass) {
                    json train{{"customer_id", customer_id},
                               {"workspace_id", workspace_id},
                               {"name", name},
                               {"methodology", methodology},
                               {"holdout_fraction", setting("holdout_fraction", default_holdout)},
                               {"target", choice.target}};
                    if (qc::trains_as_is(usable, choice, fixes)) {
                        dataset_path = source_path;
                        json used = json::array();
                        json dropped = json::array();
                        for (const json& column : usable) {
                            const std::string column_name = text(column, "name");
                            if (column_name == choice.target ||
                                column_name == choice.entity_column) {
                                continue;
                            }
                            if (column.contains("feature") &&
                                column["feature"].value("use", false)) {
                                used.push_back(column_name);
                            } else {
                                dropped.push_back({{"name", column_name},
                                                   {"reason",
                                                    column.contains("feature")
                                                        ? column["feature"].value("reason", "")
                                                        : std::string()}});
                            }
                        }
                        preparation = {{"methodology", methodology},
                                       {"copy", false},
                                       {"dataset_id", record["dataset"]["id"]},
                                       {"entity", choice.entity_column},
                                       {"used", std::move(used)},
                                       {"dropped", std::move(dropped)},
                                       {"label", {{"column", choice.target}, {"positive", "1"}}},
                                       {"applied", applied}};
                        train["entity_column"] = choice.entity_column;
                    } else {
                        auto rows =
                            derived_rows
                                ? std::expected<std::vector<json>, std::string>(*derived_rows)
                                : take(co_await read_rows(
                                      context, customer_id, workspace_id, record["dataset"]));
                        if (!rows) {
                            co_return co_await refuse(
                                {{"code", "cannot_prepare"}, {"message", rows.error()}});
                        }
                        qc::Prepare made = qc::prepare_rows(usable, *rows, choice, fixes);
                        if (!made.prepared) {
                            co_return co_await refuse(
                                {{"code", "cannot_prepare"}, {"message", made.problem}});
                        }
                        const qc::Prepared& prepared = *made.prepared;
                        std::string format = "jsonl";
                        std::string content = prepared.content;
                        if (content.size() > qc::max_upload_bytes) {
                            format = "csv";
                            content = qc::csv_content(prepared);
                        }
                        if (content.size() > qc::max_upload_bytes) {
                            co_return co_await refuse(
                                {{"code", "cannot_prepare"},
                                 {"message",
                                  "The columns the model trains on are larger than Lake loads in "
                                  "one call (16 MiB). Train this dataset from Train → Models, or "
                                  "start the quickstart with a smaller file."}});
                        }
                        std::string table = text(record["dataset"], "name") + "_training";
                        if (table.size() > 63) {
                            table = text(record["dataset"], "name").substr(0, 54) + "_training";
                        }
                        const ApiResult uploaded =
                            take(co_await nested(context,
                                                 "upload_dataset",
                                                 {{"customer_id", customer_id},
                                                  {"workspace_id", workspace_id},
                                                  {"content", std::move(content)},
                                                  {"format", format},
                                                  {"table_name", table},
                                                  {"if_exists", "rename"}}));
                        if (!uploaded.ok) {
                            if (uploaded.error.code == "payment_required") {
                                co_return uploaded;
                            }
                            co_return co_await refuse(error_json(uploaded.error));
                        }
                        const std::string database = text(uploaded.value, "database").empty()
                                                         ? std::string("datasets")
                                                         : text(uploaded.value, "database");
                        const std::string copied = text(uploaded.value, "table").empty()
                                                       ? table
                                                       : text(uploaded.value, "table");
                        dataset_path = database + "." + copied;
                        preparation = {
                            {"methodology", methodology},
                            {"copy", true},
                            {"dataset_id", datasets::id_of(database, copied)},
                            {"format", format},
                            {"entity", prepared.entity},
                            {"used", prepared.used},
                            {"dropped", prepared.dropped},
                            {"label", prepared.label},
                            {"rows", prepared.rows},
                            {"skipped", prepared.skipped},
                            {"note", prepared.note.empty() ? json(nullptr) : json(prepared.note)},
                            {"applied", applied}};
                        train["entity_column"] = prepared.entity;
                    }
                    train["dataset_path"] = dataset_path;
                    started = take(co_await nested(context, "train_model", std::move(train)));
                    if (started->ok || started->error.code == "payment_required") {
                        break;
                    }
                    const auto refused = qc::refusal(started->error.message);
                    if (refused && pass == 0) {
                        if (const auto fix = qc::fix_for(*refused, usable, choice)) {
                            // Lake can satisfy it: the training copy does, once.
                            fixes.drop.insert(fix->drop.begin(), fix->drop.end());
                            fixes.dates_from_values |= fix->dates_from_values;
                            fixes.number_rows |= fix->number_rows;
                            applied.push_back(refused->name);
                            continue;
                        }
                    }
                    if (refused) {
                        co_return co_await refuse(
                            {{"code", "training_refused"},
                             {"message", qc::explain_refusal(*refused, usable, choice)},
                             {"transform", refused->name}});
                    }
                    break;
                }
            }
        }
        if (!started && qc::label_free(methodology)) {
            // Segmentation / clustering (M-3, lake#1182): nothing to predict, so no target. The
            // engine keys the segments by the column that names a row; without one that is unique
            // Lake writes a numbered training copy of the usable columns.
            json detected = record["dataset"].value("columns", json::array());
            if (!qc::detected(detected)) {
                auto first = take(co_await analyze(
                    context, customer_id, workspace_id, record["dataset"], as_of, {}));
                if (first) {
                    detected = qc::detect_columns(*first, detected);
                    record["dataset"]["columns"] = detected;
                }
            }
            if (choice.entity_column.empty() && !input.contains("entity_column") &&
                qc::detected(detected)) {
                const json suggested = qc::suggest(detected);
                if (suggested["entity_column"].is_string()) {
                    choice.entity_column = suggested["entity_column"];
                }
            }
            choice.target.clear();
            choice.target_value.clear();
            // lake#1203: segmentation has no held-out share (zetta refuses `holdout_fraction` for
            // M-3), and the engine chooses the number of segments itself, between its own
            // minimum_k and maximum_k (2 to 8) and never more than the rows. Lake checks the rows
            // can make two segments, and sends nothing else.
            if (const std::string problem =
                    qc::clustering_problem(record["dataset"].value("rows", json(nullptr)));
                !problem.empty()) {
                co_return co_await refuse({{"code", "cannot_prepare"}, {"message", problem}});
            }
            json train{{"customer_id", customer_id},
                       {"workspace_id", workspace_id},
                       {"name", name},
                       {"methodology", methodology}};
            std::optional<std::vector<json>> rows;
            if (qc::detected(detected)) {
                auto read =
                    take(co_await read_rows(context, customer_id, workspace_id, record["dataset"]));
                if (!read) {
                    co_return co_await refuse(
                        {{"code", "cannot_prepare"}, {"message", read.error()}});
                }
                rows = std::move(*read);
            }
            qc::Prepare made =
                rows ? qc::prepare_unlabeled(detected, *rows, choice.entity_column) : qc::Prepare{};
            if (rows && !made.prepared) {
                co_return co_await refuse({{"code", "cannot_prepare"}, {"message", made.problem}});
            }
            if (made.prepared && !made.prepared->copy) {
                // A unique column names every row and every other column is an input as it
                // stands: the dataset trains as it is. Anything Lake left out (a row number, an
                // identifier, free text) or derived (a date's parts) needs the training copy,
                // since the engine groups by every column it is given (lake#1242).
                preparation = {{"methodology", methodology},
                               {"copy", false},
                               {"dataset_id", record["dataset"]["id"]},
                               {"entity", choice.entity_column},
                               {"used", made.prepared->used},
                               {"dropped", made.prepared->dropped},
                               {"derived", made.prepared->derived},
                               {"label", nullptr},
                               {"applied", json::array()}};
                train["dataset_path"] = source_path;
                train["entity_column"] = choice.entity_column;
            } else if (made.prepared) {
                const qc::Prepared& prepared = *made.prepared;
                std::string format = "jsonl";
                std::string content = prepared.content;
                if (content.size() > qc::max_upload_bytes) {
                    format = "csv";
                    content = qc::csv_content(prepared);
                }
                if (content.size() > qc::max_upload_bytes) {
                    co_return co_await refuse(
                        {{"code", "cannot_prepare"},
                         {"message",
                          "The columns the model groups rows by are larger than Lake loads in "
                          "one call (16 MiB). Train this dataset from Train → Models, or start "
                          "the quickstart with a smaller file."}});
                }
                std::string table = text(record["dataset"], "name") + "_training";
                if (table.size() > 63) {
                    table = text(record["dataset"], "name").substr(0, 54) + "_training";
                }
                const ApiResult uploaded = take(co_await nested(context,
                                                                "upload_dataset",
                                                                {{"customer_id", customer_id},
                                                                 {"workspace_id", workspace_id},
                                                                 {"content", std::move(content)},
                                                                 {"format", format},
                                                                 {"table_name", table},
                                                                 {"if_exists", "rename"}}));
                if (!uploaded.ok) {
                    if (uploaded.error.code == "payment_required") {
                        co_return uploaded;
                    }
                    co_return co_await refuse(error_json(uploaded.error));
                }
                const std::string database = text(uploaded.value, "database").empty()
                                                 ? std::string("datasets")
                                                 : text(uploaded.value, "database");
                const std::string copied =
                    text(uploaded.value, "table").empty() ? table : text(uploaded.value, "table");
                dataset_path = database + "." + copied;
                preparation = {
                    {"methodology", methodology},
                    {"copy", true},
                    {"dataset_id", datasets::id_of(database, copied)},
                    {"format", format},
                    {"entity", prepared.entity},
                    {"used", prepared.used},
                    {"dropped", prepared.dropped},
                    {"derived", prepared.derived},
                    {"label", nullptr},
                    {"rows", prepared.rows},
                    {"skipped", prepared.skipped},
                    {"note", prepared.note.empty() ? json(nullptr) : json(prepared.note)},
                    {"applied", json::array()}};
                train["dataset_path"] = dataset_path;
                train["entity_column"] = prepared.entity;
                choice.entity_column = prepared.entity;
            } else {
                // The engine could not profile the dataset: trained as it is.
                train["dataset_path"] = source_path;
                if (!choice.entity_column.empty()) {
                    train["entity_column"] = choice.entity_column;
                }
            }
            started = take(co_await nested(context, "train_model", std::move(train)));
        }
        if (!started) {
            // Another methodology, or an engine that cannot profile the dataset: trained as it
            // is, with the columns sent.
            json train{{"customer_id", customer_id},
                       {"workspace_id", workspace_id},
                       {"name", name},
                       {"methodology", methodology},
                       {"dataset_path", source_path}};
            if (!qc::label_free(methodology)) {
                train["holdout_fraction"] = setting("holdout_fraction", default_holdout);
            }
            if (!choice.target.empty()) {
                train["target"] = choice.target;
            }
            if (!choice.entity_column.empty()) {
                train["entity_column"] = choice.entity_column;
            }
            started = take(co_await nested(context, "train_model", std::move(train)));
        }
        if (!started->ok) {
            if (started->error.code == "payment_required") {
                co_return *started;
            }
            // In plain words for the person (lake#1204); the engine's own reason is the detail.
            co_return co_await refuse(
                {{"code", started->error.code},
                 {"message", qc::explain_run_failure(started->error.message, choice, methodology)},
                 {"detail", started->error.message}});
        }
        json training{{"name", name},
                      {"methodology", methodology},
                      {"target", choice.target.empty() ? json(nullptr) : json(choice.target)},
                      {"target_value",
                       choice.target_value.empty() ? json(nullptr) : json(choice.target_value)},
                      {"entity_column",
                       choice.entity_column.empty() ? json(nullptr) : json(choice.entity_column)},
                      {"holdout_fraction",
                       qc::label_free(methodology) ? json(nullptr)
                                                   : setting("holdout_fraction", default_holdout)},
                      {"as_of", as_of},
                      {"dataset_path", dataset_path},
                      {"preparation", preparation},
                      {"derived_target", derived_kept},
                      {"attempt", attempt}};
        training["run_id"] = text(started->value, "run_id");
        training["started_at"] = now_text();
        record["training"] = std::move(training);
        clear_error(record, "model");
        if (auto failure = take(co_await save())) {
            co_return std::move(*failure);
        }
        context.audit_details["run_id"] = record["training"]["run_id"];
        progress = {{"state", "running"},
                    {"status",
                     text(started->value, "status").empty() ? json("running")
                                                            : json(text(started->value, "status"))},
                    {"model_id", nullptr},
                    {"error", nullptr},
                    {"started_at", record["training"]["started_at"]}};
        co_return ApiResult::success(status_of(record, progress));
    }
    // Trained. A segmentation / clustering model groups rows and answers no question per row:
    // the engine's predict serves only gradient-boosted models, so it is not published for the
    // website (lake#1182) and the quickstart is done.
    if (qc::label_free(text(record.value("training", json::object()), "methodology"))) {
        record["model"] = {{"id", model_id}, {"published", false}, {"serves", false}};
        clear_error(record, "model_answers");
        if (auto failure = take(co_await save())) {
            co_return std::move(*failure);
        }
        context.audit_details["model_id"] = model_id;
        co_return ApiResult::success(status_of(record, json(nullptr)));
    }
    // Trained: publish it, so the website can ask it.
    const ApiResult exposed = take(co_await nested(
        context,
        "expose_model",
        {{"customer_id", customer_id}, {"workspace_id", workspace_id}, {"model_id", model_id}}));
    if (!exposed.ok) {
        if (exposed.error.code == "payment_required") {
            co_return exposed;
        }
        set_error(record, "model_answers", error_json(exposed.error));
        if (auto failure = take(co_await save())) {
            co_return std::move(*failure);
        }
        co_return ApiResult::success(status_of(record, json(nullptr)));
    }
    record["model"] = {{"id", model_id}, {"published", true}};
    clear_error(record, "model_answers");
    if (auto failure = take(co_await save())) {
        co_return std::move(*failure);
    }
    context.audit_details["model_id"] = model_id;
    co_return ApiResult::success(status_of(record, json(nullptr)));
}

// ---- start_quickstart_conversation (lake#1182) ----

liquid::runtime::task<ApiResult> start_quickstart_conversation(const json& input,
                                                               ApiContext& context) {
    if (!context.store || !context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (context.acting_user_id.empty()) {
        co_return ApiResult::failure("unauthorized",
                                     "an Ask Orama conversation is held by a person");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    auto state = take(co_await workspace_state(context, customer_id, workspace_id));
    if (!state) {
        co_return std::move(state.error());
    }
    if (!state->value("ready", false)) {
        co_return ApiResult::failure("workspace_not_ready",
                                     "the workspace is not ready yet; read quickstart_status",
                                     "/workspace_id");
    }
    auto opened = workspace_records(context, customer_id, workspace_id);
    if (!opened) {
        co_return failed(opened.error());
    }
    auto loaded = take(
        co_await load(*opened, customer_id, input.value("quickstart_id", std::string()), false));
    if (!loaded) {
        co_return std::move(loaded.error());
    }
    if (!loaded->record) {
        co_return ApiResult::failure(
            "invalid_input",
            "this workspace has no quickstart yet; call start_quickstart first",
            "/quickstart_id");
    }
    json record = loaded->record->body;
    context.audit_details["quickstart_id"] = record.at("id");
    if (!present(record, "dataset")) {
        co_return ApiResult::failure(
            "invalid_input",
            "the quickstart has no dataset yet; call start_quickstart first",
            "/quickstart_id");
    }
    const auto answer = [&]() -> liquid::runtime::task<ApiResult> {
        const json progress =
            present(record, "training") && !present(record, "model")
                ? take(co_await training_progress(context, customer_id, workspace_id, record))
                : json(nullptr);
        co_return ApiResult::success(status_of(record, progress));
    };
    // Once: the conversation already open is answered, unless `restart` asks for a new one.
    if (present(record, "conversation") && !input.value("restart", false)) {
        co_return co_await answer();
    }
    std::string message = input.value("message", std::string());
    if (message.empty()) {
        message = opening_message(record);
    }
    // The same capability the Ask Orama page calls, as the same person: the conversation is kept
    // in this workspace's Zetta, and the turn runs under their own permissions and metering.
    json ask{{"customer_id", customer_id},
             {"workspace_id", workspace_id},
             {"message", message},
             {"page", conversation_page(record)}};
    if (const std::string agent = input.value("agent", std::string()); !agent.empty()) {
        ask["agent"] = agent;
    }
    const ApiResult sent = take(co_await nested(context, "send_ask_message", std::move(ask)));
    if (!sent.ok) {
        co_return sent;
    }
    record["conversation"] = {{"id", text(sent.value, "conversation_id")},
                              {"task_id", text(sent.value, "task_id")},
                              {"message_id", text(sent.value, "message_id")},
                              {"started_at", now_text()}};
    Saved saved(*opened, loaded->record->revision, loaded->record->lake_revision);
    if (auto written = take(co_await saved.save(record)); !written) {
        co_return failed(written.error());
    }
    context.audit_details["conversation_id"] = record["conversation"]["id"];
    co_return co_await answer();
}

// ---- quickstart_snippets ----

// lake's browser-facing origin, as the website calls it.
std::string public_origin(const ApiContext& context) {
    if (!context.external_origin.empty()) {
        return context.external_origin;
    }
    if (!context.public_host.empty()) {
        return "https://" + context.public_host;
    }
    return context.request_origin.empty() ? std::string("https://lake.example.com")
                                          : context.request_origin;
}

// A JSON value written into a page's script: valid JavaScript, and never closes the script.
std::string script_value(const json& value) {
    std::string out = value.dump();
    for (std::size_t at = out.find("</"); at != std::string::npos; at = out.find("</", at + 3)) {
        out.replace(at, 2, "<\\/");
    }
    return out;
}

std::string html_snippet(const std::string& origin,
                         const json& search,
                         const json& model,
                         const std::string& target,
                         const std::string& table) {
    std::string out =
        "<!-- Lake search over " + table +
        ": paste this into any web page. -->\n"
        "<div id=\"lake-search\">\n"
        "  <input id=\"lake-query\" type=\"search\" placeholder=\"Search " +
        table +
        "\" autocomplete=\"off\" />\n"
        "  <ol id=\"lake-hits\"></ol>\n"
        "</div>\n"
        "<script>\n"
        "(() => {\n"
        "  const LAKE = " +
        script_value(origin) +
        ";\n"
        "  // Anyone who opens this page can read the key: it can only search this index";
    out += model.is_null() ? "" : " and ask this model";
    out += ",\n"
           "  // and only from your website's address. Revoke it on the index's page in Lake.\n"
           "  const KEY = " +
           script_value(std::string(key_placeholder)) +
           ";\n"
           "  const SEARCH = " +
           script_value(search) +
           ";\n"
           "  const MODEL = " +
           script_value(model) +
           ";\n"
           "  const TARGET = " +
           script_value(target) +
           ";\n"
           "  const input = document.getElementById(\"lake-query\");\n"
           "  const list = document.getElementById(\"lake-hits\");\n"
           "  const headers = { Authorization: `Bearer ${KEY}` };\n"
           "  let timer;\n"
           "  input.addEventListener(\"input\", () => {\n"
           "    clearTimeout(timer);\n"
           "    timer = setTimeout(search, 250);\n"
           "  });\n"
           "  async function search() {\n"
           "    const query = input.value.trim();\n"
           "    if (!query) return list.replaceChildren();\n"
           "    const params = new URLSearchParams({ ...SEARCH, query, top_k: \"10\" });\n"
           "    const found = await fetch(`${LAKE}/v1/search_index?${params}`, { headers "
           "}).then((r) => r.json());\n"
           "    if (!found.ok) return (list.textContent = found.error.message);\n"
           "    const hits = found.value.hits;\n"
           "    const answers = [];\n"
           "    if (MODEL && hits.length) {\n"
           "      const rows = hits.map((hit) => ({ ...hit.fields }));\n"
           "      const scored = await fetch(`${LAKE}/v1/predict`, {\n"
           "        method: \"POST\",\n"
           "        headers: { ...headers, \"Content-Type\": \"application/json\" },\n"
           "        body: JSON.stringify({ ...MODEL, rows }),\n"
           "      }).then((r) => r.json());\n"
           "      if (scored.ok) for (const entry of scored.value.ranked) answers[entry.row] = "
           "entry.risk;\n"
           "    }\n"
           "    list.replaceChildren(\n"
           "      ...hits.map((hit, i) => {\n"
           "        const item = document.createElement(\"li\");\n"
           "        item.textContent = Object.values(hit.fields || {}).join(\" \\u00b7 \");\n"
           "        if (answers[i] !== undefined) {\n"
           "          const answer = document.createElement(\"strong\");\n"
           "          answer.textContent = ` \\u2014 ${TARGET}: ${answers[i].toFixed(2)}`;\n"
           "          item.append(answer);\n"
           "        }\n"
           "        return item;\n"
           "      }),\n"
           "    );\n"
           "  }\n"
           "})();\n"
           "</script>\n";
    return out;
}

std::string react_snippet(const std::string& origin,
                          const json& search,
                          const json& model,
                          const std::string& target) {
    std::string out = "// LakeSearch.jsx: a search box over your Lake index";
    out += model.is_null() ? "" : ", with your model's answer beside each result";
    out += ".\n"
           "import { useEffect, useState } from \"react\";\n\n"
           "const LAKE = " +
           script_value(origin) +
           ";\n"
           "// Anyone who opens the page can read the key: it can only search this index";
    out += model.is_null() ? "" : " and ask this model";
    out += ", and only from your website's address.\nconst KEY = " +
           script_value(std::string(key_placeholder)) +
           ";\n"
           "const SEARCH = " +
           script_value(search) +
           ";\n"
           "const MODEL = " +
           script_value(model) +
           ";\n"
           "const TARGET = " +
           script_value(target) +
           ";\n"
           "const headers = { Authorization: `Bearer ${KEY}` };\n\n"
           "export function LakeSearch() {\n"
           "  const [query, setQuery] = useState(\"\");\n"
           "  const [hits, setHits] = useState([]);\n"
           "  const [answers, setAnswers] = useState([]);\n"
           "  const [error, setError] = useState(\"\");\n"
           "  useEffect(() => {\n"
           "    const q = query.trim();\n"
           "    if (!q) {\n"
           "      setHits([]);\n"
           "      setAnswers([]);\n"
           "      return;\n"
           "    }\n"
           "    const controller = new AbortController();\n"
           "    const timer = setTimeout(async () => {\n"
           "      try {\n"
           "        const params = new URLSearchParams({ ...SEARCH, query: q, top_k: \"10\" });\n"
           "        const found = await fetch(`${LAKE}/v1/search_index?${params}`, { headers, "
           "signal: controller.signal }).then((r) => r.json());\n"
           "        if (!found.ok) return setError(found.error.message);\n"
           "        setError(\"\");\n"
           "        setHits(found.value.hits);\n"
           "        const next = [];\n"
           "        if (MODEL && found.value.hits.length) {\n"
           "          const scored = await fetch(`${LAKE}/v1/predict`, {\n"
           "            method: \"POST\",\n"
           "            headers: { ...headers, \"Content-Type\": \"application/json\" },\n"
           "            body: JSON.stringify({ ...MODEL, rows: found.value.hits.map((hit) => ({ "
           "...hit.fields })) }),\n"
           "            signal: controller.signal,\n"
           "          }).then((r) => r.json());\n"
           "          if (scored.ok) for (const entry of scored.value.ranked) next[entry.row] = "
           "entry.risk;\n"
           "        }\n"
           "        setAnswers(next);\n"
           "      } catch (e) {\n"
           "        if (e.name !== \"AbortError\") setError(String(e));\n"
           "      }\n"
           "    }, 250);\n"
           "    return () => {\n"
           "      clearTimeout(timer);\n"
           "      controller.abort();\n"
           "    };\n"
           "  }, [query]);\n"
           "  return (\n"
           "    <div>\n"
           "      <input type=\"search\" value={query} onChange={(e) => setQuery(e.target.value)} "
           "placeholder=\"Search\" />\n"
           "      {error && <p role=\"alert\">{error}</p>}\n"
           "      <ol>\n"
           "        {hits.map((hit, i) => (\n"
           "          <li key={hit.id}>\n"
           "            {Object.values(hit.fields ?? {}).join(\" \\u00b7 \")}\n"
           "            {answers[i] !== undefined && <strong> {\"\\u2014\"} {TARGET}: "
           "{answers[i].toFixed(2)}</strong>}\n"
           "          </li>\n"
           "        ))}\n"
           "      </ol>\n"
           "    </div>\n"
           "  );\n"
           "}\n";
    return out;
}

// `site` is the website's origin the key lists: a browser key answers only calls that name it in
// Origin, as a browser does, so curl names it too.
std::string curl_snippet(const std::string& origin,
                         const json& search,
                         const json& model,
                         const std::string& site) {
    std::string out = "export LAKE_SEARCH_KEY=" + std::string(key_placeholder) +
                      "\n"
                      "curl -s -G \"" +
                      origin +
                      "/v1/search_index\" \\\n"
                      "  -H \"Authorization: Bearer $LAKE_SEARCH_KEY\" -H \"Origin: " +
                      site + "\" \\\n";
    for (const char* field : {"customer_id", "workspace_id", "index_id"}) {
        out += "  --data-urlencode \"" + std::string(field) + "=" + text(search, field) + "\" \\\n";
    }
    out += "  --data-urlencode \"query=your words\"\n";
    if (!model.is_null()) {
        json body = model;
        body["rows"] = json::array({{{"<column>", "<value>"}}});
        out += "\ncurl -s \"" + origin +
               "/v1/predict\" \\\n"
               "  -H \"Authorization: Bearer $LAKE_SEARCH_KEY\" -H \"Origin: " +
               site +
               "\" \\\n"
               "  -H \"Content-Type: application/json\" \\\n"
               "  -d '" +
               body.dump() + "'\n";
    }
    return out;
}

// The prompt a person pastes into their coding agent (lake#1185): one self-contained block with the
// goal, the endpoint, ids and key, what the key can and cannot do, the response shapes, the source,
// how to fit it into the site, the origins, how to verify, and where Lake's tools are. The secret
// is in it only when this call made the key; otherwise it says how to make one.
struct PromptFacts {
    std::string origin;
    json search;
    json model;
    std::string table;
    json fields; // the index's fields
    json label;  // the model's question: {column, positive}, or null
    std::string secret;
    json made;         // the key this call made, or null
    json create_input; // the create_browser_key call
    std::string html;
    std::string react;
};

std::string agent_prompt(const PromptFacts& facts) {
    const bool with_model = !facts.model.is_null();
    const std::string key = facts.secret.empty() ? std::string(key_placeholder) : facts.secret;
    std::string question;
    if (with_model && facts.label.is_object()) {
        const std::string column = text(facts.label, "column");
        const std::string positive = text(facts.label, "positive");
        question = positive.empty() || positive == "1" ? column : column + " is " + positive;
    }
    std::string fields;
    for (const json& field : facts.fields) {
        if (field.is_string()) {
            fields += (fields.empty() ? "" : ", ") + field.get<std::string>();
        }
    }
    json origins = json::array();
    int rate = control::browser_key_default_rate_limit;
    if (facts.made.is_object()) {
        origins = facts.made.value("origins", json::array());
        rate = facts.made.value("rate_limit_per_minute", rate);
    }
    std::string allowed;
    for (const json& origin : origins) {
        allowed += "  - " + origin.get<std::string>() + "\n";
    }
    std::string out;
    out +=
        "# Add search" + std::string(with_model ? " and model answers" : "") + " to my website\n\n";
    out += "You are working in my website's code. Add a search box over my Lake search index " +
           (facts.table.empty() ? std::string("of my dataset")
                                : "of my dataset `" + facts.table + "`") +
           (with_model ? ", and beside each result show my model's answer: how likely " +
                             (question.empty() ? std::string("its outcome") : question) +
                             " is for that row, from 0 to 1."
                       : ".") +
           " Put it where a visitor would look for search, built the way the rest of the site is "
           "built (its framework, components and styles), not pasted as a foreign block.\n\n";

    out += "## What to call\n\n";
    out += "- Lake: " + facts.origin + "\n";
    out += "- Search: `GET " + facts.origin +
           "/v1/search_index` with `customer_id=" + text(facts.search, "customer_id") +
           "`, `workspace_id=" + text(facts.search, "workspace_id") +
           "`, `index_id=" + text(facts.search, "index_id") +
           "`, `query=<what the visitor typed>`, optional `top_k` (1 to 100, default 10).\n";
    if (with_model) {
        out += "- Model answers: `POST " + facts.origin +
               "/v1/predict` with JSON `{\"customer_id\": \"" + text(facts.model, "customer_id") +
               "\", \"workspace_id\": \"" + text(facts.model, "workspace_id") +
               "\", \"model_id\": \"" + text(facts.model, "model_id") +
               "\", \"rows\": [<each hit's fields>]}`.\n";
    }
    out += "- Every call sends `Authorization: Bearer " + key + "`.";
    if (!fields.empty()) {
        out += " The index's fields: " + fields + ".";
    }
    out += "\n\n";

    out += "## The key\n\n";
    if (facts.secret.empty()) {
        out += "The code below has `" + std::string(key_placeholder) +
               "` where the key goes. I have not made it yet: ask me to make a website key in "
               "Lake (Quickstart step 3, or Ship → Search Index → Website Keys) for this site's "
               "address, or call `create_browser_key` with this input, changing `origins` to the "
               "site's origins:\n\n```json\n" +
               facts.create_input.dump(2) +
               "\n```\n\nIts `secret` is shown once; put it where the code says " +
               std::string(key_placeholder) + ".\n\n";
    } else {
        out += "The key is already in the code below.\n\n";
    }
    out += "It is a browser key, made to sit in a public web page: keep it in the client code, "
           "never move it behind a server or treat it as a secret. It can only search this "
           "index" +
           std::string(with_model ? " and ask this model" : "") +
           ", it changes nothing, it answers only pages served from the origins it lists, and it "
           "makes at most " +
           std::to_string(rate) + " calls a minute. Every search" +
           std::string(with_model ? " and answer" : "") +
           " is billed to my Lake account. I can revoke it on the index's page in Lake.\n\n";

    out += "## Allowed origins\n\n";
    if (!allowed.empty()) {
        out += "The key answers only pages served from:\n\n" + allowed + "\n";
    } else {
        out += "The key answers only the origins it is made for (`https://host[:port]`; "
               "`http://` only for localhost).\n\n";
    }
    out += "If the site runs anywhere else (another domain, a preview URL, `localhost` while you "
           "develop), a call from there is refused and the browser reports a CORS error. Do not "
           "work around it: make another key for that origin (Ship → Search Index → Website Keys "
           "in Lake, or `create_browser_key`).\n\n";

    out += "## What Lake answers\n\n";
    out += "Every answer is JSON. Success: `{\"ok\": true, \"value\": ...}`. Refusal: "
           "`{\"ok\": false, \"error\": {\"code\": \"...\", \"message\": \"...\"}}`.\n\n";
    out +=
        "`search_index` value:\n\n```json\n"
        "{\"hits\": [{\"id\": \"<row id>\", \"score\": 3.21, \"fields\": {\"<column>\": "
        "\"<value>\"}}]}\n```\n\nHits are ranked best first; `fields` holds the row's columns.\n\n";
    if (with_model) {
        out += "`predict` value:\n\n```json\n"
               "{\"model_id\": \"" +
               text(facts.model, "model_id") +
               "\", \"rows\": 2, \"ranked\": [{\"row\": 0, \"risk\": 0.87}, {\"row\": 1, "
               "\"risk\": 0.12}]}\n```\n\n`row` is the position in the `rows` you sent; `risk` is "
               "the model's answer for it, from 0 to 1. Send the hits' `fields` as `rows`, in "
               "order.\n\n";
    }

    out += "## How to build it\n\n";
    out += "1. Find where search belongs in this site (a header, a search page, a listing) and "
           "use the site's own framework, components and styles.\n";
    out += "2. Search as the visitor types, waiting about 250 ms after the last keystroke, and "
           "cancel a search that a newer one replaces.\n";
    out += "3. Show each hit's useful fields, not raw JSON." +
           std::string(with_model ? " Beside each, show the model's answer (for example as a "
                                    "percentage), labelled in words the visitor understands."
                                  : "") +
           "\n";
    out += "4. Handle every state: nothing typed, searching, no results (say so), an error "
           "(show `error.message` quietly, keep the box usable), and `429 rate_limited` (too many "
           "calls this minute: wait and try again, never retry in a loop).\n";
    out += "5. Keep the key in the client code as it is.\n\n";

    out += "## The source\n\nA plain web page (HTML and JavaScript):\n\n```html\n" + facts.html +
           "```\n\nThe same as a React component:\n\n```jsx\n" + facts.react + "```\n\n";

    out += "## How to check it works\n\n";
    out += "1. From the site's origin, a search returns hits:\n\n```shell\ncurl -s -G \"" +
           facts.origin + "/v1/search_index\" -H \"Authorization: Bearer " + key +
           "\" -H \"Origin: <the site's origin>\" -D - \\\n  --data-urlencode \"customer_id=" +
           text(facts.search, "customer_id") +
           "\" --data-urlencode \"workspace_id=" + text(facts.search, "workspace_id") +
           "\" --data-urlencode \"index_id=" + text(facts.search, "index_id") +
           "\" --data-urlencode \"query=<a word in the data>\"\n"
           "```\n\n";
    out += "2. The answer's headers include `Access-Control-Allow-Origin: <the site's origin>`; "
           "without it the origin is not one the key lists.\n";
    out += std::string("3. In the browser, typing in the box shows results") +
           (with_model ? ", each with the model's answer beside it." : ".") + "\n\n";

    out += "## More from Lake\n\n";
    out += "- MCP server: " + facts.origin + "/mcp (the tools `search_index`" +
           std::string(with_model ? " and `predict`" : "") + ", for an agent that uses tools)\n";
    out += "- Everything Lake can do, for agents: " + facts.origin + "/llms.txt\n";
    return out;
}

liquid::runtime::task<ApiResult> quickstart_snippets(const json& input, ApiContext& context) {
    if (!context.store || !context.workspaces) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    auto opened = workspace_records(context, customer_id, workspace_id);
    if (!opened) {
        co_return failed(opened.error());
    }
    auto loaded = take(
        co_await load(*opened, customer_id, input.value("quickstart_id", std::string()), false));
    if (!loaded) {
        co_return std::move(loaded.error());
    }
    if (!loaded->record) {
        co_return ApiResult::failure(
            "invalid_input",
            "this workspace has no quickstart yet; call start_quickstart first",
            "/quickstart_id");
    }
    const json& record = loaded->record->body;
    const json index = record.value("index", json(nullptr));
    const json model_record = record.value("model", json(nullptr));
    const std::string origin = public_origin(context);
    json out{{"quickstart_id", record.at("id")},
             {"customer_id", customer_id},
             {"workspace_id", workspace_id},
             {"origin", origin},
             {"ready", false},
             {"includes_model", false},
             {"snippets", json::array()},
             {"key", nullptr},
             {"mcp", {{"endpoint", origin + "/mcp"}, {"tools", json::array({"search_index"})}}}};
    if (!index.is_object() || !index.value("published", false)) {
        out["next"] = "The search index is not published yet: call start_quickstart with this "
                      "quickstart_id first.";
        co_return ApiResult::success(std::move(out));
    }
    const json search{{"customer_id", customer_id},
                      {"workspace_id", workspace_id},
                      {"index_id", text(index, "id")}};
    json model = nullptr;
    if (model_record.is_object() && model_record.value("published", false)) {
        model = {{"customer_id", customer_id},
                 {"workspace_id", workspace_id},
                 {"model_id", text(model_record, "id")}};
        out["includes_model"] = true;
        out["mcp"]["tools"].push_back("predict");
    }
    const std::string target = text(record.value("training", json(nullptr)), "target");
    const std::string table = text(record.value("dataset", json(nullptr)), "name");
    json capabilities = json::array({"search_index"});
    if (!model.is_null()) {
        capabilities.push_back("predict");
    }
    // The browser key the code needs (lake#1152): bound to this index (and model), this workspace
    // and the website's origins. With `origins` it is made here, through create_browser_key as the
    // same caller (its own authorization and audit), and its secret answered once.
    json create_input{{"customer_id", customer_id},
                      {"workspace_id", workspace_id},
                      {"index_id", text(index, "id")},
                      {"name", "website " + (table.empty() ? std::string("search") : table)},
                      {"origins", json::array({std::string(example_site)})}};
    if (!model.is_null()) {
        create_input["model_id"] = text(model_record, "id");
    }
    std::string secret;
    std::string site = std::string(example_site);
    json made = nullptr;
    if (const auto origins = input.find("origins"); origins != input.end()) {
        create_input["origins"] = *origins;
        ApiResult created = take(co_await nested(context, "create_browser_key", create_input));
        if (!created.ok) {
            co_return created;
        }
        secret = created.value.value("secret", std::string());
        made = created.value.at("key");
        site = made.at("origins").at(0).get<std::string>();
    }
    out["ready"] = true;
    out["key"] = {
        {"kind", "browser_key"},
        {"placeholder", key_placeholder},
        {"capabilities", capabilities},
        {"create", {{"capability", "create_browser_key"}, {"input", create_input}}},
        {"secret", secret.empty() ? json(nullptr) : json(secret)},
        {"browser_key", made},
        {"note",
         std::string(
             secret.empty()
                 ? "Make the key with create_browser_key, naming your website's origins "
                   "(or call quickstart_snippets again with `origins`): its `secret` is "
                   "shown once and goes where the code says " +
                       std::string(key_placeholder) + ". "
                 : "The key is in the code below and is shown only this once: copy it now. ") +
             "It can only search this index" +
             (model.is_null() ? std::string() : std::string(" and ask this model")) +
             ", only from the websites it lists, and never change anything. It belongs to the "
             "workspace, not to your sign-in, so signing out leaves your website working; it "
             "lasts until you revoke it (list_browser_keys, revoke_browser_key, or the index's "
             "page in Lake). Every search and model answer is billed to the account."}};
    const auto with_secret = [&](std::string code) {
        if (secret.empty()) {
            return code;
        }
        for (std::size_t at = code.find(key_placeholder); at != std::string::npos;
             at = code.find(key_placeholder, at + secret.size())) {
            code.replace(at, key_placeholder.size(), secret);
        }
        return code;
    };
    out["snippets"] = json::array(
        {{{"id", "html"},
          {"title", "Plain Web Page"},
          {"language", "html"},
          {"code",
           with_secret(
               html_snippet(origin, search, model, target.empty() ? "model" : target, table))}},
         {{"id", "react"},
          {"title", "React"},
          {"language", "jsx"},
          {"code",
           with_secret(react_snippet(origin, search, model, target.empty() ? "model" : target))}},
         {{"id", "curl"},
          {"title", "curl"},
          {"language", "shell"},
          {"code", with_secret(curl_snippet(origin, search, model, site))}}});
    PromptFacts facts{origin,
                      search,
                      model,
                      table,
                      index.value("fields", json::array()),
                      nullptr,
                      secret,
                      made,
                      create_input,
                      out["snippets"][0]["code"],
                      out["snippets"][1]["code"]};
    if (const json training = record.value("training", json(nullptr)); training.is_object()) {
        const json preparation = training.value("preparation", json(nullptr));
        facts.label =
            preparation.is_object() && preparation.value("label", json(nullptr)).is_object()
                ? preparation["label"]
                : json{{"column", target}, {"positive", text(training, "target_value")}};
    }
    out["prompt"] = agent_prompt(facts);
    out["next"] = !secret.empty() ? "Paste a snippet into your site: the key is already in it."
                  : model.is_null()
                      ? "Make the key (quickstart_snippets with `origins`, or create_browser_key) "
                        "and paste a snippet into your site. train_quickstart adds model answers."
                      : "Make the key (quickstart_snippets with `origins`, or create_browser_key) "
                        "and paste a snippet into your site: it shows search results with the "
                        "model's answer beside each.";
    co_return ApiResult::success(std::move(out));
}

json status_output_schema() {
    const json step_schema{
        {"type", "object"},
        {"properties",
         {{"step", {{"type", "string"}}},
          {"title", {{"type", "string"}}},
          {"state",
           {{"type", "string"}, {"enum", {"pending", "running", "done", "failed", "skipped"}}}},
          {"note", {{"type", "string"}}},
          {"error", {{"type", json::array({"object", "null"})}}},
          {"progress", {{"type", json::array({"object", "null"})}}}}},
        {"required", json::array({"step", "title", "state", "error"})}};
    return {
        {"type", "object"},
        {"properties",
         {{"quickstart_id", {{"type", json::array({"string", "null"})}}},
          {"customer_id", {{"type", "string"}}},
          {"workspace_id", {{"type", "string"}}},
          {"state",
           {{"type", "string"},
            {"enum",
             {"waiting_for_workspace",
              "not_started",
              "in_progress",
              "search_ready",
              "training",
              "trained",
              "done",
              "failed"}}}},
          {"next", {{"type", "string"}}},
          {"steps", {{"type", "array"}, {"items", step_schema}}},
          {"dataset", {{"type", json::array({"object", "null"})}}},
          {"index", {{"type", json::array({"object", "null"})}}},
          {"training", {{"type", json::array({"object", "null"})}}},
          {"model", {{"type", json::array({"object", "null"})}}},
          {"search", {{"type", json::array({"object", "null"})}}},
          {"predict", {{"type", json::array({"object", "null"})}}},
          {"conversation",
           {{"type", json::array({"object", "null"})},
            {"description",
             "the Ask Orama conversation about this dataset that decides what to build "
             "(start_quickstart_conversation, lake#1182): {conversation_id, task_id, "
             "started_at, page, read, reply}; `read` is the get_ask_conversation call, `reply` "
             "the send_ask_message call (with `page`) that continues it; null until it is "
             "started"}}},
          {"snapshot_date",
           {{"type", json::array({"string", "null"})},
            {"description",
             "the date the dataset's dates are read as of: when it was uploaded unless "
             "train_quickstart's `as_of` changed it"}}},
          {"suggested",
           {{"type", json::array({"object", "null"})},
            {"description",
             "{target, target_value, entity_column}: the column to predict (and the value to look "
             "for) and the column that names a row the quickstart offers first"}}}}},
        {"required",
         json::array({"quickstart_id", "customer_id", "workspace_id", "state", "next", "steps"})}};
}

json quickstart_id_schema() {
    return string_schema("a quickstart of this workspace (quickstart_status lists them); the "
                         "newest when left out");
}

json workspace_schema(std::string_view description) {
    return string_schema(description);
}

} // namespace

void register_quickstart_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "start_quickstart",
        .title = "Start the quickstart",
        .description =
            "The quickstart process, first steps (lake#1128): a dataset becomes a published "
            "search index a website can query. Without `workspace_id` it uses the account's first "
            "workspace and makes one when there is none (start_workspace); while that workspace "
            "starts it answers state `waiting_for_workspace` and keeps nothing, so call it again "
            "with the same input once quickstart_status reports the workspace ready. Then it adds "
            "the dataset (the file's `content` with `format` csv or jsonl, through upload_dataset; "
            "or an existing `dataset_id`), builds its search index (create_index) and publishes it "
            "(expose_index). It reads each column's kind from the engine's own profile "
            "(analyze_dataset): number, category, yes or no, free text, date or time, "
            "identifier, empty or the same in every row, in `dataset.columns` with what the "
            "first model can use and predict. Each step's state is kept in the workspace; a step "
            "that fails is "
            "answered with why (state `failed`), and calling again with the `quickstart_id` "
            "resumes from it. Paid: refused 402 payment_required with the top-up link while the "
            "account's balance is blocked. Next: quickstart_snippets for the website code, "
            "train_quickstart for model answers.",
        .input_schema = input_schema(
            {{"workspace_id",
              workspace_schema("the workspace to use; left out, the account's first, made when "
                               "it has none")},
             {"quickstart_id",
              string_schema("resume this quickstart instead of starting a new one")},
             {"content",
              string_schema("the dataset file's text, at most 16 MiB (send it or dataset_id)")},
             {"format",
              {{"type", "string"},
               {"enum", {"csv", "jsonl"}},
               {"description", "how `content` is laid out: csv with a header row, or jsonl"}}},
             {"name",
              string_schema("the dataset's name (letters, digits and underscores; default "
                            "quickstart, the next free name when it is taken)")},
             {"dataset_id",
              string_schema("an existing dataset, <database>/<name> as list_datasets answers it, "
                            "instead of content")},
             {"fields",
              {{"type", "array"},
               {"items", {{"type", "string"}, {"minLength", 1}}},
               {"minItems", 1},
               {"description", "the columns to search (default: every column)"}}}},
            json::array()),
        .output_schema = status_output_schema(),
        .kind = CapabilityKind::Action,
        .max_body_bytes = maximum_capability_body_bytes,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = start_quickstart,
    });
    registry.add(Capability{
        .name = "quickstart_status",
        .title = "Quickstart status",
        .description =
            "Where the quickstart process stands in a workspace (lake#1128): each step in order "
            "(workspace, dataset, search_index, website, model, model_answers) with its state "
            "(pending, running, done or failed, with why), the dataset with each column's "
            "detected kind (`dataset.columns`: kind, title, feature {use, reason}, predict {can, "
            "reason, values}), `snapshot_date`, `suggested` (the column to predict and the column "
            "that names a row offered first), the dataset, index and model ids, the "
            "training's progress read live from its run, the `search_index` and `predict` calls "
            "that reach what it made, and `next`, what to call next. `quickstart_id` picks one; "
            "the newest otherwise. `quickstarts` lists the workspace's quickstarts, newest first. "
            "With `include_costs`, `costs` says what each paid step is billed by at the price list "
            "in force and the account's prepaid balance (where Lake bills).",
        .input_schema = input_schema(
            {{"workspace_id", workspace_schema("the workspace the quickstart is in")},
             {"quickstart_id", quickstart_id_schema()},
             {"include_costs",
              {{"type", "boolean"},
               {"description", "also answer what the paid steps cost and the balance"}}}},
            json::array({"workspace_id"})),
        .output_schema = status_output_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = quickstart_status,
    });
    registry.add(Capability{
        .name = "train_quickstart",
        .title = "Train the quickstart's model",
        .description =
            "The quickstart process, model steps (lake#1128): trains a standalone model on the "
            "quickstart's dataset (train_model; methodology M-1 and a 0.2 held-out share unless "
            "told otherwise, `target` the column to predict, `entity_column` the column that names "
            "a row), and once the run has finished, publishes the model (expose_model) so the "
            "website can ask it. When no column is a yes-or-no answer, `derived_target` derives "
            "one from another column (lake#1182: price above 50, a band, or the values that count "
            "as yes). Methodology M-3 (segmentation / clustering) groups rows without a target; "
            "its model is trained but not published for the website, because predict serves only "
            "models that answer a question per row. Any column types train (lake#1181): M-1 "
            "answers a yes-or-no "
            "question, so `target` is a column with two values or a category with `target_value` "
            "the value to look for (the most common unless sent); left out, the columns "
            "quickstart_status `suggested` names. Date and time columns are read as of "
            "`as_of` (default: the quickstart's snapshot date, when the dataset was uploaded) and "
            "left out of the first model with identifiers, free text, empty columns and columns "
            "with one value; when the dataset cannot train as it is, Lake writes the usable "
            "columns, a row number when no column names a row, and the target as 0/1 to a "
            "training copy (upload_dataset, `<name>_training`) and trains on that. A "
            "`transform_required` refusal Lake can satisfy is applied and tried once more; any "
            "other is answered on the model step in plain words with what to choose. Call it "
            "once to start training, read quickstart_status for "
            "progress, then call it again to publish; while the run is still going it changes "
            "nothing. A failed run is trained again by the next call, with the settings sent then "
            "or the ones before. Paid: training runs on the workspace engine's billed compute; "
            "refused 402 payment_required with the top-up link while the balance is blocked.",
        .input_schema = input_schema(
            {{"workspace_id", workspace_schema("the workspace the quickstart is in")},
             {"quickstart_id", quickstart_id_schema()},
             {"target",
              string_schema("the column to predict: with M-1 a column with two values, or a "
                            "category with target_value (default: quickstart_status "
                            "`suggested`)")},
             {"target_value",
              string_schema("the value of `target` the model looks for: its answer is how "
                            "likely a row has it (default: a yes-like value, else the rarer of "
                            "two, else the category's most common)")},
             {"as_of",
              string_schema("the snapshot date the dataset's dates are read as of, such as "
                            "2026-10-24 or an ISO-8601 time (default: the quickstart's, when the "
                            "dataset was uploaded); kept on the quickstart")},
             {"entity_column",
              string_schema("the column that identifies a row (default: a detected identifier; "
                            "without one Lake numbers the rows)")},
             {"derived_target",
              {{"type", "object"},
               {"description",
                "a yes-or-no column to predict derived from another column, when none is one "
                "(lake#1182): {column, above, at_least, below, at_most, values, name}. A numeric "
                "rule (above 50; or the band at_least 20 and below 50) reads a number column; "
                "`values` (the values that count as yes) reads any column. Lake writes the "
                "training copy with the derived column (yes or no, `name`, default the rule such "
                "as price_above_50) and leaves the column it is derived from out of the inputs. "
                "Replaces `target`"},
               {"properties",
                {{"column", {{"type", "string"}, {"minLength", 1}}},
                 {"above", {{"type", "number"}}},
                 {"at_least", {{"type", "number"}}},
                 {"below", {{"type", "number"}}},
                 {"at_most", {{"type", "number"}}},
                 {"values",
                  {{"type", "array"},
                   {"items", {{"type", "string"}, {"minLength", 1}}},
                   {"minItems", 1}}},
                 {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 63}}}}},
               {"required", json::array({"column"})},
               {"additionalProperties", false}}},
             {"methodology",
              string_schema("a methodology id from list_methodologies (default M-1, tabular "
                            "supervised learning, a yes-or-no question); M-3 segmentation / "
                            "clustering groups rows without a column to predict, and its model "
                            "is not published for the website")},
             {"holdout_fraction",
              {{"type", "number"},
               {"exclusiveMinimum", 0},
               {"exclusiveMaximum", 1},
               {"description", "rows held out to score the model honestly (default 0.2)"}}},
             {"name",
              string_schema("the model's name (default: the dataset's, with the "
                            "quickstart's id)")}},
            json::array({"workspace_id"})),
        .output_schema = status_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = train_quickstart,
    });
    registry.add(Capability{
        .name = "start_quickstart_conversation",
        .title = "Talk through what to build",
        .description =
            "The quickstart process, model step (lake#1182): opens an Ask Orama conversation about "
            "the quickstart's dataset, so the person (or an agent acting for them) decides what is "
            "worth building on it. The first turn carries the dataset's name, row count, each "
            "column with its detected kind and whether it can be predicted, and the search index "
            "(`conversation.page`), so Ask Orama starts from what this dataset could become. It "
            "proposes a model (train_quickstart, including a target derived from a column, such "
            "as price above 50, or segmentation / clustering with methodology M-3), opportunities "
            "from the Auto-Suggest Agent, an app such as an analysis dashboard from the "
            "Application Agent, an "
            "analysis in the conversation, or a generated corpus or labels first; nothing runs "
            "until the person confirms it with confirm_ask_proposal, and confirming a model "
            "carries the quickstart on. Kept in the workspace Zetta like any Ask Orama "
            "conversation; quickstart_status names it in `conversation` with the calls that read "
            "and continue it. Calling it again answers the open conversation; `restart` starts a "
            "new one. `message` is the person's first message (default: what could I build with "
            "this dataset). Ask Orama's model tokens are metered against the account.",
        .input_schema = input_schema(
            {{"workspace_id", workspace_schema("the workspace the quickstart is in")},
             {"quickstart_id", quickstart_id_schema()},
             {"message",
              {{"type", "string"},
               {"minLength", 1},
               {"maxLength", 4000},
               {"description",
                "the person's first message (default: what could I build with this dataset, and "
                "which ideas are the most interesting)"}}},
             {"restart",
              {{"type", "boolean"},
               {"description", "start a new conversation even when one is open"}}},
             {"agent",
              string_schema("the chat agent to talk to, as send_ask_message's `agent` (default "
                            "agent-ask-orama)")}},
            json::array({"workspace_id"})),
        .output_schema = status_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = start_quickstart_conversation,
    });
    registry.add(Capability{
        .name = "quickstart_snippets",
        .title = "Quickstart website snippets",
        .description =
            "The website instructions of the quickstart process (lake#1128): code for a plain web "
            "page, a React component and curl that search the quickstart's published index, with "
            "the model's answer beside each result once train_quickstart has published it, with "
            "Lake's host and the account, workspace, index and model ids filled in. The code needs "
            "a browser key (lake#1152): one bound to this index (and model), this workspace and "
            "your website's address, that works only from there and outlives your sign-in. Send "
            "`origins` (such as [\"https://www.example.com\"]) and this call makes it through "
            "create_browser_key and answers it once, in `key.secret` and already in the code; "
            "without `origins` it makes nothing and `key.create` is the create_browser_key call "
            "whose `secret` goes where the code says YOUR_LAKE_SEARCH_KEY. Anyone who opens the "
            "page can read the key; it can only search and ask, and every call is billed to the "
            "account. `prompt` is the same as one block to paste into a coding agent (lake#1185): "
            "the goal, endpoint, ids and key, what the key can and cannot do, the search_index "
            "and predict answers, the source, how to fit it to the site and handle empty, error "
            "and 429 answers, the allowed origins, how to check it, and links to MCP and "
            "llms.txt.",
        .input_schema = input_schema(
            {{"workspace_id", workspace_schema("the workspace the quickstart is in")},
             {"quickstart_id", quickstart_id_schema()},
             {"origins",
              {{"type", "array"},
               {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 300}}},
               {"minItems", 1},
               {"maxItems", 20},
               {"description",
                "your website's origins, such as https://www.example.com: makes the browser key "
                "now (create_browser_key) and puts it in the code, shown once"}}}},
            json::array({"workspace_id"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"quickstart_id", {{"type", "string"}}},
                            {"origin", {{"type", "string"}}},
                            {"ready", {{"type", "boolean"}}},
                            {"includes_model", {{"type", "boolean"}}},
                            {"snippets", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                            {"prompt",
                             {{"type", "string"},
                              {"description",
                               "one self-contained prompt for a coding agent to install the "
                               "search (and model answers) into a website: the source, the ids, "
                               "the key (when this call made it), its limits, the response "
                               "shapes, the origins and how to check it (lake#1185)"}}},
                            {"key", {{"type", json::array({"object", "null"})}}},
                            {"mcp", {{"type", "object"}}},
                            {"next", {{"type", "string"}}}}},
                          {"required",
                           json::array({"quickstart_id", "origin", "ready", "snippets", "key"})}},
        // POST, because with `origins` it makes a key (through create_browser_key, which is
        // authorized as a write); without, it changes nothing and is authorized as a read.
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = quickstart_snippets,
    });
}

} // namespace lake::api
