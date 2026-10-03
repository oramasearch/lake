#include "lake/api/web_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>

#include "lake/api/oauth.hpp"

namespace lake::api {
namespace {
using nlohmann::json;

std::string escape(const json& value) {
    return web_ui_escape(value.is_string() ? value.get<std::string>() : value.dump());
}

// Zetta's held-out metric names: the run report's keys, with the spellings Zetta still accepts
// as aliases (zetta include/zetta/ml/metric_names.hpp, zetta#1217). Zetta normalizes what it is
// given; this only labels a study recorded before then by the name it now has.
constexpr std::array<std::pair<std::string_view, std::array<std::string_view, 5>>, 5> metric_names{{
    {"held_out_auc", {"validation.auc", "ROC-AUC", "held-out ROC-AUC", "roc_auc", "auc"}},
    {"held_out_log_loss", {"validation.log_loss", "log loss", "log_loss", "logloss", ""}},
    {"held_out_accuracy", {"validation.accuracy", "accuracy", "", "", ""}},
    {"held_out_rmse", {"validation.rmse", "RMSE", "rmse", "", ""}},
    {"held_out_mae", {"validation.mae", "MAE", "mae", "", ""}},
}};

// Zetta's matching: case is ignored, and ' ', '-' and '.' read as '_'.
std::string metric_key(std::string_view name) {
    std::string key;
    for (const char character : name) {
        key += character == ' ' || character == '-' || character == '.'
                   ? '_'
                   : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return key;
}

json metric_aliases() {
    json aliases = json::object();
    for (const auto& [canonical, spellings] : metric_names) {
        aliases[metric_key(canonical)] = canonical;
        for (const auto spelling : spellings) {
            if (!spelling.empty()) {
                aliases[metric_key(spelling)] = canonical;
            }
        }
    }
    return aliases;
}

std::string metric_label(const json& name) {
    if (!name.is_string()) {
        return escape(name);
    }
    static const json aliases = metric_aliases();
    const auto found = aliases.find(metric_key(name.get<std::string>()));
    return web_ui_escape(found == aliases.end() ? name.get<std::string>()
                                                : found->get<std::string>());
}

std::string link(const ExperimentsPage& page, std::string_view path, std::string_view key,
                 const std::string& id, std::string_view label, std::string_view capability) {
    return "<a data-capability=\"" + std::string(capability) + "\" href=\"" + std::string(path) +
        "?customer_id=" + oauth_url_encode(page.customer_id) + "&amp;" + std::string(key) + "=" +
        oauth_url_encode(id) + "\">" + web_ui_escape(label) + "</a>";
}

std::string input(std::string_view label, std::string_view name, std::string_view value,
                  std::string_view attributes = "") {
    return "<label>" + std::string(label) + "<input name=\"" + std::string(name) + "\" value=\"" +
        web_ui_escape(value) + "\" " + std::string(attributes) + "></label>";
}

std::string hidden(std::string_view name, std::string_view value) {
    return "<input type=\"hidden\" name=\"" + std::string(name) + "\" value=\"" +
           web_ui_escape(value) + "\">";
}

std::string action(const std::string& capability, const std::string& label,
                   const std::string& study, const std::string& run = "") {
    return "<button type=\"button\" data-action=\"" + capability + "\" data-capability=\"" +
        capability + "\" data-study=\"" + web_ui_escape(study) + "\" data-run=\"" +
        web_ui_escape(run) + "\" disabled>" + label + "</button>";
}

std::string start_form() {
    std::string html = "<details class=\"panel\"><summary data-capability=\"start_study\">"
        "Start a Study</summary><form id=\"start-study\" data-capability=\"start_study\">"
        "<div class=\"experiment-fields\">";
    html += input("Study name", "name", "", "required");
    html += input("Project", "project", "", "required");
    html += input("Pipeline", "pipeline", "", "required");
    html += input("Dataset", "dataset", "", "required");
    html += input("Methodology", "methodology", "M-1", "required");
    html += input("Objective metric", "objective.metric", "held_out_auc",
                  "list=\"metric-names\" required");
    html += "<datalist id=\"metric-names\">";
    for (const auto& [canonical, spellings] : metric_names) {
        html += "<option value=\"" + std::string(canonical) + "\"></option>";
    }
    html += "</datalist>";
    html += "<label>Direction<select name=\"objective.direction\"><option value=\"maximize\">"
        "Maximize</option><option value=\"minimize\">Minimize</option></select></label>";
    html += input("Validation split", "objective.validation_split", "validation", "required");
    html += "<label>Sampler<select name=\"sampler\"><option>tpe</option><option>random</option>"
        "<option>grid</option></select></label><label>Pruner<select name=\"pruner\">"
        "<option>asha</option><option>median</option><option>none</option></select></label>";
    html += input("Trial budget", "budgets.max_trials", "10", "type=\"number\" min=\"1\" required");
    html += input("Parallel trials", "budgets.max_parallel_trials", "1", "type=\"number\" min=\"1\" required");
    html += input("Wall-time budget (ms; 0 is unlimited)", "budgets.max_wall_time_ms", "0",
                  "type=\"number\" min=\"0\" required");
    html += input("Cost cap (0 is unlimited)", "budgets.cost_cap", "0",
                  "type=\"number\" min=\"0\" step=\"any\" required");
    html += input("Seed", "seed", "1138", "type=\"number\" min=\"0\" required");
    html += "</div><label>Search parameters (JSON; leave blank for methodology defaults)"
        "<textarea name=\"search_space\" rows=\"4\"></textarea></label>"
        "<label><input type=\"checkbox\" name=\"differential_privacy\"> Differential privacy</label>"
        "<label>Released metrics for private studies (comma separated)"
        "<input name=\"released_metrics\"></label><p>Selection uses the validation split. "
        "The final test split is never used for tuning. Metrics are named as the run report "
        "names them: held_out_auc, held_out_log_loss, held_out_accuracy, held_out_rmse, "
        "held_out_mae.</p>"
        "<button type=\"submit\" data-start disabled>Review and start study</button></form></details>";
    return html;
}

std::string agent_panel(const ExperimentsPage& page) {
    std::string html =
        "<section class=\"panel\"><h2>Run an Experiment</h2><p class=\"sub\">Describe the "
        "training or tuning goal. ExperimentsAgent proposes a complete study for you to edit and "
        "confirm before it starts.</p><form class=\"stack\" method=\"post\" "
        "action=\"/train/experiments/agent\" data-capability=\"agent_turn\">" +
        hidden("customer_id", page.customer_id) +
        "<label>Goal<textarea name=\"goal\" rows=\"4\" placeholder=\"Tune a churn model for "
        "validation AUC with a bounded trial budget.\" required";
    if (!page.experiments_agent) {
        html += " disabled";
    }
    html += "></textarea></label><button type=\"submit\"";
    if (!page.experiments_agent) {
        html += " disabled";
    }
    html += ">Ask ExperimentsAgent</button></form>";
    if (!page.experiments_agent) {
        html += "<p class=\"notice status-warning\">ExperimentsAgent is not registered on this "
                "lake yet.</p>";
    }
    html += "</section>";
    if (page.agent_task.empty()) {
        return html;
    }
    json answer = page.agent_task.value("answer", json::object());
    if (!answer.is_object()) {
        answer = json::object();
    }
    json result = answer.value("result", json::object());
    if (!result.is_object()) {
        result = json::object();
    }
    const std::string reply =
        answer.value("reply", page.agent_task.value("message", std::string()));
    if (page.agent_working) {
        return html +
               "<section class=\"panel\" aria-live=\"polite\"><h2>ExperimentsAgent Is "
               "Working</h2><p>It keeps working if you leave; this page checks again every 2 "
               "seconds.</p><p role=\"status\">" +
               web_ui_escape(page.agent_task.value("message", std::string())) +
               // The review URL ends in #experiment-proposal, and a declarative refresh to it
               // is a fragment navigation, never a reload (lake#438). Reload it; the declarative
               // refresh remains without scripts.
               "</p><script>setTimeout(()=>location.reload(),2000);</script><noscript><meta "
               "http-equiv=\"refresh\" content=\"2\"></noscript></section>";
    }
    if (result.value("kind", std::string()) == "proposal" &&
        result.value("confirm", false) && result.value("study", json()).is_object()) {
        return html + "<section class=\"panel\" id=\"experiment-proposal\"><h2>Proposed "
                      "Study</h2><div class=\"ask-reply\">" +
               web_ui_markdown(reply) +
               "</div><p>Edit the study JSON if needed. The reviewed object is validated again "
               "before the agent can start it.</p><form class=\"stack\" method=\"post\" "
               "action=\"/train/experiments/agent/confirm\" "
               "data-capability=\"agent_turn\">" +
               hidden("customer_id", page.customer_id) +
               hidden("task_id", page.agent_task.value("task_id", std::string())) +
               hidden("proposal_sha256", page.proposal_sha256) +
               "<label>Study proposal (JSON)<textarea name=\"study\" rows=\"18\" required>" +
               web_ui_escape(result.at("study").dump(2)) +
               "</textarea></label><details><summary>Why these choices</summary><pre>" +
               web_ui_escape(result.value("explanations", json::object()).dump(2)) +
               "</pre></details><button class=\"button button-primary\" type=\"submit\">Confirm "
               "and start study</button></form></section>";
    }
    std::string result_link;
    const json study = result.value("study", json::object());
    if (result.value("kind", std::string()) == "running" && study.is_object() &&
        study.value("id", std::string()) != "") {
        result_link = "<p>" + link(page,
                                   "/train/experiment",
                                   "study",
                                   study.at("id"),
                                   "Open the running study",
                                   "get_experiment_study") +
                      "</p>";
    }
    std::string reply_form;
    if (page.agent_reply) {
        reply_form =
            "<h3>Reply</h3><form class=\"stack\" method=\"post\" "
            "action=\"/train/experiments/agent\" data-capability=\"agent_turn\">" +
            hidden("customer_id", page.customer_id) +
            hidden("task_id", page.agent_task.value("task_id", std::string())) +
            "<label>Reply<textarea name=\"reply\" rows=\"4\" required></textarea></label>"
            "<button class=\"button button-primary\" type=\"submit\">Reply to "
            "ExperimentsAgent</button></form>";
    }
    return html + "<section class=\"panel\"><h2>ExperimentsAgent Result</h2><div "
                  "class=\"ask-reply\">" +
           web_ui_markdown(reply) + "</div>" + result_link + reply_form + "</section>";
}

std::string study_listing(const ExperimentsPage& page) {
    std::string html = "<section class=\"panel\"><h2>Studies</h2><div class=\"table-scroll\">"
        "<table><thead><tr><th>Name</th><th>Project</th><th>State</th><th>Objective</th>"
        "<th>Best</th><th>Complete / Pruned / Failed</th><th>Trials / Budget</th>"
        "<th>Cost / Cap</th><th>Actions</th></tr></thead><tbody>";
    for (const auto& study : page.data.value("studies", json::array())) {
        const auto& spec = study.at("spec");
        std::map<std::string, int> counts;
        json best = nullptr;
        for (const auto& trial : study.at("trials")) {
            ++counts[trial.at("state")];
            if (trial.at("state") == "complete" && trial.contains("objective")) {
                const auto& value = trial.at("objective");
                if (best.is_null() || (spec.at("objective").at("direction") == "maximize"
                    ? value > best : value < best)) {
                    best = value;
                }
            }
        }
        html += "<tr><td>" + link(page, "/train/experiment", "study", study.at("id"),
            spec.at("name").get<std::string>(), "get_experiment_study") + "</td><td>" + escape(spec.at("project")) +
            "</td><td>" + escape(study.at("status")) + "</td><td>" +
            metric_label(spec.at("objective").at("metric")) + "</td><td>" +
            (best.is_null() ? "—" : escape(best)) + "</td><td>" +
            std::to_string(counts["complete"]) + " / " + std::to_string(counts["pruned"]) +
            " / " + std::to_string(counts["failed"]) + "</td><td>" +
            std::to_string(study.at("trials").size()) + " / " +
            escape(spec.at("budgets").at("max_trials")) + "</td><td>" + escape(study.at("spent_cost")) +
            " / " + escape(spec.at("budgets").at("cost_cap")) + "</td><td>";
        if (page.can_write && study.at("status") == "running") {
            html += action("stop_study", "Stop", study.at("id"));
        }
        html += "</td></tr>";
    }
    return html + "</tbody></table></div></section>";
}

std::string list_page(const ExperimentsPage& page) {
    std::string html = "<form method=\"get\" action=\"/train/experiments\" "
        "data-capability=\"list_experiments\" class=\"experiment-fields\">"
        "<input type=\"hidden\" name=\"customer_id\" value=\"" + web_ui_escape(page.customer_id) + "\">";
    html += input("Project", "project", page.project) +
        "<label>State<select name=\"state\">";
    for (const std::string state : {"", "pending", "running", "succeeded", "failed", "stopped", "complete"}) {
        html += "<option value=\"" + state + "\"" + (state == page.state ? " selected" : "") +
            ">" + (state.empty() ? "All states" : state) + "</option>";
    }
    html += "</select></label><button type=\"submit\">Filter</button></form>";
    if (page.can_write) {
        html += start_form();
    }
    html += study_listing(page);
    html += "<section class=\"panel\"><h2>Tracked Runs Outside Studies</h2>"
        "<form id=\"compare-selection\" data-capability=\"compare_runs\">"
        "<div class=\"table-scroll\"><table><thead><tr><th>Compare</th><th>Run</th>"
        "<th>Project</th><th>State</th><th>Created</th></tr></thead><tbody>";
    for (const auto& run : page.data.value("runs", json::array())) {
        html += "<tr><td><input type=\"checkbox\" name=\"runs\" value=\"" + escape(run.at("id")) +
            "\" aria-label=\"Compare run " + escape(run.at("id")) + "\"></td><td>" +
            link(page, "/train/experiment/run", "run", run.at("id"), run.at("id").get<std::string>(), "get_run_metrics") +
            "</td><td>" + escape(run.at("project")) + "</td><td>" + escape(run.at("status"));
        if (run.contains("metrics_suppressed")) {
            html += " · curves suppressed for differential privacy";
        }
        html += "</td><td>" + escape(run.at("created")) + "</td></tr>";
    }
    html += "</tbody></table></div><button type=\"submit\">Compare selected runs</button>"
        "<p>Select two to eight runs. Trial runs can also be compared from their study.</p>"
        "</form></section>";
    return html;
}

std::string metric_tables(const json& details) {
    std::string html;
    for (const auto& run : details) {
        if (run.contains("metrics_suppressed")) {
            html += "<p>" + escape(run.at("run")) +
                ": curves suppressed for differential privacy</p>";
            continue;
        }
        for (const auto& series : run.value("series", json::array())) {
            html += "<details><summary data-capability=\"get_run_metrics\">" +
                escape(run.at("run")) + " · " + escape(series.at("name")) +
                " — Data Table</summary><div class=\"table-scroll\"><table><thead><tr>"
                "<th>Step</th><th>Wall time</th><th>Value</th></tr></thead><tbody>";
            for (const auto& point : series.at("points")) {
                html += "<tr><td>" + escape(point.at("step")) + "</td><td>" +
                    escape(point.at("time")) + "</td><td>" + escape(point.at("value")) + "</td></tr>";
            }
            html += "</tbody></table></div></details>";
        }
        html += "<details><summary data-capability=\"get_run_metrics\">Parameters and Artifacts — " +
            escape(run.at("run")) + "</summary><pre>" + escape(run.at("params").dump(2)) +
            "</pre><pre>" + escape(run.at("artifacts").dump(2)) + "</pre></details>";
    }
    return html;
}

std::string plot_controls(std::string_view capability) {
    const std::string mapping = " data-capability=\"" + std::string(capability) + "\"";
    return "<div class=\"experiment-toolbar\"><label>X axis<select id=\"chart-axis\"" + mapping +
        "><option value=\"step\">Step</option><option value=\"relative\">Relative time</option>"
        "<option value=\"wall\">Wall time</option></select></label><label>Smoothing (EMA) "
        "<output id=\"smoothing-value\">0%</output><input id=\"chart-smoothing\" type=\"range\" "
        "min=\"0\" max=\"99\" value=\"0\"" + mapping + "></label><label><input id=\"chart-log\" "
        "type=\"checkbox\"" + mapping + "> Log scale</label>"
        "<button type=\"button\" data-view=\"in\"" + mapping + ">Zoom in</button>"
        "<button type=\"button\" data-view=\"out\"" + mapping + ">Zoom out</button>"
        "<button type=\"button\" data-view=\"left\"" + mapping + ">Pan left</button>"
        "<button type=\"button\" data-view=\"right\"" + mapping + ">Pan right</button>"
        "<button type=\"button\" data-view=\"reset\"" + mapping + ">Reset view</button></div>"
        "<p class=\"sub\">Drag a region to zoom; Shift-drag to pan. Hover for the crosshair and "
        "values. Non-positive values are omitted on a log axis and remain in the data table.</p>"
        "<div id=\"metric-panels\"></div><section class=\"panel\"><h2>Parameter Differences</h2>"
        "<div id=\"params-diff\"></div></section><section class=\"panel\"><h2>Artifacts</h2>"
        "<div id=\"run-artifacts\"></div></section>";
}

std::string study_page(const ExperimentsPage& page) {
    if (!page.data.contains("spec")) {
        return "";
    }
    const auto& spec = page.data.at("spec");
    std::string html = "<section class=\"panel\"><h2>" + escape(spec.at("name")) +
        "</h2><p id=\"study-status\">State: " + escape(page.data.at("status")) +
        " · Objective: " + metric_label(spec.at("objective").at("metric")) + " (" +
        escape(spec.at("objective").at("direction")) + ")</p>";
    if (page.can_write && page.data.at("status") == "running") {
        html += action("stop_study", "Stop study", page.study);
    }
    if (const std::string promoted = page.data.value("promoted_model", std::string());
        !promoted.empty()) {
        html += "<p><span class=\"badge status-success\">promoted</span> Model <code>" +
                web_ui_escape(promoted) + "</code> is this study's promoted model.</p>";
    }
    // Readable without scripts (lake#449): each analysis is a table; the charts draw over them.
    const auto value = [](const json& row, const char* key) {
        return row.is_object() && row.contains(key) ? escape(row.at(key)) : std::string("—");
    };
    std::string history;
    for (const auto& row : page.analysis.value("optimization_history", json::array())) {
        history += "<tr><td>" + value(row, "number") + "</td><td>" + value(row, "run") +
                   "</td><td>" + value(row, "value") + "</td><td>" + value(row, "best") +
                   "</td></tr>";
    }
    std::vector<std::string> parameters;
    for (const auto& row : page.analysis.value("parallel_coordinates", json::array())) {
        for (const auto& [name, ignored] : row.value("params", json::object()).items()) {
            if (std::ranges::find(parameters, name) == parameters.end()) {
                parameters.push_back(name);
            }
        }
    }
    std::ranges::sort(parameters);
    std::string parallel = "<tr><th scope=\"col\">Run</th>";
    for (const auto& name : parameters) {
        parallel += "<th scope=\"col\">" + web_ui_escape(name) + "</th>";
    }
    parallel += "<th scope=\"col\">Objective</th></tr></thead><tbody>";
    for (const auto& row : page.analysis.value("parallel_coordinates", json::array())) {
        const json params = row.value("params", json::object());
        parallel += "<tr><td>" + value(row, "run") + "</td>";
        for (const auto& name : parameters) {
            parallel += "<td>" + value(params, name.c_str()) + "</td>";
        }
        parallel += "<td>" + value(row, "value") + "</td></tr>";
    }
    std::string importance;
    for (const auto& row : page.analysis.value("parameter_importances", json::array())) {
        importance += "<tr><td>" + value(row, "parameter") + "</td><td>" +
                      value(row, "importance") + "</td></tr>";
    }
    const auto empty = [](const std::string& rows, std::string_view what) {
        return rows.empty() ? "<p>" + std::string(what) + "</p>" : std::string();
    };
    html += "</section><section class=\"panel\"><h2>Optimization History</h2>"
        "<div id=\"optimization-chart\"></div><div id=\"optimization-table\">" +
        empty(history, "No trial has completed yet.") +
        "<div class=\"table-scroll\"><table><thead><tr><th scope=\"col\">Trial</th>"
        "<th scope=\"col\">Run</th><th scope=\"col\">Objective</th><th scope=\"col\">Best "
        "so far</th></tr></thead><tbody>" + history + "</tbody></table></div></div></section>"
        "<section class=\"panel\"><h2>Parallel Coordinates</h2><div id=\"parallel-chart\"></div>"
        "<div id=\"parallel-table\"><div class=\"table-scroll\"><table><thead>" + parallel +
        "</tbody></table></div></div></section><section class=\"panel\"><h2>Parameter "
        "Importance</h2><div id=\"importance-chart\"></div><div id=\"importance-table\">" +
        empty(importance, "Parameter importance needs completed trials.") +
        "<div class=\"table-scroll\"><table><thead><tr><th scope=\"col\">Parameter</th>"
        "<th scope=\"col\">Importance</th></tr></thead><tbody>" + importance +
        "</tbody></table></div></div></section><section class=\"panel\"><h2>Trial Leaderboard</h2>"
        "<label>Trial state<select id=\"trial-state\" data-capability=\"list_experiment_trials\">"
        "<option value=\"\">All states</option><option>running</option><option>complete</option>"
        "<option>pruned</option><option>failed</option></select></label>"
        "<details><summary data-capability=\"get_experiment_analysis\">Columns</summary>"
        "<div id=\"trial-columns\"></div></details><form id=\"compare-selection\" "
        "data-capability=\"compare_runs\"><div id=\"leaderboard\" class=\"table-scroll\">"
        "<table><thead><tr><th>Compare</th><th>Trial</th><th>State</th><th>Objective</th>"
        "<th>Parameters</th><th>Actions</th></tr></thead><tbody>";
    for (const auto& trial : page.data.at("trials")) {
        html += "<tr><td><input name=\"runs\" type=\"checkbox\" value=\"" + escape(trial.at("run")) +
            "\" aria-label=\"Compare trial " + escape(trial.at("number")) + "\"></td><td>" +
            link(page, "/train/experiment/run", "run", trial.at("run"),
                 "Trial " + trial.at("number").dump(), "get_run_metrics") + "</td><td>" +
            escape(trial.at("state")) +
            (!page.data.value("promoted_model", std::string()).empty() &&
                     trial.value("model", std::string()) == page.data.value("promoted_model", std::string())
                 ? " <span class=\"badge status-success\">promoted</span>"
                 : std::string()) +
            "</td><td>" + escape(trial.value("objective", json("—"))) +
            "</td><td>" + escape(trial.at("params").dump()) + "</td><td>";
        if (page.can_promote && trial.at("state") == "complete" && trial.at("model") != "") {
            html += action("promote_trial", "Promote", page.study, trial.at("run"));
        }
        if (page.can_write && trial.at("state") == "running") {
            html += action("stop_experiment_trial", "Stop trial", page.study, trial.at("run"));
        }
        html += "</td></tr>";
    }
    html += "</tbody></table></div><button type=\"submit\">Compare selected runs</button></form>"
        "</section><h2>Latest Trial Curves</h2><p>The latest eight trial runs appear below. "
        "Open any trial for its full charts and artifacts.</p>" + plot_controls("get_run_metrics");
    return html;
}

std::string safe_json(json data) {
    auto text = data.dump();
    std::string result;
    for (const char character : text) {
        if (character == '<') {
            result += "\\u003c";
        } else if (character == '>') {
            result += "\\u003e";
        } else if (character == '&') {
            result += "\\u0026";
        } else {
            result += character;
        }
    }
    return result;
}
} // namespace

std::span<const std::string_view> web_ui_experiment_asset_names() {
    static constexpr std::string_view names[] = {
        "uPlot.iife.js", "uPlot.min.css", "experiments.js", "experiments.css"};
    return names;
}

std::string_view web_ui_experiment_asset_url(std::string_view name) {
    static const auto urls = [] {
        std::map<std::string_view, std::string> result;
        for (const auto asset : web_ui_experiment_asset_names()) {
            result.emplace(asset, web_ui_hashed_asset_url("/assets/experiments/" + std::string(asset),
                web_ui_experiment_asset(asset)));
        }
        return result;
    }();
    const auto found = urls.find(name);
    return found == urls.end() ? std::string_view{} : std::string_view(found->second);
}

std::string web_ui_experiments(const ExperimentsPage& page, WebUiViewer viewer) {
    std::string html;
    for (const std::string_view name : {"uPlot.min.css", "experiments.css"}) {
        html += "<link rel=\"stylesheet\" href=\"" + std::string(web_ui_experiment_asset_url(name)) + "\">";
    }
    html += "<section id=\"experiments\"><p class=\"eyebrow\">Train</p><h1>" +
        std::string(page.mode == "study" ? "Study" : page.mode == "run" ? "Run Charts" :
            page.mode == "compare" ? "Compare Runs" : "Experiments") + "</h1>"
        "<p>Follow training, compare results, and promote a completed trial.</p>"
        "<p id=\"experiment-notice\" role=\"status\">" + web_ui_escape(page.error) + "</p>"
        "<noscript><p>Recorded data remains available below. Enable JavaScript for charts, live "
        "updates, and confirmed actions.</p></noscript>";
    if (page.mode != "list") {
        html += "<p>" + link(page, "/train/experiments", "project", "", "All experiments",
            "list_experiments") + "</p><label><input id=\"experiment-live\" type=\"checkbox\" "
            "data-capability=\"tail_experiment_events\" checked> Live updates</label>"
            "<span id=\"live-status\" role=\"status\">Connecting…</span>";
    }
    json details = json::array();
    if (page.mode == "list") {
        html += agent_panel(page);
        html += list_page(page);
    } else if (page.mode == "study") {
        html += study_page(page);
        details = page.trial_metrics;
        html += "<section class=\"panel\" id=\"initial-data\"><h2>Recorded Data</h2>" +
            (details.empty() ? std::string("<p>No trial has recorded curves yet.</p>")
                             : metric_tables(details)) + "</section>";
    } else {
        html += plot_controls(page.mode == "compare" ? "compare_runs" : "get_run_metrics");
        details = page.mode == "compare" ? page.data.value("details", json::array()) :
            page.data.contains("run") ? json::array({page.data}) : json::array();
        html += "<section class=\"panel\" id=\"initial-data\"><h2>Recorded Data</h2>" +
            metric_tables(details) + "</section>";
    }
    html += "<dialog id=\"experiment-confirm\" aria-labelledby=\"confirm-title\">"
        "<h2 id=\"confirm-title\">Confirm Action</h2><p id=\"confirm-message\"></p>"
        "<pre id=\"confirm-details\"></pre><button id=\"confirm-cancel\" type=\"button\" "
        "data-capability=\"start_study\">Cancel</button><button id=\"confirm-accept\" "
        "type=\"button\" data-capability=\"start_study\">Confirm</button></dialog>";
    html += "<script type=\"application/json\" id=\"experiment-data\">" + safe_json({
        {"customer_id", page.customer_id}, {"mode", page.mode}, {"study", page.study},
        {"run", page.run}, {"runs", page.runs}, {"data", page.data}, {"analysis", page.analysis},
        {"details", details}, {"can_write", page.can_write}, {"can_promote", page.can_promote},
        {"metric_names", metric_aliases()}}) +
        "</script></section>";
    for (const std::string_view name : {"uPlot.iife.js", "experiments.js"}) {
        html += "<script defer src=\"" + std::string(web_ui_experiment_asset_url(name)) + "\"></script>";
    }
    return web_ui_shell("lake — Experiments",
        {{"Train", "/train/runs", ""}, {"Experiments", "/train/experiments", ""}}, html, viewer);
}
} // namespace lake::api
