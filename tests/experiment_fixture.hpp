#pragma once

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>

#include "lake/control/memory_store.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "record_fixture.hpp"
#include "temporary_directory.hpp"

namespace lake::test {
using ExperimentJson = nlohmann::json;

inline ExperimentJson experiment_start_input() {
    return {{"name", "Retention Tuning"}, {"project", "retention"}, {"pipeline", "training"},
        {"dataset", "accounts"}, {"methodology", "M-1"}, {"from_defaults", true},
        {"objective", {{"metric", "held_out_auc"}, {"direction", "maximize"},
            {"validation_split", "validation"}}},
        {"budgets", {{"max_trials", 4}, {"max_parallel_trials", 1},
            {"max_wall_time_ms", 60000}, {"cost_cap", 5}}}, {"confirm", true}};
}

inline std::vector<std::pair<std::string, ExperimentJson>> experiment_calls(
    const std::string& customer, const std::string& workspace) {
    using json = ExperimentJson;
    auto start = experiment_start_input();
    start["customer_id"] = customer;
    // Lake's names (lake#561); the fake caller below answers the engine's study operations.
    const json experiment = {{"customer_id", customer}, {"experiment_id", "study-one"}};
    auto confirmed = experiment;
    confirmed["confirm"] = true;
    auto trial = confirmed;
    trial["run"] = "trial-1";
    std::vector<std::pair<std::string, json>> calls{
        {"start_experiment", start},
        {"list_experiments", {{"customer_id", customer}}},
        {"get_experiment", experiment},
        {"list_experiment_trials", experiment},
        {"get_experiment_analysis", experiment},
        {"get_run_metrics", {{"customer_id", customer}, {"run", "run-a"}}},
        {"compare_runs", {{"customer_id", customer}, {"runs", {"run-a", "run-b"}}}},
        {"tail_experiment_events",
         {{"customer_id", customer},
          {"kind", "experiment"},
          {"id", "study-one"},
          {"wait_ms", 100},
          {"max_events", 10}}},
        {"promote_trial", trial},
        {"stop_experiment_trial", trial},
        {"stop_experiment", confirmed}};
    for (auto& [name, input] : calls) {
        input["workspace_id"] = workspace;
    }
    return calls;
}

inline ExperimentJson experiment_metrics(const std::string& run, int count = 3) {
    using json = ExperimentJson;
    const bool second = run == "run-b" || run == "trial-2";
    json series = json::array();
    json summaries = json::object();
    for (const std::string name : {"train.loss", "validation.loss", "validation.auc",
                                  "train.learning_rate", "train.grad_norm", "system.memory_bytes"}) {
        json points = json::array();
        for (int step = 1; step <= count; ++step) {
            const double value = name == "validation.auc" ? 0.7 + step * 0.04 :
                name == "system.memory_bytes" ? 1024.0 * step :
                name == "train.learning_rate" ? 0.001 / step : 1.0 / step + (second ? 0.05 : 0);
            points.push_back({{"step", step}, {"value", value},
                {"time", "2026-09-28T18:20:" + std::to_string(10 + step) + ".000Z"}});
        }
        const auto last = points.back();
        series.push_back({{"name", name}, {"points", points}});
        summaries[name] = {{"final", last.at("value")}, {"final_step", count},
            {"best", last.at("value")}, {"best_step", count}, {"direction", "min"}};
    }
    json artifacts = json::array();
    const auto add = [&](const std::string& kind, json data) {
        artifacts.push_back({{"type", kind}, {"name", kind},
            {"time", "2026-09-28T18:20:20.000Z"}, {"data", std::move(data)}});
    };
    add("confusion_matrix", {{"labels", {"No", "Yes"}}, {"matrix", {{8, 2}, {1, 9}}}});
    add("roc_curve", {{"x", {0, 0.2, 1}}, {"y", {0, 0.8, 1}}, {"thresholds", {1, 0.5, 0}}});
    add("pr_curve", {{"x", {0, 0.5, 1}}, {"y", {1, 0.9, 0.6}}, {"thresholds", {1, 0.5, 0}}});
    add("calibration", {{"predicted", {0.2, 0.8}}, {"observed", {0.25, 0.75}}, {"counts", {10, 10}}});
    add("feature_importance", {{"features", {"age", "spend"}}, {"values", {0.25, 0.75}}});
    add("sample_predictions", {{"rows", {{{"account", "<script>alert(1)</script>"},
        {"predicted", true}, {"score", 0.83}, {"actual", nullptr}}}}});
    json result = {{"run", run}, {"params", {{"learning_rate", second ? 0.2 : 0.1},
        {"family", "gbdt"}, {"nullable", nullptr}}}, {"summary", summaries},
        {"series", series}, {"artifacts", artifacts}};
    if (run.starts_with("trial-")) {
        result["params"]["study.id"] = "study-one";
    }
    if (run == "dp-run") {
        result["metrics_suppressed"] = "differential_privacy";
        result["series"] = json::array();
        result["summary"] = json::object();
        result["artifacts"] = json::array();
    }
    return result;
}

inline ExperimentJson experiment_trial(int number, std::string state = "complete") {
    return {{"number", number}, {"run", "trial-" + std::to_string(number)},
        {"params", {{"learning_rate", number * 0.1}, {"model_family", "gbdt"}}},
        {"state", state}, {"objective", 0.8 + number * 0.03}, {"seed", 1138 + number},
        {"cost", 0.5}, {"reason", ""}, {"model", state == "complete" ? "model-one" : ""},
        {"intermediate", {{{"step", 1}, {"value", 0.8}}}}};
}

inline ExperimentJson experiment_study() {
    auto spec = experiment_start_input();
    spec.erase("confirm");
    spec.erase("from_defaults");
    spec.update({{"sampler", "tpe"}, {"pruner", "asha"}, {"seed", 1138},
        {"differential_privacy", false}, {"released_metrics", ExperimentJson::array()},
        {"search_space", {{{"name", "learning_rate"}, {"kind", "float"}, {"minimum", 0.01},
            {"maximum", 0.5}, {"step", 0}, {"log", false}, {"choices", ExperimentJson::array()},
            {"condition_parameter", ""}, {"condition_values", ExperimentJson::array()}}}}});
    return {{"id", "study-one"}, {"spec", spec}, {"status", "running"}, {"spent_cost", 1.0},
        {"stop_reason", ""}, {"promoted_model", ""},
        {"created", "2026-09-28T18:20:00.000Z"}, {"updated", "2026-09-28T18:20:03.000Z"},
        {"trials", {experiment_trial(1), experiment_trial(2, "running")}}};
}

class ExperimentCaller final : public control::WorkspaceCaller {
public:
    using json = ExperimentJson;
    json study = experiment_study();
    std::vector<json> calls;
    std::string last_token, last_url;
    std::shared_ptr<control::WorkspaceCaller> fallback;
    // Canned answers for engine capabilities the fixture does not model (get_graph, chat, …).
    std::map<std::string, json> answers;
    std::mutex mutex;
    bool live = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    json analysis() const {
        json result = {{"best_trial", nullptr}, {"optimization_history", json::array()},
            {"parameter_importances", {{{"parameter", "learning_rate"}, {"importance", 1.0}}}},
            {"parallel_coordinates", json::array()}, {"leaderboard", json::array()}};
        double best = 0;
        int rank = 0;
        for (const auto& trial : study.at("trials")) {
            if (trial.at("state") != "complete") {
                continue;
            }
            best = std::max(best, trial.at("objective").get<double>());
            result["best_trial"] = {{"number", trial.at("number")}, {"run", trial.at("run")},
                {"params", trial.at("params")}, {"value", trial.at("objective")}};
            result["optimization_history"].push_back({{"number", trial.at("number")},
                {"run", trial.at("run")}, {"value", trial.at("objective")}, {"best", best}});
            result["parallel_coordinates"].push_back({{"run", trial.at("run")},
                {"params", trial.at("params")}, {"value", trial.at("objective")}});
            result["leaderboard"].push_back({{"rank", ++rank}, {"run", trial.at("run")},
                {"value", trial.at("objective")}, {"confidence_low", 0.8}, {"confidence_high", 0.9}});
        }
        return result;
    }

    json run_listing() const {
        json runs = json::array();
        for (const std::string id : {"run-a", "run-b", "dp-run", "trial-1"}) {
            runs.push_back({{"id", id}, {"project", "retention"}, {"graph", "training"},
                {"deployment", ""}, {"status", "succeeded"}, {"log_count", 0}, {"output_count", 1},
                {"created", "2026-09-28T18:20:00.000Z"}, {"updated", "2026-09-28T18:20:03.000Z"}});
        }
        return {{"runs", runs}};
    }

    json comparison(const json& input) const {
        json metrics = json::array();
        for (const auto& name : input.at("names")) {
            json points = json::array();
            for (int step = 1; step <= 3; ++step) {
                json values = json::array();
                for (const std::string run : input.at("runs")) {
                    const auto data = experiment_metrics(run);
                    for (const auto& series : data.at("series")) {
                        if (series.at("name") == name) {
                            auto point = series.at("points").at(step - 1);
                            point.erase("step");
                            point["run"] = run;
                            values.push_back(point);
                        }
                    }
                }
                points.push_back({{"step", step}, {"values", values}});
            }
            metrics.push_back({{"name", name}, {"points", points}});
        }
        return {{"runs", input.at("runs")}, {"metrics", metrics}};
    }

    json metrics(const json& input, int count) const {
        auto result = experiment_metrics(input.at("run"), count);
        if (input.contains("names")) {
            auto& series = result["series"];
            series.erase(std::remove_if(series.begin(), series.end(), [&](const json& row) {
                return std::ranges::find(input.at("names"), row.at("name")) == input.at("names").end();
            }), series.end());
        }
        return result;
    }

    liquid::runtime::task<control::ZettaControlResult<json>> call(std::string url,
        std::string token, std::string capability, bool read, json input) override {
        // Streams, and the workspace records that hold their creators (lake#379) and drive share
        // links (lake#350), go to the fallback caller.
        if (fallback && (capability.find("topic") != std::string::npos ||
            capability == "publish_event" || capability == "update_event" || capability == "delete_events" ||
            RecordTables::handles(capability, input))) {
            co_return control::settled(co_await fallback->call(url, token, capability, read, input));
        }
        std::lock_guard lock(mutex);
        last_token = token;
        last_url = url;
        calls.push_back({{"name", capability}, {"read", read}, {"input", input}});
        const auto fail = [](const std::string& code) -> control::ZettaControlResult<json> {
            return std::unexpected(control::ZettaControlError{code, code});
        };
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count();
        if (live && study.at("status") == "running") {
            study["trials"] = elapsed < 2 ? json::array() : elapsed < 6 ?
                json::array({experiment_trial(1, "running")}) :
                json::array({experiment_trial(1), experiment_trial(2, "running")});
        }
        if (capability == "list_studies") {
            co_return json{{"studies", {study}}};
        }
        if (input.contains("study") && input.at("study") != study.at("id")) {
            // The engine's words; lake says experiment (lake#561).
            co_return control::ZettaControlResult<json>(std::unexpected(control::ZettaControlError{
                "not_found", "no study named '" + input.at("study").get<std::string>() + "'"}));
        }
        if (capability == "create_study" && input.value("name", "") == "Rejected By Zetta") {
            // Zetta reports the field at fault on every create_study error (zetta#1217).
            co_return control::ZettaControlResult<json>(std::unexpected(control::ZettaControlError{
                "invalid_input", "required; set from_defaults: true to fill a missing objective",
                "/objective"}));
        }
        if (capability == "create_study") {
            study = experiment_study();
            for (const auto& [key, value] : input.items()) {
                if (key != "from_defaults") {
                    study["spec"][key] = value;
                }
            }
            started = std::chrono::steady_clock::now();
            if (live) {
                study["trials"] = json::array();
            }
            co_return study;
        }
        if (capability == "get_study") {
            co_return study;
        }
        if (capability == "get_study_analysis") {
            co_return analysis();
        }
        if (capability == "list_trials") {
            json trials = json::array();
            for (const auto& trial : study.at("trials")) {
                if (!input.contains("state") || input.at("state") == trial.at("state")) {
                    trials.push_back(trial);
                }
            }
            co_return json{{"study", study.at("id")}, {"trials", trials}};
        }
        if (capability == "stop_study") {
            study["status"] = "stopped";
            co_return json{{"study", study.at("id")}, {"status", "stopped"}, {"reason", "stopped by caller"}};
        }
        if (capability == "stop_trial") {
            co_return json{{"study", study.at("id")}, {"run", input.at("run")}, {"state", "failed"}};
        }
        if (capability == "promote_trial") {
            study["promoted_model"] = "model-one";
            co_return json{{"study", study.at("id")}, {"run", input.at("run")},
                {"model", "model-one"}, {"project", "retention"}};
        }
        if (capability == "list_runs") {
            co_return run_listing();
        }
        if (capability == "get_run_metrics") {
            const std::string run = input.at("run");
            if (run == "foreign" || run == "missing") {
                co_return fail("not_found");
            }
            co_return metrics(input, live ? std::clamp<int>(elapsed, 1, 20) : 3);
        }
        if (capability == "compare_runs") {
            co_return comparison(input);
        }
        if (const auto canned = answers.find(capability); canned != answers.end()) {
            co_return canned->second;
        }
        if (capability == "receive_events") {
            const int sequence = input.value("from_sequence", 1);
            co_return json{{"events", {{{"sequence", sequence}, {"id", "event-one"},
                {"encoding", "json"}, {"payload", "{\"step\":3}"}}}},
                {"next_sequence", sequence + 1}};
        }
        co_return fail("unknown_capability");
    }
};

struct ExperimentWorkspace {
    std::string id;
    std::shared_ptr<ExperimentCaller> caller = std::make_shared<ExperimentCaller>();
    std::shared_ptr<control::WorkspaceService> service;

    ExperimentWorkspace(const std::shared_ptr<control::InMemoryStore>& store, const std::string& customer) {
        const auto directory = fixture_path("experiment-credentials-" + control::new_uuid_v4());
        std::filesystem::create_directories(directory);
        service = std::make_shared<control::WorkspaceService>(
            control::WorkspaceConfig{directory.string(), 0}, store, caller);
        const auto state = liquid::runtime::block_on(service->request(customer));
        if (!state || !*state) {
            throw std::runtime_error("could not request experiment workspace");
        }
        const auto& workspace = **state;
        id = workspace.workspace_id;
        const auto ready = store->set_service_status(workspace.service_id, "Ready", "", "http://experiments.test");
        if (!ready) {
            throw std::runtime_error(ready.error().message);
        }
        std::ofstream(directory / (workspace.slug + ".json")) <<
            ExperimentJson{{"kind", "zetta"}, {"token", "experiment-service-secret"}}.dump();
    }
};
} // namespace lake::test
