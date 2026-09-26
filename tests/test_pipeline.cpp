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
             json::array({{{"id", "pull"}, {"kind", "connect"}, {"name", "Pull accounts"}},
                          {{"id", "tidy"}, {"kind", "clean"}, {"name", "Tidy"}},
                          {{"id", "train"}, {"kind", "model"}, {"name", "Train churn"}},
                          {{"id", "rank"}, {"kind", "serve"}, {"name", "Rank at risk"}}})},
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
}
