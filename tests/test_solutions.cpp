// A workspace's own AI solutions (docs/api/solutions.md, lake#597): the pure parts. What a
// solution must carry, how one of Orama's catalogue entries becomes an editable copy, the names a
// copy takes, and where their audit goes. The capabilities against workspace records are in
// test_workspace_records.cpp (SolutionsAreTheWorkspacesOwnAndOramasStayReadOnly).

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/audit_routing.hpp"
#include "lake/control/solutions.hpp"

namespace {

using json = nlohmann::json;
using lake::control::WorkspaceSolution;

WorkspaceSolution valid() {
    WorkspaceSolution solution;
    solution.customer_id = "customer-1";
    solution.name = "Late payments";
    solution.problem = "Invoices are paid late.";
    solution.decision = "Who collections calls first.";
    solution.measurement = {"days sales outstanding", "52", "40"};
    solution.methodologies = {{"M-1", 1}, {"M-3", 2}};
    return solution;
}

std::string problem_path(const WorkspaceSolution& solution) {
    const auto problem = lake::control::solution_problem(solution);
    return problem ? problem->path : std::string();
}

TEST(Solutions, ASolutionNeedsItsProblemDecisionAndMetric) {
    EXPECT_FALSE(lake::control::solution_problem(valid()));
    auto solution = valid();
    solution.name.clear();
    EXPECT_EQ(problem_path(solution), "/name");
    solution = valid();
    solution.problem.clear();
    EXPECT_EQ(problem_path(solution), "/problem");
    solution = valid();
    solution.decision.clear();
    EXPECT_EQ(problem_path(solution), "/decision");
    solution = valid();
    solution.measurement.metric.clear();
    EXPECT_EQ(problem_path(solution), "/measurement/metric");
    solution = valid();
    solution.measurement.baseline.clear();
    solution.measurement.target.clear();
    solution.methodologies.clear();
    EXPECT_FALSE(lake::control::solution_problem(solution))
        << "baseline, target, methodologies, pipeline and notes are optional";
    solution = valid();
    solution.customer_id.clear();
    EXPECT_EQ(problem_path(solution), "/customer_id");
}

TEST(Solutions, FieldsKeepTheirBoundsAndMethodologiesAreDistinctIds) {
    auto solution = valid();
    solution.name = std::string(lake::control::max_solution_name_bytes + 1, 'a');
    EXPECT_EQ(problem_path(solution), "/name");
    solution = valid();
    solution.measurement.target = std::string(lake::control::max_solution_measure_bytes + 1, '9');
    EXPECT_EQ(problem_path(solution), "/measurement/target");
    solution = valid();
    solution.notes = std::string(lake::control::max_solution_notes_bytes + 1, 'n');
    EXPECT_EQ(problem_path(solution), "/notes");
    solution = valid();
    solution.methodologies = {{"M-1", 1}, {"M 3", 1}};
    EXPECT_EQ(problem_path(solution), "/methodologies/1/id");
    solution.methodologies = {{"M-1", 0}};
    EXPECT_EQ(problem_path(solution), "/methodologies/0/minimum_level");
    solution.methodologies = {{"M-1", 1}, {"M-2", 4}, {"M-1", 2}};
    EXPECT_EQ(problem_path(solution), "/methodologies/2/id");
    solution.methodologies.assign(lake::control::max_solution_methodologies + 1, {"M-1", 1});
    EXPECT_EQ(problem_path(solution), "/methodologies");
}

TEST(Solutions, OnlyAUuidNamesAWorkspaceSolution) {
    EXPECT_TRUE(lake::control::workspace_solution_id("0b6c3c5e-1f0a-4c55-9d1e-2b7f0d1e9a10"));
    for (const std::string id : {"S-1",
                                 "S-23",
                                 "",
                                 "0b6c3c5e1f0a4c559d1e2b7f0d1e9a10",
                                 "0b6c3c5e-1f0a-4c55-9d1e-2b7f0d1e9a1z"}) {
        EXPECT_FALSE(lake::control::workspace_solution_id(id)) << id;
    }
}

TEST(Solutions, OneOfOramasBecomesAnEditableCopy) {
    const json entry{{"id", "S-4"},
                     {"name", "Demand Forecasting"},
                     {"promise", "Forecast demand and set the order quantity."},
                     {"use_case", "Stock-outs and overstock cost margin."},
                     {"composition",
                      json::array({{{"methodology", "M-7"}, {"minimum_level", 1}},
                                   {{"methodology", "M-7"}, {"minimum_level", 2}},
                                   {{"methodology", "M-9"}, {"minimum_level", 3}}})},
                     {"pipeline_order", json::array({"M-7", "M-9"})},
                     {"measurement_design", "Forecast value added against a naive baseline."},
                     {"decision_policy_template", "Order bands with approval above a cap."}};
    const WorkspaceSolution copy = lake::control::solution_from_catalogue(entry, "");
    EXPECT_EQ(copy.name, "Demand Forecasting");
    EXPECT_EQ(copy.problem, "Stock-outs and overstock cost margin.");
    EXPECT_EQ(copy.decision, "Forecast demand and set the order quantity.");
    EXPECT_EQ(copy.measurement.metric, "Forecast value added against a naive baseline.")
        << "with no measured outcome, the measurement design";
    EXPECT_TRUE(copy.measurement.baseline.empty()) << "the person's own figures";
    ASSERT_EQ(copy.methodologies.size(), 2U) << "a methodology is listed once";
    EXPECT_EQ(copy.methodologies[0], (lake::control::SolutionMethodology{"M-7", 1}));
    EXPECT_EQ(copy.methodologies[1], (lake::control::SolutionMethodology{"M-9", 3}));
    EXPECT_EQ(copy.pipeline, "Methodologies in order: M-7, M-9");
    EXPECT_EQ(copy.duplicated_from, "S-4");
    EXPECT_NE(copy.notes.find("Duplicated from Orama's S-4 Demand Forecasting."),
              std::string::npos);
    EXPECT_NE(copy.notes.find("Decision policy: Order bands"), std::string::npos);

    auto stored = copy;
    stored.customer_id = "customer-1";
    EXPECT_FALSE(lake::control::solution_problem(stored)) << "a copy is a valid solution";

    const WorkspaceSolution named = lake::control::solution_from_catalogue(entry, "Our demand");
    EXPECT_EQ(named.name, "Our demand");
    const WorkspaceSolution bare =
        lake::control::solution_from_catalogue({{"id", "S-9"}, {"promise", "Route the work."}}, "");
    EXPECT_EQ(bare.name, "S-9");
    EXPECT_EQ(bare.problem, "Route the work.");
    EXPECT_EQ(bare.measurement.metric, "the return of S-9");
    EXPECT_TRUE(bare.methodologies.empty());
}

TEST(Solutions, ACopyIsNamedWithinTheBound) {
    EXPECT_EQ(lake::control::duplicate_name("Churn"), "Churn (copy)");
    const std::string long_name(lake::control::max_solution_name_bytes, 'a');
    const std::string copy = lake::control::duplicate_name(long_name);
    EXPECT_EQ(copy.size(), lake::control::max_solution_name_bytes);
    EXPECT_TRUE(copy.ends_with(" (copy)"));
    // Never cut inside a UTF-8 sequence.
    const std::string accented =
        std::string(lake::control::max_solution_name_bytes - 8, 'a') + "\xC3\xA9\xC3\xA9";
    const std::string cut = lake::control::duplicate_name(accented);
    EXPECT_EQ(cut, std::string(lake::control::max_solution_name_bytes - 8, 'a') + " (copy)");
}

TEST(Solutions, AnEditKeepsWhatItDoesNotSend) {
    auto solution = valid();
    lake::control::SolutionChange change;
    EXPECT_TRUE(lake::control::empty_change(change));
    change.notes = "";
    change.updated_by = "user-2";
    EXPECT_FALSE(lake::control::empty_change(change));
    solution.notes = "old";
    lake::control::apply_change(solution, change, 1'800'000'000'000'000);
    EXPECT_EQ(solution.notes, "");
    EXPECT_EQ(solution.name, "Late payments");
    EXPECT_EQ(solution.updated_by, "user-2");
    EXPECT_EQ(solution.updated_at, "2027-01-15T08:00:00Z");
}

TEST(Solutions, SolutionAuditGoesToTheWorkspace) {
    for (const std::string name : {"list_solutions",
                                   "get_solution",
                                   "create_solution",
                                   "update_solution",
                                   "delete_solution",
                                   "duplicate_solution"}) {
        EXPECT_EQ(lake::api::audit_log_of(name), lake::api::AuditLog::Workspace) << name;
    }
}

} // namespace
