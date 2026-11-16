// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A workspace's own AI solution (docs/api/solutions.md, lake#597): one a person wrote, or
// duplicated from Orama's catalogue or from another of the workspace's own, in the catalogue's
// shape as far as a person can author it. It lives in the workspace Zetta of its workspace, in the
// sealed collection `lake_solutions`, found by its space, so another workspace never sees it.
// Orama's catalogue solutions (S-1, S-2, …) are the engine's and are never stored here; they are
// read-only.
struct SolutionMethodology {
    std::string id;        // a methodology id, such as "M-1"
    int minimum_level = 1; // the level from which the solution uses it, 1 to 4
    friend bool operator==(const SolutionMethodology&, const SolutionMethodology&) = default;
};

// How the solution's return is measured: the metric, where it stands today and where it should go.
struct SolutionMeasurement {
    std::string metric;
    std::string baseline;
    std::string target;
    friend bool operator==(const SolutionMeasurement&, const SolutionMeasurement&) = default;
};

struct WorkspaceSolution {
    std::string id;
    std::string customer_id;
    std::string name;
    std::string problem;  // the business problem it answers
    std::string decision; // the decision it drives
    SolutionMeasurement measurement;
    std::vector<SolutionMethodology> methodologies;
    std::string pipeline;        // a pipeline template or reference, or empty
    std::string notes;           // or empty
    std::string duplicated_from; // "S-1" (Orama's), a workspace solution id, or empty
    std::string created_by;      // the user id (or the operator's identity) that created it
    std::string created_at;      // "YYYY-MM-DDTHH:MM:SSZ"
    std::string updated_by;      // empty until it is edited
    std::string updated_at;      // empty until it is edited
    std::int64_t at_us = 0;
    std::int64_t lake_revision = 1;
};

struct WorkspaceSolutionCodec {
    using Value = WorkspaceSolution;
    static constexpr RecordCollection collection{"solutions", 1};

    static std::string lookup(const WorkspaceSolution& solution);
    static nlohmann::json encode(const WorkspaceSolution& solution);
    static std::optional<WorkspaceSolution> decode(const StoredRecord& record);
};

using WorkspaceSolutions = RecordSet<WorkspaceSolutionCodec>;

// The bounds a solution's fields keep, in bytes.
inline constexpr std::size_t max_solution_name_bytes = 200;
inline constexpr std::size_t max_solution_text_bytes = 4000;   // problem, decision, pipeline
inline constexpr std::size_t max_solution_measure_bytes = 300; // metric, baseline, target
inline constexpr std::size_t max_solution_notes_bytes = 20000;
inline constexpr std::size_t max_solution_methodologies = 32;

// What is wrong with a solution, and the input field at fault (a JSON pointer).
struct SolutionProblem {
    std::string message;
    std::string path;
};

// Refuses a solution without a space, a name, a problem, a decision or a metric, one whose fields
// are longer than their bounds, or one whose methodologies are not distinct ids with levels 1-4.
std::optional<SolutionProblem> solution_problem(const WorkspaceSolution& solution);

// Whether `id` can name a workspace solution: lake gives them UUIDs, and Orama's catalogue ids
// (S-1, …) never are.
bool workspace_solution_id(std::string_view id);

// The lookup key of a space's solutions in a workspace.
std::string solutions_lookup(const std::string& customer_id);

// A workspace solution from one of Orama's catalogue entries (the engine's get_solution answer):
// its name (or `name` when given), problem, decision, measurement, methodologies and pipeline
// order, ready to edit. `duplicated_from` is the catalogue id.
WorkspaceSolution solution_from_catalogue(const nlohmann::json& entry, const std::string& name);

// The name a duplicate takes when the person gives none: "<name> (copy)", within the bound.
std::string duplicate_name(const std::string& name);

// The fields an edit changes; an absent one is kept.
struct SolutionChange {
    std::optional<std::string> name;
    std::optional<std::string> problem;
    std::optional<std::string> decision;
    std::optional<SolutionMeasurement> measurement;
    std::optional<std::vector<SolutionMethodology>> methodologies;
    std::optional<std::string> pipeline;
    std::optional<std::string> notes;
    std::string updated_by;
};
bool empty_change(const SolutionChange& change);
void apply_change(WorkspaceSolution& solution, const SolutionChange& change, std::int64_t now_us);

// Writes a new solution under a new id; answers it as stored.
liquid::runtime::task<ZettaControlResult<WorkspaceSolution>>
record_solution(WorkspaceRecords& records, WorkspaceSolution solution);

// Every solution of the space in this workspace, by name (then newest first).
liquid::runtime::task<ZettaControlResult<std::vector<WorkspaceSolution>>>
list_workspace_solutions(WorkspaceRecords& records, std::string customer_id);

// One solution of the space by id; nullopt when this workspace holds none.
liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceSolution>>>
get_workspace_solution(WorkspaceRecords& records, std::string customer_id, std::string id);

// Changes a solution with a write conditional on the revision it read, rereading on a conflict.
// `not_found` when this workspace holds no such solution of the space.
liquid::runtime::task<ZettaControlResult<WorkspaceSolution>> edit_workspace_solution(
    WorkspaceRecords& records, std::string customer_id, std::string id, SolutionChange change);

// Deletes a solution at the revision read. `not_found` when this workspace holds none.
liquid::runtime::task<ZettaControlResult<void>>
remove_workspace_solution(WorkspaceRecords& records, std::string customer_id, std::string id);

} // namespace lake::control
