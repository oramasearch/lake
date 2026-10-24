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
//   and ids filled in, and the create_token call that makes the key they need.
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
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr control::RecordCollection quickstart_records{"quickstarts", 1};
// The newest quickstarts quickstart_status lists beside the one it answers.
constexpr std::size_t listed_quickstarts = 20;
// The methodology and held-out share Train → Models' New Model starts with.
constexpr std::string_view default_methodology = "M-1";
constexpr double default_holdout = 0.2;
// Where the website snippets put the key create_token answers.
constexpr std::string_view key_placeholder = "YOUR_LAKE_SEARCH_KEY";

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
        if (reason.empty()) {
            reason = "the training run " + status;
        }
        progress["error"] = {{"code", "run_failed"}, {"message", reason}};
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
    steps.push_back(step("model_answers",
                         "Model Answers on Your Website",
                         model_published                                 ? "done"
                         : error_of(record, "model_answers").is_object() ? "failed"
                                                                         : "pending",
                         error_of(record, "model_answers")));

    std::string state = "in_progress";
    std::string next;
    for (const json& entry : steps) {
        if (entry["state"] == "failed") {
            state = "failed";
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
        } else if (model_state == "running") {
            state = "training";
            next = "The model is training: read quickstart_status until the model step is done, "
                   "then call train_quickstart again to publish it.";
        } else if (model_state == "done") {
            state = "trained";
            next = "Call train_quickstart again to publish the model.";
        } else if (index_published) {
            state = "search_ready";
            next = "Search works: quickstart_snippets answers the website code. train_quickstart "
                   "trains a model on the same dataset (paid: it runs on the workspace's compute).";
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
             {"created_at", record.value("created_at", json(nullptr))},
             {"updated_at", record.value("updated_at", json(nullptr))}};
    if (!model_id.empty()) {
        out["model"] = {{"id", model_id}, {"published", model_published}};
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
            const json& record = loaded->record->body;
            const json progress =
                present(record, "training") && !present(record, "model")
                    ? take(co_await training_progress(context, customer_id, workspace_id, record))
                    : json(nullptr);
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
        const std::string training_state = progress.value("state", std::string());
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
        json training{{"name", name},
                      {"methodology", setting("methodology", std::string(default_methodology))},
                      {"target", setting("target", nullptr)},
                      {"entity_column", setting("entity_column", nullptr)},
                      {"holdout_fraction", setting("holdout_fraction", default_holdout)},
                      {"attempt", attempt}};
        json train{{"customer_id", customer_id},
                   {"workspace_id", workspace_id},
                   {"name", name},
                   {"methodology", training["methodology"]},
                   {"dataset_path",
                    record["dataset"]["database"].get<std::string>() + "." +
                        record["dataset"]["name"].get<std::string>()},
                   {"holdout_fraction", training["holdout_fraction"]}};
        for (const char* optional : {"target", "entity_column"}) {
            if (training[optional].is_string()) {
                train[optional] = training[optional];
            }
        }
        const ApiResult started = take(co_await nested(context, "train_model", std::move(train)));
        if (!started.ok) {
            if (started.error.code == "payment_required") {
                co_return started;
            }
            set_error(record, "model", error_json(started.error));
            if (auto failure = take(co_await save())) {
                co_return std::move(*failure);
            }
            co_return ApiResult::success(status_of(record, json(nullptr)));
        }
        training["run_id"] = text(started.value, "run_id");
        training["started_at"] = now_text();
        record["training"] = std::move(training);
        clear_error(record, "model");
        if (auto failure = take(co_await save())) {
            co_return std::move(*failure);
        }
        context.audit_details["run_id"] = record["training"]["run_id"];
        progress = {{"state", "running"},
                    {"status",
                     text(started.value, "status").empty() ? json("running")
                                                           : json(text(started.value, "status"))},
                    {"model_id", nullptr},
                    {"error", nullptr},
                    {"started_at", record["training"]["started_at"]}};
        co_return ApiResult::success(status_of(record, progress));
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

// ---- quickstart_snippets ----

// lake's browser-facing origin, as the website calls it.
std::string public_origin(const ApiContext& context) {
    if (!context.external_origin.empty()) {
        return context.external_origin;
    }
    if (!context.public_host.empty()) {
        return "https://" + context.public_host;
    }
    return context.request_origin.empty() ? std::string("https://lake.live")
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
        "  // Anyone who opens this page can read the key: it can only search published indexes";
    out += model.is_null() ? "" : " and ask published models";
    out += ".\n"
           "  // Revoke it on Lake's Tokens page.\n"
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
           "// Anyone who opens the page can read the key: it can only search published indexes";
    out += model.is_null() ? "" : " and ask published models";
    out += ".\nconst KEY = " + script_value(std::string(key_placeholder)) +
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

std::string curl_snippet(const std::string& origin, const json& search, const json& model) {
    std::string out = "export LAKE_SEARCH_KEY=" + std::string(key_placeholder) +
                      "\n"
                      "curl -s -G \"" +
                      origin +
                      "/v1/search_index\" \\\n"
                      "  -H \"Authorization: Bearer $LAKE_SEARCH_KEY\" \\\n";
    for (const char* field : {"customer_id", "workspace_id", "index_id"}) {
        out += "  --data-urlencode \"" + std::string(field) + "=" + text(search, field) + "\" \\\n";
    }
    out += "  --data-urlencode \"query=your words\"\n";
    if (!model.is_null()) {
        json body = model;
        body["rows"] = json::array({{{"<column>", "<value>"}}});
        out += "\ncurl -s \"" + origin +
               "/v1/predict\" \\\n"
               "  -H \"Authorization: Bearer $LAKE_SEARCH_KEY\" -H \"Content-Type: "
               "application/json\" \\\n"
               "  -d '" +
               body.dump() + "'\n";
    }
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
    json scopes = json::array({"search_index"});
    if (!model.is_null()) {
        scopes.push_back("predict");
    }
    out["ready"] = true;
    out["key"] = {
        {"placeholder", key_placeholder},
        {"scopes", scopes},
        {"create",
         {{"capability", "create_token"},
          {"input",
           {{"name", "website " + (table.empty() ? std::string("search") : table)},
            {"scopes", scopes}}}}},
        {"note",
         "Make the key with create_token (its `secret` is shown once) and put it where the "
         "snippets say " +
             std::string(key_placeholder) + ". It can only search published indexes" +
             (model.is_null() ? std::string() : std::string(" and ask published models")) +
             " of the accounts you can read, never change anything, and lasts as long as the "
             "sign-in that made it (at most 30 days) unless it is a standing key. Revoke it on "
             "the Tokens page. Every search and model answer is billed to the account."}};
    out["snippets"] = json::array(
        {{{"id", "html"},
          {"title", "Plain Web Page"},
          {"language", "html"},
          {"code", html_snippet(origin, search, model, target.empty() ? "model" : target, table)}},
         {{"id", "react"},
          {"title", "React"},
          {"language", "jsx"},
          {"code", react_snippet(origin, search, model, target.empty() ? "model" : target)}},
         {{"id", "curl"},
          {"title", "curl"},
          {"language", "shell"},
          {"code", curl_snippet(origin, search, model)}}});
    out["next"] = model.is_null()
                      ? "Paste a snippet into your site with a key from create_token. "
                        "train_quickstart adds model answers."
                      : "Paste a snippet into your site with a key from create_token: it shows "
                        "search results with the model's answer beside each.";
    co_return ApiResult::success(std::move(out));
}

json status_output_schema() {
    const json step_schema{
        {"type", "object"},
        {"properties",
         {{"step", {{"type", "string"}}},
          {"title", {{"type", "string"}}},
          {"state", {{"type", "string"}, {"enum", {"pending", "running", "done", "failed"}}}},
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
          {"predict", {{"type", json::array({"object", "null"})}}}}},
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
            "(expose_index). Each step's state is kept in the workspace; a step that fails is "
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
            "(pending, running, done or failed, with why), the dataset, index and model ids, the "
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
            "website can ask it. Call it once to start training, read quickstart_status for "
            "progress, then call it again to publish; while the run is still going it changes "
            "nothing. A failed run is trained again by the next call, with the settings sent then "
            "or the ones before. Paid: training runs on the workspace engine's billed compute; "
            "refused 402 payment_required with the top-up link while the balance is blocked.",
        .input_schema = input_schema(
            {{"workspace_id", workspace_schema("the workspace the quickstart is in")},
             {"quickstart_id", quickstart_id_schema()},
             {"target",
              string_schema("the column to predict (list_methodologies says whether "
                            "the methodology needs one)")},
             {"entity_column",
              string_schema("the column that identifies a row (default: the "
                            "dataset's own)")},
             {"methodology",
              string_schema("a methodology id from list_methodologies (default "
                            "M-1, tabular supervised learning)")},
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
        .name = "quickstart_snippets",
        .title = "Quickstart website snippets",
        .description =
            "The website instructions of the quickstart process (lake#1128): code for a plain web "
            "page, a React component and curl that search the quickstart's published index, with "
            "the model's answer beside each result once train_quickstart has published it, with "
            "Lake's host and the account, workspace, index and model ids filled in. The key is "
            "never stored or answered: `key.create` is the create_token call (scopes search_index, "
            "and predict once there is a model) whose `secret` goes where the code says "
            "YOUR_LAKE_SEARCH_KEY. The key can only search published indexes and ask published "
            "models, and anyone who opens the page can read it.",
        .input_schema =
            input_schema({{"workspace_id", workspace_schema("the workspace the quickstart is in")},
                          {"quickstart_id", quickstart_id_schema()}},
                         json::array({"workspace_id"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"quickstart_id", {{"type", "string"}}},
                            {"origin", {{"type", "string"}}},
                            {"ready", {{"type", "boolean"}}},
                            {"includes_model", {{"type", "boolean"}}},
                            {"snippets", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                            {"key", {{"type", json::array({"object", "null"})}}},
                            {"mcp", {{"type", "object"}}},
                            {"next", {{"type", "string"}}}}},
                          {"required",
                           json::array({"quickstart_id", "origin", "ready", "snippets", "key"})}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = quickstart_snippets,
    });
}

} // namespace lake::api
