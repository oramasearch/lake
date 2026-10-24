#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// A pipeline: one Zetta execution graph (zetta include/zetta/ir/execution_graph.hpp), the central
// object of Design, Train and Ship (docs/design/zetta-parity.md). This is the pure model the
// pipeline page renders and edits: parse what get_graph returns, apply one edit, check the result,
// and produce what save_graph takes. No I/O; the HTTP layer calls Zetta.
namespace lake::api::pipeline {

inline constexpr std::size_t maximum_steps = 50'000;
inline constexpr std::size_t maximum_edges = 100'000;
inline constexpr std::size_t maximum_edits = 256;

// Check counts and array shapes before copying any engine or client graph data.
std::string check_size(const nlohmann::json& value);

// Zetta's step kinds in canonical pipeline order (zetta include/zetta/ir/step_kind.hpp).
inline constexpr std::string_view step_kinds[] = {
    "connect", "clean", "model", "index", "serve", "act"};
// Zetta's edge kinds: data flows from one step to the next, or control only orders them.
inline constexpr std::string_view edge_kinds[] = {"data_flow", "control"};

struct Step {
    std::string id, kind, name;
    // What the step does beyond its kind (a connect step's source and table, a model step's target
    // and algo). Carried through every edit: a save that dropped them would change what runs.
    nlohmann::json attributes = nlohmann::json::object();
};
struct Edge {
    std::string from, to, kind;
};
struct Graph {
    std::string graph_id, name, project, approval, approved_by, approved_at;
    std::vector<Step> steps;
    std::vector<Edge> edges;

    const Step* step(std::string_view id) const;
};

// get_graph's value, or an error naming what is missing.
std::expected<Graph, std::string> from_json(const nlohmann::json& value);
// save_graph's nodes and edges.
nlohmann::json steps_json(const Graph& graph);
nlohmann::json edges_json(const Graph& graph);

// Empty when the graph is one Zetta will accept and lake can draw: every step has a unique
// non-empty id and a known kind, every edge joins two existing steps with a known kind and is not a
// self-loop or a duplicate, and there is no cycle. Otherwise what is wrong, in a sentence.
std::string validate(const Graph& graph);

// One edit, as the pipeline page's forms post it. `op` is one of add_step, rename_step,
// kind_step, remove_step, add_edge, remove_edge, set_attribute (step, name, value), rename; the other fields are read as that op
// needs. The result is validated; an edit that would break the graph is refused with the reason.
struct Edit {
    std::string op, step, kind, name, from, to;
    // set_attribute's value (name is the attribute); null removes the attribute.
    nlohmann::json value = nullptr;
};
std::expected<Graph, std::string> apply(Graph graph, const Edit& edit);
// An edit from its JSON form ({op, step, kind, name, from, to, value}).
Edit edit_from_json(const nlohmann::json& value);
// At most maximum_edits edits in order, all or nothing. Validate the resulting graph once;
// an operation that fails names its position and reason.
std::expected<Graph, std::string> apply_all(Graph graph, const nlohmann::json& edits);

// The steps in dependency order (a step after every step with an edge into it); ties keep the
// graph's own order. Empty when there is a cycle.
std::vector<std::string> topological_order(const Graph& graph);

// The graph as an inline SVG: one column per stage in `step_kinds` order that has steps, the
// steps of a column stacked in dependency order, edges as arrows (data flow solid, control dashed).
// `status` maps a step id to its latest run outcome ("succeeded", "failed", "running", …) and
// colours it; `selected` outlines one step. Every step links to `link_base` + url-encoded step id.
std::string render_svg(const Graph& graph,
                       const std::map<std::string, std::string>& status,
                       std::string_view selected,
                       std::string_view link_base);

} // namespace lake::api::pipeline
