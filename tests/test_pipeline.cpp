// SPDX-License-Identifier: MIT

#include <chrono>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/pipeline.hpp"

namespace pl = lake::api::pipeline;
using json = nlohmann::json;

namespace {

// What get_graph returns for a small churn pipeline.
json churn() {
    return {{"graph_id", "g-1"},
            {"name", "churn"},
            {"project", "retention"},
            {"approval", "approved"},
            {"nodes",
             json::array({{{"id", "pull"},
                           {"kind", "connect"},
                           {"name", "Pull accounts"},
                           {"attributes", {{"source", "warehouse"}, {"table", "customers"}}}},
                          {{"id", "tidy"}, {"kind", "clean"}, {"name", "Tidy"}, {"attributes", json::object()}},
                          {{"id", "train"},
                           {"kind", "model"},
                           {"name", "Train churn"},
                           {"attributes", {{"target", "churned"}, {"algo", "gradient_boosting"}}}},
                          {{"id", "rank"}, {"kind", "serve"}, {"name", "Rank at risk"}, {"attributes", json::object()}}})},
            {"edges",
             json::array({{{"from", "pull"}, {"to", "tidy"}, {"kind", "data_flow"}},
                          {{"from", "tidy"}, {"to", "train"}, {"kind", "data_flow"}},
                          {{"from", "train"}, {"to", "rank"}, {"kind", "data_flow"}}})}};
}

pl::Graph parsed() {
    auto graph = pl::from_json(churn());
    EXPECT_TRUE(graph) << graph.error();
    return *graph;
}

} // namespace

TEST(Pipeline, ParsesGetGraphAndRoundTripsWhatSaveGraphTakes) {
    const pl::Graph graph = parsed();
    EXPECT_EQ(graph.name, "churn");
    EXPECT_EQ(graph.approval, "approved");
    ASSERT_EQ(graph.steps.size(), 4U);
    ASSERT_EQ(graph.edges.size(), 3U);
    EXPECT_EQ(pl::steps_json(graph), churn()["nodes"]);
    EXPECT_EQ(pl::edges_json(graph), churn()["edges"]);
    EXPECT_EQ(pl::validate(graph), "");
    EXPECT_EQ(pl::topological_order(graph),
              (std::vector<std::string>{"pull", "tidy", "train", "rank"}));
}

TEST(Pipeline, EditsChangeTheGraphAndKeepItValid) {
    auto added = pl::apply(parsed(), {.op = "add_step", .kind = "act", .name = "Notify CSMs"});
    ASSERT_TRUE(added) << added.error();
    ASSERT_EQ(added->steps.size(), 5U);
    EXPECT_EQ(added->steps.back().id, "act-1");
    auto joined = pl::apply(*added, {.op = "add_edge", .from = "rank", .to = "act-1"});
    ASSERT_TRUE(joined) << joined.error();
    EXPECT_EQ(joined->edges.back().kind, "data_flow");

    auto renamed = pl::apply(*joined, {.op = "rename_step", .step = "tidy", .name = "Deduplicate"});
    ASSERT_TRUE(renamed);
    EXPECT_EQ(renamed->step("tidy")->name, "Deduplicate");
    auto rekinded = pl::apply(*renamed, {.op = "kind_step", .step = "rank", .kind = "index"});
    ASSERT_TRUE(rekinded);
    EXPECT_EQ(rekinded->step("rank")->kind, "index");

    auto removed = pl::apply(*rekinded, {.op = "remove_step", .step = "tidy"});
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->step("tidy"), nullptr);
    EXPECT_EQ(removed->edges.size(), 2U) << "the step's edges go with it";

    auto unjoined = pl::apply(*removed, {.op = "remove_edge", .from = "rank", .to = "act-1"});
    ASSERT_TRUE(unjoined);
    EXPECT_EQ(unjoined->edges.size(), 1U);
    auto titled = pl::apply(*unjoined, {.op = "rename", .name = "churn v2"});
    ASSERT_TRUE(titled);
    EXPECT_EQ(titled->name, "churn v2");
}

TEST(Pipeline, EditsThatWouldBreakTheGraphAreRefusedWithTheReason) {
    const auto cycle = pl::apply(parsed(), {.op = "add_edge", .from = "rank", .to = "pull"});
    ASSERT_FALSE(cycle);
    EXPECT_NE(cycle.error().find("cycle"), std::string::npos);
    const auto self = pl::apply(parsed(), {.op = "add_edge", .from = "tidy", .to = "tidy"});
    ASSERT_FALSE(self);
    const auto dangling = pl::apply(parsed(), {.op = "add_edge", .from = "pull", .to = "nowhere"});
    ASSERT_FALSE(dangling);
    EXPECT_NE(dangling.error().find("does not exist"), std::string::npos);
    const auto twice = pl::apply(parsed(), {.op = "add_edge", .from = "pull", .to = "tidy"});
    ASSERT_FALSE(twice);
    EXPECT_FALSE(pl::apply(parsed(), {.op = "kind_step", .step = "tidy", .kind = "teleport"}));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "add_step", .kind = ""}));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "rename_step", .step = "tidy", .name = ""}));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "remove_step", .step = "ghost"}));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "remove_edge", .from = "pull", .to = "train"}));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "explode"}));
}

TEST(Pipeline, RendersLanesStepsEdgesStatusAndSelection) {
    const std::string svg = pl::render_svg(parsed(),
                                           {{"train", "succeeded"}, {"rank", "failed"}},
                                           "tidy",
                                           "/design/pipeline?x=1&step=");
    EXPECT_TRUE(svg.starts_with("<svg class=\"pipeline-graph\""));
    for (const char* lane : {">connect<", ">clean<", ">model<", ">serve<"}) {
        EXPECT_NE(svg.find(lane), std::string::npos) << lane;
    }
    EXPECT_EQ(svg.find(">act<"), std::string::npos) << "a stage with no steps has no lane";
    EXPECT_EQ(std::count(svg.begin(), svg.end(), '\n'), 0);
    size_t edges = 0;
    for (size_t at = svg.find("<line class=\"edge"); at != std::string::npos;
         at = svg.find("<line class=\"edge", at + 1)) {
        ++edges;
    }
    EXPECT_EQ(edges, 3U);
    EXPECT_NE(svg.find("step-model status-succeeded"), std::string::npos);
    EXPECT_NE(svg.find("step-serve status-failed"), std::string::npos);
    EXPECT_NE(svg.find("step-clean selected"), std::string::npos);
    EXPECT_NE(svg.find("href=\"/design/pipeline?x=1&amp;step=train\""), std::string::npos);

    json hostile = churn();
    hostile["nodes"][0]["name"] = "<script>alert(1)</script>";
    const std::string escaped = pl::render_svg(*pl::from_json(hostile), {}, "", "?step=");
    EXPECT_EQ(escaped.find("<script>"), std::string::npos);

    // LSR-193 (lake#999): a run status from the engine cannot leave the class attribute.
    const std::string injected = pl::render_svg(
        parsed(), {{"train", "x\" onmouseover=\"alert(1)"}}, "", "?step=");
    EXPECT_EQ(injected.find("onmouseover=\""), std::string::npos);
    EXPECT_NE(injected.find("step-model status-unknown"), std::string::npos);
}

TEST(Pipeline, EveryEditKeepsWhatEachStepDoes) {
    // Found by the UX review 2026-09-27: saving a renamed step dropped every step's source, table,
    // target and algorithm, silently changing what the pipeline runs.
    auto renamed = pl::apply(parsed(), {.op = "rename_step", .step = "tidy", .name = "Clean"});
    ASSERT_TRUE(renamed) << renamed.error();
    const json saved = pl::steps_json(*renamed);
    EXPECT_EQ(saved[0]["attributes"]["table"], "customers");
    EXPECT_EQ(saved[2]["attributes"]["algo"], "gradient_boosting");
}

TEST(Pipeline, SetAttributeChangesOrClearsOneSetting) {
    auto pointed = pl::apply(parsed(),
                             {.op = "set_attribute", .step = "pull", .name = "table",
                              .value = "ux_review_churn"});
    ASSERT_TRUE(pointed) << pointed.error();
    EXPECT_EQ(pointed->step("pull")->attributes["table"], "ux_review_churn");
    auto cleared = pl::apply(*pointed, {.op = "set_attribute", .step = "pull", .name = "source"});
    ASSERT_TRUE(cleared);
    EXPECT_FALSE(cleared->step("pull")->attributes.contains("source"));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "set_attribute", .step = "nope", .name = "x"}));
    EXPECT_FALSE(pl::apply(parsed(), {.op = "set_attribute", .step = "pull"}));
}

TEST(Pipeline, ApplyAllRunsEditsInOrderAllOrNothing) {
    const json edits = json::array({{{"op", "add_step"}, {"kind", "act"}, {"name", "Notify"},
                                     {"id", "notify"}},
                                    {{"op", "add_edge"}, {"from", "rank"}, {"to", "notify"}},
                                    {{"op", "set_attribute"}, {"step", "train"},
                                     {"name", "target"}, {"value", "cancelled"}}});
    auto applied = pl::apply_all(parsed(), edits);
    ASSERT_TRUE(applied) << applied.error();
    ASSERT_NE(applied->step("notify"), nullptr) << "a later edit can use the id the add named";
    EXPECT_EQ(applied->edges.back().to, "notify");
    EXPECT_EQ(applied->step("train")->attributes["target"], "cancelled");

    const auto refused = pl::apply_all(
        parsed(), json::array({{{"op", "remove_step"}, {"step", "tidy"}},
                               {{"op", "remove_edge"}, {"from", "tidy"}, {"to", "train"}}}));
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("edit 2 (remove_edge)"), std::string::npos) << refused.error();
    EXPECT_FALSE(pl::apply_all(parsed(), json::array()));
}

TEST(Pipeline, FiftyThousandReversedStepsValidateWithinOneSecond) {
    pl::Graph graph;
    for (std::size_t index = pl::maximum_steps; index > 0; --index) {
        graph.steps.push_back({std::to_string(index), "connect", "Step"});
        if (index > 1) {
            graph.edges.push_back({std::to_string(index - 1), std::to_string(index), "data_flow"});
        }
    }
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(pl::validate(graph).empty());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    const auto order = pl::topological_order(graph);
    ASSERT_EQ(order.size(), pl::maximum_steps);
    EXPECT_EQ(order.front(), "1");
    EXPECT_EQ(order.back(), std::to_string(pl::maximum_steps));
    const auto batch_start = std::chrono::steady_clock::now();
    const auto applied = pl::apply_all(
        std::move(graph),
        json::array_t(pl::maximum_edits, json{{"op", "rename"}, {"name", "renamed"}}));
    ASSERT_TRUE(applied) << applied.error();
    EXPECT_LT(std::chrono::steady_clock::now() - batch_start, std::chrono::seconds(1));
}

TEST(Pipeline, ReadyStepsKeepTheEarliestOriginalIndex) {
    pl::Graph graph{
        .steps = {{"first", "connect", ""}, {"second", "connect", ""}, {"third", "connect", ""}},
        .edges = {{"second", "first", "data_flow"}}};
    EXPECT_EQ(pl::topological_order(graph), (std::vector<std::string>{"second", "first", "third"}));
    graph.edges.push_back({"first", "second", "control"});
    EXPECT_TRUE(pl::topological_order(graph).empty());
    EXPECT_FALSE(pl::validate(graph).empty());
}

TEST(Pipeline, GraphAndEditBudgetsAreCheckedBeforeWorkAndDuringGrowth) {
    json value = churn();
    value["nodes"] = json::array_t(pl::maximum_steps + 1, value["nodes"][0]);
    EXPECT_FALSE(pl::from_json(value));
    value = churn();
    value["edges"] = json::array_t(pl::maximum_edges + 1, value["edges"][0]);
    EXPECT_FALSE(pl::from_json(value));
    value["edges"] = json::object();
    EXPECT_FALSE(pl::from_json(value));
    EXPECT_FALSE(pl::apply_all(
        parsed(),
        json::array_t(pl::maximum_edits + 1, json{{"op", "rename"}, {"name", "renamed"}})));

    pl::Graph graph;
    graph.steps.resize(pl::maximum_steps);
    EXPECT_FALSE(pl::apply(graph, {.op = "add_step", .kind = "connect"}));
    graph.steps.emplace_back();
    EXPECT_FALSE(pl::validate(graph).empty());
    EXPECT_FALSE(pl::apply(graph, {.op = "remove_step", .step = ""}));
    EXPECT_FALSE(pl::apply_all(graph, json::array({{{"op", "rename"}, {"name", "renamed"}}})));
    EXPECT_TRUE(pl::render_svg(graph, {}, "", "").empty());
    graph.steps.clear();
    graph.edges.resize(pl::maximum_edges);
    EXPECT_FALSE(pl::apply(graph, {.op = "add_edge", .from = "x", .to = "y"}));
}

TEST(Pipeline, BatchValidatesTheFinalGraphOnceAndKeepsAtomicity) {
    // An edge may be introduced before its target in an atomic proposal.
    const json edits = json::array({{{"op", "add_edge"}, {"from", "rank"}, {"to", "notify"}},
                                    {{"op", "add_step"}, {"id", "notify"}, {"kind", "act"}}});
    auto applied = pl::apply_all(parsed(), edits);
    ASSERT_TRUE(applied) << applied.error();
    EXPECT_TRUE(pl::validate(*applied).empty());
    const auto cycle =
        pl::apply_all(parsed(),
                      json::array({{{"op", "rename"}, {"name", "changed"}},
                                   {{"op", "add_edge"}, {"from", "rank"}, {"to", "pull"}}}));
    EXPECT_FALSE(cycle);
    EXPECT_EQ(parsed().name, "churn");
}
