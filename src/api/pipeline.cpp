#include "lake/api/pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <set>
#include <span>

#include "lake/api/web_ui.hpp"

namespace lake::api::pipeline {
namespace {

std::string text(const nlohmann::json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool known(std::string_view value, std::span<const std::string_view> names) {
    return std::ranges::find(names, value) != names.end();
}

std::string url_encode(std::string_view input) {
    std::string output;
    for (const unsigned char character : input) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output += static_cast<char>(character);
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", character);
            output += buffer;
        }
    }
    return output;
}

// A new step id: the kind and the first free number, so ids stay readable ("model-2").
std::string fresh_id(const Graph& graph, std::string_view kind) {
    for (int number = 1;; ++number) {
        std::string candidate = std::format("{}-{}", kind, number);
        if (graph.step(candidate) == nullptr) {
            return candidate;
        }
    }
}

} // namespace

const Step* Graph::step(std::string_view id) const {
    for (const Step& candidate : steps) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

std::expected<Graph, std::string> from_json(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::unexpected(std::string("the pipeline is not an object"));
    }
    Graph graph;
    graph.graph_id = text(value, "graph_id");
    graph.name = text(value, "name");
    graph.project = text(value, "project");
    graph.approval = text(value, "approval");
    graph.approved_by = text(value, "approved_by");
    graph.approved_at = text(value, "approved_at");
    for (const nlohmann::json& node : value.value("nodes", nlohmann::json::array())) {
        if (!node.is_object()) {
            return std::unexpected(std::string("a step is not an object"));
        }
        graph.steps.push_back({text(node, "id"), text(node, "kind"), text(node, "name")});
    }
    for (const nlohmann::json& edge : value.value("edges", nlohmann::json::array())) {
        if (!edge.is_object()) {
            return std::unexpected(std::string("an edge is not an object"));
        }
        graph.edges.push_back({text(edge, "from"), text(edge, "to"), text(edge, "kind")});
    }
    return graph;
}

nlohmann::json steps_json(const Graph& graph) {
    nlohmann::json out = nlohmann::json::array();
    for (const Step& step : graph.steps) {
        out.push_back({{"id", step.id}, {"kind", step.kind}, {"name", step.name}});
    }
    return out;
}

nlohmann::json edges_json(const Graph& graph) {
    nlohmann::json out = nlohmann::json::array();
    for (const Edge& edge : graph.edges) {
        out.push_back({{"from", edge.from}, {"to", edge.to}, {"kind", edge.kind}});
    }
    return out;
}

std::vector<std::string> topological_order(const Graph& graph) {
    std::map<std::string, int> incoming;
    for (const Step& step : graph.steps) {
        incoming[step.id] = 0;
    }
    for (const Edge& edge : graph.edges) {
        if (incoming.contains(edge.to) && incoming.contains(edge.from)) {
            ++incoming[edge.to];
        }
    }
    std::vector<std::string> order;
    std::set<std::string> placed;
    // Repeatedly take the first step, in graph order, whose predecessors are all placed.
    while (order.size() < graph.steps.size()) {
        bool progressed = false;
        for (const Step& step : graph.steps) {
            if (placed.contains(step.id) || incoming[step.id] != 0) {
                continue;
            }
            order.push_back(step.id);
            placed.insert(step.id);
            for (const Edge& edge : graph.edges) {
                if (edge.from == step.id && incoming.contains(edge.to)) {
                    --incoming[edge.to];
                }
            }
            progressed = true;
            break;
        }
        if (!progressed) {
            return {};
        }
    }
    return order;
}

std::string validate(const Graph& graph) {
    std::set<std::string> ids;
    for (const Step& step : graph.steps) {
        if (step.id.empty()) {
            return "every step needs an id";
        }
        if (!ids.insert(step.id).second) {
            return "two steps share the id " + step.id;
        }
        if (!known(step.kind, step_kinds)) {
            return "step " + step.id + " has an unknown kind '" + step.kind + "'";
        }
    }
    std::set<std::pair<std::string, std::string>> joined;
    for (const Edge& edge : graph.edges) {
        if (!ids.contains(edge.from) || !ids.contains(edge.to)) {
            return "an edge joins a step that does not exist (" + edge.from + " → " + edge.to + ")";
        }
        if (edge.from == edge.to) {
            return "step " + edge.from + " cannot lead to itself";
        }
        if (!known(edge.kind, edge_kinds)) {
            return "the edge " + edge.from + " → " + edge.to + " has an unknown kind '" +
                   edge.kind + "'";
        }
        if (!joined.insert({edge.from, edge.to}).second) {
            return "there are two edges from " + edge.from + " to " + edge.to;
        }
    }
    if (topological_order(graph).size() != graph.steps.size()) {
        return "the edges would make a cycle; a pipeline must flow one way";
    }
    return {};
}

std::expected<Graph, std::string> apply(Graph graph, const Edit& edit) {
    const auto find = [&](const std::string& id) {
        return std::ranges::find_if(graph.steps, [&](const Step& step) { return step.id == id; });
    };
    if (edit.op == "add_step") {
        if (!known(edit.kind, step_kinds)) {
            return std::unexpected("choose a step kind");
        }
        std::string name = edit.name.empty() ? edit.kind : edit.name;
        graph.steps.push_back({fresh_id(graph, edit.kind), edit.kind, std::move(name)});
    } else if (edit.op == "rename_step" || edit.op == "kind_step") {
        const auto step = find(edit.step);
        if (step == graph.steps.end()) {
            return std::unexpected("there is no step " + edit.step);
        }
        if (edit.op == "rename_step") {
            if (edit.name.empty()) {
                return std::unexpected("a step needs a name");
            }
            step->name = edit.name;
        } else {
            step->kind = edit.kind;
        }
    } else if (edit.op == "remove_step") {
        const auto step = find(edit.step);
        if (step == graph.steps.end()) {
            return std::unexpected("there is no step " + edit.step);
        }
        graph.steps.erase(step);
        std::erase_if(graph.edges, [&](const Edge& edge) {
            return edge.from == edit.step || edge.to == edit.step;
        });
    } else if (edit.op == "add_edge") {
        graph.edges.push_back(
            {edit.from, edit.to, edit.kind.empty() ? std::string("data_flow") : edit.kind});
    } else if (edit.op == "remove_edge") {
        const auto before = graph.edges.size();
        std::erase_if(graph.edges, [&](const Edge& edge) {
            return edge.from == edit.from && edge.to == edit.to;
        });
        if (graph.edges.size() == before) {
            return std::unexpected("there is no edge from " + edit.from + " to " + edit.to);
        }
    } else if (edit.op == "rename") {
        if (edit.name.empty()) {
            return std::unexpected("a pipeline needs a name");
        }
        graph.name = edit.name;
    } else {
        return std::unexpected("unknown edit '" + edit.op + "'");
    }
    if (std::string problem = validate(graph); !problem.empty()) {
        return std::unexpected(std::move(problem));
    }
    return graph;
}

std::string render_svg(const Graph& graph,
                       const std::map<std::string, std::string>& status,
                       std::string_view selected,
                       std::string_view link_base) {
    constexpr int column_width = 200, box_width = 164, box_height = 56, row_height = 84, top = 36,
                  left = 18;
    // Columns: the stage kinds that have steps, in canonical order; a step of an unknown kind goes
    // in a last column so a graph Zetta sent is always drawn.
    std::vector<std::string> columns;
    for (const std::string_view kind : step_kinds) {
        if (std::ranges::any_of(graph.steps, [&](const Step& s) { return s.kind == kind; })) {
            columns.emplace_back(kind);
        }
    }
    if (std::ranges::any_of(graph.steps,
                            [](const Step& s) { return !known(s.kind, step_kinds); })) {
        columns.emplace_back("other");
    }
    std::vector<std::string> order = topological_order(graph);
    if (order.empty()) {
        for (const Step& step : graph.steps) {
            order.push_back(step.id);
        }
    }
    std::map<std::string, std::pair<int, int>> position; // id → (x, y) of the box's top-left
    std::map<std::string, int> filled;
    for (const std::string& id : order) {
        const Step* step = graph.step(id);
        const std::string column = known(step->kind, step_kinds) ? step->kind : "other";
        const auto index = std::ranges::find(columns, column) - columns.begin();
        position[id] = {left + static_cast<int>(index) * column_width,
                        top + filled[column]++ * row_height};
    }
    int rows = 1;
    for (const auto& [column, count] : filled) {
        rows = std::max(rows, count);
    }
    const int width = std::max(1, static_cast<int>(columns.size())) * column_width + left;
    const int height = top + rows * row_height;

    std::string svg = std::format(
        "<svg class=\"pipeline-graph\" role=\"img\" viewBox=\"0 0 {} {}\" data-width=\"{}\" "
        "data-height=\"{}\" xmlns=\"http://www.w3.org/2000/svg\"><title>{} — {} steps, {} "
        "edges</title><defs><marker id=\"arrow\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" "
        "markerWidth=\"7\" markerHeight=\"7\" orient=\"auto-start-reverse\"><path d=\"M 0 0 L 10 "
        "5 L 0 10 z\" fill=\"currentColor\"/></marker></defs>",
        width,
        height,
        width,
        height,
        web_ui_escape(graph.name),
        graph.steps.size(),
        graph.edges.size());
    for (std::size_t index = 0; index < columns.size(); ++index) {
        svg += std::format("<text class=\"lane\" x=\"{}\" y=\"20\">{}</text>",
                           left + static_cast<int>(index) * column_width,
                           web_ui_escape(columns[index]));
    }
    for (const Edge& edge : graph.edges) {
        if (!position.contains(edge.from) || !position.contains(edge.to)) {
            continue;
        }
        const auto [fx, fy] = position[edge.from];
        const auto [tx, ty] = position[edge.to];
        // From the right edge of the source to the left edge of the target, or, within one
        // column, from the bottom of one box to the top of the next.
        const bool same_column = fx == tx;
        const int x1 = same_column ? fx + box_width / 2 : fx + box_width;
        const int y1 = same_column ? fy + box_height : fy + box_height / 2;
        const int x2 = same_column ? tx + box_width / 2 : tx;
        const int y2 = same_column ? ty : ty + box_height / 2;
        svg += std::format("<line class=\"edge {}\" x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" "
                           "marker-end=\"url(#arrow)\"><title>{} → {} ({})</title></line>",
                           edge.kind == "control" ? "edge-control" : "edge-data",
                           x1,
                           y1,
                           x2,
                           y2,
                           web_ui_escape(edge.from),
                           web_ui_escape(edge.to),
                           web_ui_escape(edge.kind));
    }
    for (const Step& step : graph.steps) {
        const auto [x, y] = position[step.id];
        const auto found = status.find(step.id);
        const std::string outcome = found == status.end() ? std::string() : found->second;
        svg += std::format(
            "<a href=\"{}{}\"><g class=\"step step-{}{}{}\"><rect x=\"{}\" y=\"{}\" width=\"{}\" "
            "height=\"{}\" rx=\"8\"/><text x=\"{}\" y=\"{}\" class=\"step-name\">{}</text><text "
            "x=\"{}\" y=\"{}\" class=\"step-meta\">{}{}</text><title>{} ({}){}</title></g></a>",
            web_ui_escape(link_base),
            url_encode(step.id),
            known(step.kind, step_kinds) ? step.kind : "other",
            outcome.empty() ? "" : " status-" + outcome,
            step.id == selected ? " selected" : "",
            x,
            y,
            box_width,
            box_height,
            x + 12,
            y + 24,
            web_ui_escape(step.name.size() > 22 ? step.name.substr(0, 21) + "…" : step.name),
            x + 12,
            y + 43,
            web_ui_escape(step.id),
            outcome.empty() ? "" : " · " + web_ui_escape(outcome),
            web_ui_escape(step.name),
            web_ui_escape(step.kind),
            outcome.empty() ? "" : ", last run " + web_ui_escape(outcome));
    }
    return svg + "</svg>";
}

} // namespace lake::api::pipeline
