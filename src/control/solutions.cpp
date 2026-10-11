#include "lake/control/solutions.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string json_text(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// Cuts `text` to at most `bytes`, never inside a UTF-8 sequence.
std::string bounded(std::string text, std::size_t bytes) {
    if (text.size() <= bytes) {
        return text;
    }
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    text.resize(end);
    return text;
}

bool methodology_id(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
    });
}

std::int64_t now_microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Read {
    std::optional<WorkspaceSolution> solution;
    std::int64_t revision = 0;
};

// Reads the solution `id` of the space, or answers why not (`not_found` for another space's, a
// missing one, or an id that is not a workspace solution's).
liquid::runtime::task<ZettaControlResult<Read>>
read_own(WorkspaceRecords& records, const std::string& customer_id, const std::string& id) {
    if (!workspace_solution_id(id)) {
        co_return refused<Read>("not_found", "no such solution in this workspace");
    }
    auto got = settled(co_await records.get(WorkspaceSolutionCodec::collection, id));
    if (!got && got.error().code != "invalid_input") {
        co_return ZettaControlResult<Read>(std::unexpected(got.error()));
    }
    Read read;
    if (got && *got) {
        read.solution = WorkspaceSolutionCodec::decode(**got);
        read.revision = (*got)->revision;
    }
    if (!read.solution || read.solution->customer_id != customer_id) {
        co_return refused<Read>("not_found", "no such solution in this workspace");
    }
    co_return ZettaControlResult<Read>(std::move(read));
}

} // namespace

bool workspace_solution_id(std::string_view id) {
    if (id.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < id.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? id[index] != '-' : std::isxdigit(static_cast<unsigned char>(id[index])) == 0) {
            return false;
        }
    }
    return true;
}

std::string solutions_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::optional<SolutionProblem> solution_problem(const WorkspaceSolution& solution) {
    if (solution.customer_id.empty()) {
        return SolutionProblem{"a solution belongs to an account", "/customer_id"};
    }
    const auto required = [](const std::string& value,
                             std::size_t bound,
                             const char* what,
                             const char* path) -> std::optional<SolutionProblem> {
        if (value.empty()) {
            return SolutionProblem{std::string("a solution needs ") + what, path};
        }
        if (value.size() > bound) {
            return SolutionProblem{
                std::string(what) + " is longer than " + std::to_string(bound) + " bytes", path};
        }
        return std::nullopt;
    };
    const auto optional = [](const std::string& value,
                             std::size_t bound,
                             const char* what,
                             const char* path) -> std::optional<SolutionProblem> {
        if (value.size() > bound) {
            return SolutionProblem{
                std::string(what) + " is longer than " + std::to_string(bound) + " bytes", path};
        }
        return std::nullopt;
    };
    if (auto problem = required(solution.name, max_solution_name_bytes, "a name", "/name")) {
        return problem;
    }
    if (auto problem = required(
            solution.problem, max_solution_text_bytes, "the business problem", "/problem")) {
        return problem;
    }
    if (auto problem = required(
            solution.decision, max_solution_text_bytes, "the decision it drives", "/decision")) {
        return problem;
    }
    if (auto problem = required(solution.measurement.metric,
                                max_solution_measure_bytes,
                                "the metric its return is measured by",
                                "/measurement/metric")) {
        return problem;
    }
    if (auto problem = optional(solution.measurement.baseline,
                                max_solution_measure_bytes,
                                "the baseline",
                                "/measurement/baseline")) {
        return problem;
    }
    if (auto problem = optional(solution.measurement.target,
                                max_solution_measure_bytes,
                                "the target",
                                "/measurement/target")) {
        return problem;
    }
    if (auto problem =
            optional(solution.pipeline, max_solution_text_bytes, "the pipeline", "/pipeline")) {
        return problem;
    }
    if (auto problem = optional(solution.notes, max_solution_notes_bytes, "the notes", "/notes")) {
        return problem;
    }
    if (solution.methodologies.size() > max_solution_methodologies) {
        return SolutionProblem{"a solution composes at most 32 methodologies", "/methodologies"};
    }
    std::set<std::string> seen;
    for (std::size_t index = 0; index < solution.methodologies.size(); ++index) {
        const SolutionMethodology& methodology = solution.methodologies[index];
        const std::string at = "/methodologies/" + std::to_string(index);
        if (!methodology_id(methodology.id)) {
            return SolutionProblem{"a methodology is named by its id, such as M-1", at + "/id"};
        }
        if (methodology.minimum_level < 1 || methodology.minimum_level > 4) {
            return SolutionProblem{"a methodology's minimum level is 1 to 4",
                                   at + "/minimum_level"};
        }
        if (!seen.insert(methodology.id).second) {
            return SolutionProblem{methodology.id + " is listed twice", at + "/id"};
        }
    }
    return std::nullopt;
}

std::string WorkspaceSolutionCodec::lookup(const WorkspaceSolution& solution) {
    return solutions_lookup(solution.customer_id);
}

nlohmann::json WorkspaceSolutionCodec::encode(const WorkspaceSolution& solution) {
    nlohmann::json methodologies = nlohmann::json::array();
    for (const SolutionMethodology& methodology : solution.methodologies) {
        methodologies.push_back(
            {{"id", methodology.id}, {"minimum_level", methodology.minimum_level}});
    }
    return {{"customer_id", solution.customer_id},
            {"name", solution.name},
            {"problem", solution.problem},
            {"decision", solution.decision},
            {"measurement",
             {{"metric", solution.measurement.metric},
              {"baseline", solution.measurement.baseline},
              {"target", solution.measurement.target}}},
            {"methodologies", std::move(methodologies)},
            {"pipeline", solution.pipeline},
            {"notes", solution.notes},
            {"duplicated_from", solution.duplicated_from},
            {"created_by", solution.created_by},
            {"updated_by", solution.updated_by},
            {"updated_at", solution.updated_at}};
}

std::optional<WorkspaceSolution> WorkspaceSolutionCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    WorkspaceSolution solution;
    solution.id = record.id;
    solution.customer_id = json_text(body, "customer_id");
    solution.name = json_text(body, "name");
    solution.problem = json_text(body, "problem");
    solution.decision = json_text(body, "decision");
    const nlohmann::json measurement = body.value("measurement", nlohmann::json::object());
    solution.measurement = {json_text(measurement, "metric"),
                            json_text(measurement, "baseline"),
                            json_text(measurement, "target")};
    const nlohmann::json methodologies = body.value("methodologies", nlohmann::json::array());
    if (methodologies.is_array()) {
        for (const nlohmann::json& entry : methodologies) {
            if (!entry.is_object()) {
                continue;
            }
            const auto level = entry.find("minimum_level");
            solution.methodologies.push_back(
                {json_text(entry, "id"),
                 level != entry.end() && level->is_number_integer() ? level->get<int>() : 1});
        }
    }
    solution.pipeline = json_text(body, "pipeline");
    solution.notes = json_text(body, "notes");
    solution.duplicated_from = json_text(body, "duplicated_from");
    solution.created_by = json_text(body, "created_by");
    solution.updated_by = json_text(body, "updated_by");
    solution.updated_at = json_text(body, "updated_at");
    solution.created_at = record_instant(record.at_us);
    solution.at_us = record.at_us;
    solution.lake_revision = record.lake_revision;
    if (solution_problem(solution)) {
        return std::nullopt;
    }
    return solution;
}

std::string duplicate_name(const std::string& name) {
    constexpr std::string_view suffix = " (copy)";
    return bounded(name, max_solution_name_bytes - suffix.size()) + std::string(suffix);
}

WorkspaceSolution solution_from_catalogue(const nlohmann::json& entry, const std::string& name) {
    WorkspaceSolution solution;
    const std::string id = json_text(entry, "id");
    const std::string catalogue_name = json_text(entry, "name");
    solution.name = bounded(name.empty() ? catalogue_name : name, max_solution_name_bytes);
    if (solution.name.empty()) {
        solution.name = id;
    }
    const std::string use_case = json_text(entry, "use_case");
    const std::string promise = json_text(entry, "promise");
    solution.problem = bounded(use_case.empty() ? promise : use_case, max_solution_text_bytes);
    solution.decision = bounded(promise.empty() ? use_case : promise, max_solution_text_bytes);

    // The metric is the catalogue's measured outcome, with its unit; its design goes in the notes.
    const nlohmann::json measurement = entry.value("measurement", nlohmann::json::object());
    const nlohmann::json outcome = measurement.is_object()
                                       ? measurement.value("outcome", nlohmann::json::object())
                                       : nlohmann::json::object();
    std::string metric = json_text(outcome, "name");
    if (const std::string unit = json_text(outcome, "unit"); !metric.empty() && !unit.empty()) {
        metric += " (" + unit + ")";
    }
    const std::string design = json_text(entry, "measurement_design");
    if (metric.empty()) {
        metric = design;
    }
    if (metric.empty()) {
        metric = "the return of " + (catalogue_name.empty() ? id : catalogue_name);
    }
    solution.measurement.metric = bounded(metric, max_solution_measure_bytes);

    const nlohmann::json composition = entry.value("composition", nlohmann::json::array());
    if (composition.is_array()) {
        std::set<std::string> seen;
        for (const nlohmann::json& item : composition) {
            std::string methodology =
                item.is_string() ? item.get<std::string>() : json_text(item, "methodology");
            if (methodology.empty()) {
                methodology = json_text(item, "id");
            }
            int level = 1;
            if (item.is_object()) {
                const auto found = item.find("minimum_level");
                if (found != item.end() && found->is_number_integer()) {
                    level = std::clamp(found->get<int>(), 1, 4);
                }
            }
            if (methodology_id(methodology) && seen.insert(methodology).second &&
                solution.methodologies.size() < max_solution_methodologies) {
                solution.methodologies.push_back({methodology, level});
            }
        }
    }

    const nlohmann::json order = entry.value("pipeline_order", nlohmann::json::array());
    if (order.is_array() && !order.empty()) {
        std::string steps;
        for (const nlohmann::json& step : order) {
            if (step.is_string()) {
                steps += (steps.empty() ? "" : ", ") + step.get<std::string>();
            }
        }
        if (!steps.empty()) {
            solution.pipeline =
                bounded("Methodologies in order: " + steps, max_solution_text_bytes);
        }
    }

    std::string notes = "Duplicated from Orama's " + id +
                        (catalogue_name.empty() ? std::string() : " " + catalogue_name) + ".";
    if (!design.empty()) {
        notes += "\n\nHow Orama measures it: " + design;
    }
    if (const std::string policy = json_text(entry, "decision_policy_template"); !policy.empty()) {
        notes += "\n\nDecision policy: " + policy;
    }
    solution.notes = bounded(std::move(notes), max_solution_notes_bytes);
    solution.duplicated_from = id;
    return solution;
}

bool empty_change(const SolutionChange& change) {
    return !change.name && !change.problem && !change.decision && !change.measurement &&
           !change.methodologies && !change.pipeline && !change.notes;
}

void apply_change(WorkspaceSolution& solution, const SolutionChange& change, std::int64_t now_us) {
    if (change.name) {
        solution.name = *change.name;
    }
    if (change.problem) {
        solution.problem = *change.problem;
    }
    if (change.decision) {
        solution.decision = *change.decision;
    }
    if (change.measurement) {
        solution.measurement = *change.measurement;
    }
    if (change.methodologies) {
        solution.methodologies = *change.methodologies;
    }
    if (change.pipeline) {
        solution.pipeline = *change.pipeline;
    }
    if (change.notes) {
        solution.notes = *change.notes;
    }
    solution.updated_by = change.updated_by;
    solution.updated_at = record_instant(now_us);
}

liquid::runtime::task<ZettaControlResult<WorkspaceSolution>>
record_solution(WorkspaceRecords& records, WorkspaceSolution solution) {
    if (auto problem = solution_problem(solution)) {
        co_return refused<WorkspaceSolution>("invalid_input", problem->message);
    }
    WorkspaceSolutions set(records);
    co_return settled(co_await set.append(std::move(solution)));
}

liquid::runtime::task<ZettaControlResult<std::vector<WorkspaceSolution>>>
list_workspace_solutions(WorkspaceRecords& records, std::string customer_id) {
    WorkspaceSolutions set(records);
    auto listed = settled(co_await set.list(solutions_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<WorkspaceSolution>>(
            std::unexpected(listed.error()));
    }
    std::erase_if(*listed,
                  [&](const WorkspaceSolution& row) { return row.customer_id != customer_id; });
    std::ranges::sort(*listed, [](const WorkspaceSolution& a, const WorkspaceSolution& b) {
        const std::string left = folded(a.name);
        const std::string right = folded(b.name);
        if (left != right) {
            return left < right;
        }
        return a.at_us != b.at_us ? a.at_us > b.at_us : a.id > b.id;
    });
    co_return ZettaControlResult<std::vector<WorkspaceSolution>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceSolution>>>
get_workspace_solution(WorkspaceRecords& records, std::string customer_id, std::string id) {
    using Found = std::optional<WorkspaceSolution>;
    auto read = settled(co_await read_own(records, customer_id, id));
    if (!read) {
        if (read.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(read.error()));
    }
    co_return ZettaControlResult<Found>(std::move(read->solution));
}

liquid::runtime::task<ZettaControlResult<WorkspaceSolution>> edit_workspace_solution(
    WorkspaceRecords& records, std::string customer_id, std::string id, SolutionChange change) {
    ZettaControlError last{"conflict", "the solution kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto read = settled(co_await read_own(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<WorkspaceSolution>(std::unexpected(read.error()));
        }
        // Its space never changes, so the write below keeps its lookup.
        WorkspaceSolution next = *read->solution;
        apply_change(next, change, now_microseconds());
        if (auto problem = solution_problem(next)) {
            co_return refused<WorkspaceSolution>("invalid_input", problem->message);
        }
        auto written = settled(co_await records.put(WorkspaceSolutionCodec::collection,
                                                    id,
                                                    WorkspaceSolutionCodec::lookup(next),
                                                    WorkspaceSolutionCodec::encode(next),
                                                    RecordCondition{.if_revision = read->revision},
                                                    next.lake_revision + 1,
                                                    next.at_us));
        if (written) {
            auto decoded = WorkspaceSolutionCodec::decode(*written);
            if (!decoded) {
                co_return refused<WorkspaceSolution>("store_error",
                                                     "the stored record did not decode");
            }
            co_return ZettaControlResult<WorkspaceSolution>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<WorkspaceSolution>(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return ZettaControlResult<WorkspaceSolution>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<void>>
remove_workspace_solution(WorkspaceRecords& records, std::string customer_id, std::string id) {
    ZettaControlError last{"conflict", "the solution kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto read = settled(co_await read_own(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<void>(std::unexpected(read.error()));
        }
        auto removed = settled(
            co_await records.remove(WorkspaceSolutionCodec::collection, id, read->revision));
        if (removed || removed.error().code == "not_found") {
            co_return ZettaControlResult<void>();
        }
        if (removed.error().code != "conflict") {
            co_return ZettaControlResult<void>(std::unexpected(removed.error()));
        }
        last = removed.error();
    }
    co_return ZettaControlResult<void>(std::unexpected(last));
}

} // namespace lake::control
