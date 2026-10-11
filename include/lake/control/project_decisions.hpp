#pragma once

#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// The decision a project should improve (lake#577, docs/api/workspaces.md#a-projects-decision):
// what a person types into the New project drawer's "The decision it should improve". The
// workspace engine's projects keep a name and a description only, and its `intent` input field is
// a reserved trace note it strips before validation, so lake keeps the decision itself: one sealed
// record in the collection `lake_project_decisions` of the project's workspace Zetta, keyed by the
// engine's project id (so a project deleted and created again under the same name starts with no
// decision) and found by the space.
struct ProjectDecision {
    std::string project_id; // the engine's project uuid: the record's key
    std::string customer_id;
    std::string project; // the project's name when the decision was written
    std::string decision;
    std::string created_by; // the user id (or the operator's identity) that wrote it
    std::string created_at; // "YYYY-MM-DDTHH:MM:SSZ"
};

struct ProjectDecisionCodec {
    using Value = ProjectDecision;
    static constexpr RecordCollection collection{"project_decisions", 1};

    static std::string lookup(const ProjectDecision& decision);
    static nlohmann::json encode(const ProjectDecision& decision);
    static std::optional<ProjectDecision> decode(const StoredRecord& record);
};

// The longest decision lake keeps, in bytes.
inline constexpr std::size_t max_project_decision_bytes = 4000;

// Writes a project's decision once, `if_absent` under its project id. A project id that already
// has one is `conflict`.
liquid::runtime::task<ZettaControlResult<ProjectDecision>>
record_project_decision(WorkspaceRecords& records, ProjectDecision decision);

// A project's decision in the space, or nullopt when it has none (or it belongs to another space).
liquid::runtime::task<ZettaControlResult<std::optional<ProjectDecision>>>
get_project_decision(WorkspaceRecords& records, std::string customer_id, std::string project_id);

// Removes a project's decision; a project without one is not an error.
liquid::runtime::task<ZettaControlResult<void>>
remove_project_decision(WorkspaceRecords& records, std::string customer_id, std::string project_id);

} // namespace lake::control
