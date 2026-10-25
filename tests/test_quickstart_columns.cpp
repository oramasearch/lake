// The quickstart's column rules (lake#1181, docs/api/quickstart.md#columns): each column's kind
// from the engine's profile, the columns offered first, the training copy, and the engine's
// refusals in plain words. Pure: no engine.

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/quickstart_columns.hpp"

namespace {

using json = nlohmann::json;
namespace qc = lake::api::quickstart_columns;

using Top = std::vector<std::pair<json, int>>;

json profiled(const std::string& name,
              const std::string& semantic,
              int distinct,
              const Top& top = {},
              double null_rate = 0.0) {
    json values = json::array();
    for (const auto& [value, count] : top) {
        values.push_back({{"value", value}, {"count", count}});
    }
    return {{"name", name},
            {"semantic_type", semantic},
            {"distinct_count", distinct},
            {"null_rate", null_rate},
            {"top_values", values}};
}

// Steve's dataset (lake#1181): a first name, a country, a date, and the rest of what files hold.
json people_analysis() {
    return {{"columns",
             json::array({profiled("customer_id", "identifier", 60, {{"c-1", 1}, {"c-2", 1}}),
                          profiled("First Name", "categorical", 10, {{"Ana", 6}, {"Ben", 6}}),
                          profiled("Country",
                                   "categorical",
                                   4,
                                   {{"Canada", 20}, {"France", 15}, {"Japan", 15}, {"Brazil", 10}}),
                          profiled("Signup Date", "datetime", 60, {{"2025-01-01", 1}}),
                          profiled("Age", "numeric", 40, {{20, 1}, {21, 1}}),
                          profiled("Plan", "categorical", 2, {{"free", 40}, {"pro", 20}}),
                          profiled("Bio", "free_text", 60, {{"Likes hiking", 1}}),
                          profiled("Notes", "unknown", 0, Top{}, 1.0),
                          profiled("Region", "categorical", 1, {{"EU", 1}})})}};
}

json people_columns() {
    return qc::detect_columns(people_analysis(),
                              json::array({{{"name", "customer_id"}, {"type", "string"}},
                                           {{"name", "First Name"}, {"type", "string"}},
                                           {{"name", "Country"}, {"type", "string"}},
                                           {{"name", "Signup Date"}, {"type", "string"}},
                                           {{"name", "Age"}, {"type", "integer"}},
                                           {{"name", "Plan"}, {"type", "string"}},
                                           {{"name", "Bio"}, {"type", "string"}},
                                           {{"name", "Notes"}, {"type", "string"}},
                                           {{"name", "Region"}, {"type", "string"}}}));
}

std::vector<json> people_rows() {
    std::vector<json> rows;
    const std::vector<std::string> countries{"Canada", "France", "Japan", "Canada"};
    for (int i = 0; i < 8; ++i) {
        rows.push_back({{"customer_id", "c-" + std::to_string(i)},
                        {"First Name", i % 2 == 0 ? "Ana" : "Ben"},
                        {"Country", countries[static_cast<std::size_t>(i % 4)]},
                        {"Signup Date", "2025-0" + std::to_string(1 + i) + "-15"},
                        {"Age", 20 + i},
                        {"Plan", i % 3 == 0 ? "pro" : "free"},
                        {"Bio", "Likes hiking and long walks number " + std::to_string(i)},
                        {"Notes", nullptr},
                        {"Region", "EU"}});
    }
    return rows;
}

const json& column(const json& columns, const std::string& name) {
    const json* found = qc::find(columns, name);
    EXPECT_NE(found, nullptr) << name;
    static const json none;
    return found != nullptr ? *found : none;
}

TEST(QuickstartColumns, EveryKindIsDetectedFromTheEnginesProfileInTheFilesOrder) {
    const json columns = people_columns();
    ASSERT_EQ(columns.size(), 9U);
    EXPECT_EQ(columns[0]["name"], "customer_id") << "the file's order";
    EXPECT_EQ(column(columns, "customer_id")["kind"], "identifier");
    EXPECT_EQ(column(columns, "First Name")["kind"], "categorical");
    EXPECT_EQ(column(columns, "First Name")["title"], "category, 10 values");
    EXPECT_EQ(column(columns, "Signup Date")["kind"], "datetime");
    EXPECT_EQ(column(columns, "Age")["kind"], "numeric");
    EXPECT_EQ(column(columns, "Age")["type"], "integer") << "the storage type is kept";
    EXPECT_EQ(column(columns, "Plan")["kind"], "boolean") << "two values are a yes or no";
    EXPECT_EQ(column(columns, "Bio")["kind"], "free_text");
    EXPECT_EQ(column(columns, "Notes")["kind"], "empty");
    EXPECT_EQ(column(columns, "Region")["kind"], "constant");
    EXPECT_TRUE(qc::detected(columns));

    // What the first model reads, and why not.
    for (const char* used : {"First Name", "Country", "Age", "Plan"}) {
        EXPECT_TRUE(column(columns, used)["feature"]["use"].get<bool>()) << used;
    }
    for (const char* left : {"customer_id", "Signup Date", "Bio", "Notes", "Region"}) {
        EXPECT_FALSE(column(columns, left)["feature"]["use"].get<bool>()) << left;
        EXPECT_FALSE(column(columns, left)["feature"]["reason"].get<std::string>().empty()) << left;
    }
    // What can be predicted: two values, or a category of a few with a value to look for.
    EXPECT_TRUE(column(columns, "Plan")["predict"]["can"].get<bool>());
    EXPECT_EQ(column(columns, "Plan")["predict"]["positive"], "pro") << "the rarer of two";
    EXPECT_TRUE(column(columns, "Country")["predict"]["can"].get<bool>());
    EXPECT_TRUE(column(columns, "Country")["predict"]["choose_value"].get<bool>());
    EXPECT_EQ(column(columns, "Country")["predict"]["values"][0], "Canada");
    for (const char* cannot : {"customer_id", "Signup Date", "Age", "Bio", "Notes", "Region"}) {
        EXPECT_FALSE(column(columns, cannot)["predict"]["can"].get<bool>()) << cannot;
        EXPECT_FALSE(column(columns, cannot)["predict"]["reason"].get<std::string>().empty())
            << cannot;
    }
    // A column the profile does not name keeps no kind, and the set is not detected.
    const json partial = qc::detect_columns(json::object(), json::array({{{"name", "a"}}}));
    EXPECT_TRUE(partial[0]["kind"].is_null());
    EXPECT_FALSE(qc::detected(partial));
}

TEST(QuickstartColumns, TheFirstOfferIsASensibleTargetAndAnIdentifier) {
    // No outcome-like name: the last two-valued column, and the identifier names a row.
    json suggested = qc::suggest(people_columns());
    EXPECT_EQ(suggested["target"], "Plan");
    EXPECT_TRUE(suggested["target_value"].is_null());
    EXPECT_EQ(suggested["entity_column"], "customer_id");

    // A column named like an outcome wins over the file's order.
    json analysis{{"columns",
                   json::array({profiled("churned", "boolean", 2, {{0, 1}, {1, 1}}),
                                profiled("vip", "boolean", 2, {{0, 1}, {1, 1}}),
                                profiled("tenure", "numeric", 30, {{1, 1}})})}};
    suggested = qc::suggest(qc::detect_columns(analysis, json::array()));
    EXPECT_EQ(suggested["target"], "churned");
    EXPECT_TRUE(suggested["entity_column"].is_null()) << "no identifier: Lake numbers the rows";

    // No two-valued column: the category with the fewest values, looking for its most common.
    analysis = {{"columns",
                 json::array({profiled("name", "categorical", 10, {{"Ana", 1}}),
                              profiled("country", "categorical", 4, {{"Japan", 9}, {"Peru", 2}}),
                              profiled("signed_up", "datetime", 40, {{"2025-01-01", 1}})})}};
    suggested = qc::suggest(qc::detect_columns(analysis, json::array()));
    EXPECT_EQ(suggested["target"], "country");
    EXPECT_EQ(suggested["target_value"], "Japan");

    // Nothing to predict.
    analysis = {{"columns", json::array({profiled("amount", "numeric", 50, {{1, 1}})})}};
    suggested = qc::suggest(qc::detect_columns(analysis, json::array()));
    EXPECT_TRUE(suggested["target"].is_null());
}

TEST(QuickstartColumns, AChoiceIsCheckedInPlainWords) {
    const json columns = people_columns();
    EXPECT_TRUE(qc::check_choice(columns, {"Country", "Japan", "First Name"}).message.empty());
    EXPECT_TRUE(qc::check_choice(columns, {"Plan", "", ""}).message.empty());
    auto problem = qc::check_choice(columns, {"Age", "", ""});
    EXPECT_EQ(problem.path, "/target");
    EXPECT_NE(problem.message.find("yes-or-no"), std::string::npos) << problem.message;
    problem = qc::check_choice(columns, {"Plan", "gold", ""});
    EXPECT_EQ(problem.path, "/target_value");
    problem = qc::check_choice(columns, {"Country", "", "Country"});
    EXPECT_EQ(problem.path, "/entity_column");
    problem = qc::check_choice(columns, {"nope", "", ""});
    EXPECT_EQ(problem.path, "/target");
    problem = qc::check_choice(columns, {"", "", ""});
    EXPECT_NE(problem.message.find("Country"), std::string::npos) << "names what can be chosen";
    const json nothing = qc::detect_columns(
        {{"columns", json::array({profiled("amount", "numeric", 50, {{1, 1}})})}}, json::array());
    problem = qc::check_choice(nothing, {"", "", ""});
    EXPECT_NE(problem.message.find("None of this dataset's columns"), std::string::npos);
}

TEST(QuickstartColumns, YesLikeValuesAreWhatAModelLooksFor) {
    json yes_no{{"values", json::array({"no", "yes"})}, {"counts", json::array({10, 90})}};
    EXPECT_EQ(qc::positive_value(yes_no), "yes");
    json ints{{"values", json::array({"0", "1"})}, {"counts", json::array({90, 10})}};
    EXPECT_EQ(qc::positive_value(ints), "1");
    json tie{{"values", json::array({"blue", "red"})}, {"counts", json::array({5, 5})}};
    EXPECT_EQ(qc::positive_value(tie), "red");
    EXPECT_EQ(qc::value_text(json(1.0)), "1");
    EXPECT_EQ(qc::value_text(json(2.5)), "2.5");
    EXPECT_EQ(qc::value_text(json(true)), "true");
    EXPECT_EQ(qc::value_text(json("x")), "x");
}

TEST(QuickstartColumns, TheTrainingCopyKeepsWhatTheModelCanUseAndWritesTheAnswerAsZeroOrOne) {
    const json columns = people_columns();
    const qc::Choice choice{"Country", "Canada", "First Name"};
    EXPECT_FALSE(qc::trains_as_is(columns, choice, {}));
    const auto made = qc::prepare_rows(columns, people_rows(), choice, {});
    ASSERT_TRUE(made.prepared) << made.problem;
    const qc::Prepared& prepared = *made.prepared;
    EXPECT_TRUE(prepared.copy);
    // First Name repeats: the engine would merge its rows, so they are numbered and First Name
    // is learned from like any other category.
    EXPECT_EQ(prepared.entity, "row_number");
    EXPECT_NE(prepared.note.find("First Name repeats"), std::string::npos) << prepared.note;
    EXPECT_EQ(prepared.used, json::array({"First Name", "Age", "Plan"}));
    std::vector<std::string> dropped;
    for (const json& entry : prepared.dropped) {
        dropped.push_back(entry["name"]);
        EXPECT_FALSE(entry["reason"].get<std::string>().empty());
    }
    EXPECT_EQ(dropped,
              (std::vector<std::string>{"customer_id", "Signup Date", "Bio", "Notes", "Region"}));
    EXPECT_EQ(prepared.rows, 8U);
    EXPECT_EQ(prepared.label["positive"], "Canada");
    EXPECT_EQ(prepared.label["rows_yes"], 4);
    EXPECT_EQ(prepared.header,
              (std::vector<std::string>{"row_number", "First Name", "Age", "Plan", "Country"}));
    const json first = json::parse(prepared.content.substr(0, prepared.content.find('\n')));
    EXPECT_EQ(first,
              json({{"row_number", 1},
                    {"First Name", "Ana"},
                    {"Age", 20},
                    {"Plan", "pro"},
                    {"Country", 1}}));
    EXPECT_EQ(prepared.content.find("Signup Date"), std::string::npos) << "no date reaches it";

    // A name that is unique names the rows as it is.
    const auto unique =
        qc::prepare_rows(columns, people_rows(), {"Country", "Canada", "customer_id"}, {});
    ASSERT_TRUE(unique.prepared) << unique.problem;
    EXPECT_EQ(unique.prepared->entity, "customer_id");
    EXPECT_TRUE(unique.prepared->note.empty());

    // The same as CSV, for a copy too large as JSON Lines.
    const std::string csv = qc::csv_content(prepared);
    EXPECT_EQ(csv.substr(0, csv.find('\n')), "row_number,First Name,Age,Plan,Country");
    EXPECT_NE(csv.find("\n1,Ana,20,pro,1\n"), std::string::npos) << csv;

    // No column names a row: the rows are numbered under a free name.
    const auto numbered = qc::prepare_rows(columns, people_rows(), {"Plan", "", ""}, {});
    ASSERT_TRUE(numbered.prepared) << numbered.problem;
    EXPECT_EQ(numbered.prepared->entity, "row_number");
    EXPECT_NE(numbered.prepared->content.find("\"row_number\":1"), std::string::npos);
    EXPECT_NE(numbered.prepared->content.find("\"Plan\":1"), std::string::npos) << "pro is 1";
    EXPECT_EQ(qc::row_number_column(json::array({{{"name", "row_number"}}})), "row_number_2");
}

TEST(QuickstartColumns, AChoiceThatCannotTeachAnythingIsRefusedBeforeTraining) {
    const json columns = people_columns();
    auto made = qc::prepare_rows(columns, people_rows(), {"Country", "Peru", ""}, {});
    EXPECT_FALSE(made.prepared);
    EXPECT_NE(made.problem.find("No row has Country = Peru"), std::string::npos) << made.problem;
    std::vector<json> same = people_rows();
    for (json& row : same) {
        row["Country"] = "Canada";
    }
    made = qc::prepare_rows(columns, same, {"Country", "Canada", ""}, {});
    EXPECT_NE(made.problem.find("nothing to tell apart"), std::string::npos) << made.problem;
    std::vector<json> blank = people_rows();
    for (json& row : blank) {
        row["Country"] = nullptr;
    }
    made = qc::prepare_rows(columns, blank, {"Country", "Canada", ""}, {});
    EXPECT_NE(made.problem.find("No row has a value"), std::string::npos) << made.problem;
}

TEST(QuickstartColumns, ADatasetThatIsAlreadyReadyTrainsAsItIs) {
    const json analysis{{"columns",
                         json::array({profiled("title", "identifier", 4, {{"Alien", 1}}),
                                      profiled("genre", "categorical", 3, {{"scifi", 1}}),
                                      profiled("rating", "numeric", 4, {{8.7, 1}}),
                                      profiled("popular", "boolean", 2, {{1, 1}, {0, 1}})})}};
    const json columns =
        qc::detect_columns(analysis,
                           json::array({{{"name", "title"}, {"type", "string"}},
                                        {{"name", "genre"}, {"type", "string"}},
                                        {{"name", "rating"}, {"type", "number"}},
                                        {{"name", "popular"}, {"type", "integer"}}}));
    EXPECT_TRUE(qc::trains_as_is(columns, {"popular", "", "title"}, {}));
    EXPECT_FALSE(qc::trains_as_is(columns, {"popular", "", ""}, {})) << "no row name";
    EXPECT_FALSE(qc::trains_as_is(columns, {"popular", "0", "title"}, {})) << "looks for 0";
    qc::Fixes fixes;
    fixes.dates_from_values = true;
    EXPECT_FALSE(qc::trains_as_is(columns, {"popular", "", "title"}, fixes));
}

TEST(QuickstartColumns, DatesAreFoundInTheValuesToo) {
    const std::vector<json> rows{
        {{"when", "2025-01-01"}, {"at", "2025-01-01T09:30:00Z"}, {"n", 1}, {"s", "x"}},
        {{"when", "2025-02-28"}, {"at", "2025-01-01 09:30:00"}, {"n", 2}, {"s", "2025-01-01"}},
        {{"when", nullptr}, {"at", "2025-01-01T09:30:00.25+02:00"}, {"n", 3}, {"s", "y"}}};
    EXPECT_EQ(qc::date_columns_in(rows), (std::set<std::string>{"at", "when"}));
    EXPECT_EQ(qc::snapshot_date("2026-10-24"), "2026-10-24");
    EXPECT_EQ(qc::snapshot_date("2026-10-24T09:30:00Z"), "2026-10-24T09:30:00Z");
    EXPECT_EQ(qc::snapshot_date("2026-02-30"), "");
    EXPECT_EQ(qc::snapshot_date("10/24/2026"), "");
    EXPECT_EQ(qc::snapshot_date("2026-10-24T25:00"), "");
}

TEST(QuickstartColumns, RefusalsAreFixedByTheCopyOrExplainedNeverShownRaw) {
    const json columns = people_columns();
    const qc::Choice choice{"Country", "Canada", "First Name"};
    const auto snapshot = qc::refusal(
        "transform_required: add_snapshot_date: Datetime features must be computed relative to a "
        "declared as_of or snapshot date; using the current time would change training data "
        "silently.");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->name, "add_snapshot_date");
    EXPECT_NE(snapshot->description.find("as_of"), std::string::npos);
    const auto fix = qc::fix_for(*snapshot, columns, choice);
    ASSERT_TRUE(fix);
    EXPECT_TRUE(fix->dates_from_values);
    EXPECT_TRUE(fix->drop.contains("Signup Date"));
    const auto key = qc::fix_for({"add_entity_key", ""}, columns, choice);
    ASSERT_TRUE(key);
    EXPECT_TRUE(key->number_rows);
    EXPECT_FALSE(qc::fix_for({"create_eligible_features", ""}, columns, choice));
    EXPECT_FALSE(qc::refusal("engine_error: something else"));

    for (const char* name : {"create_eligible_features",
                             "add_snapshot_date",
                             "capture_label",
                             "add_entity_key",
                             "aggregate_events",
                             "pivot_measurements",
                             "join_label_table",
                             "something_new"}) {
        const std::string said = qc::explain_refusal({name, "Reshape it."}, columns, choice);
        EXPECT_EQ(said.find("transform_required"), std::string::npos) << name;
        EXPECT_EQ(said.find(name), std::string::npos) << name << ": " << said;
        EXPECT_FALSE(said.empty());
    }
    const std::string eligible =
        qc::explain_refusal({"create_eligible_features", ""}, columns, choice);
    EXPECT_NE(eligible.find("Signup Date (a date or time"), std::string::npos) << eligible;

    EXPECT_NE(qc::explain_run_failure("model step 'x' failed: train job failed: gbdt train: "
                                      "train_gbdt: training labels are a single class",
                                      choice)
                  .find("same answer for Country"),
              std::string::npos);
    EXPECT_EQ(qc::explain_run_failure("a: b: the disk is full", choice),
              "Training stopped: the disk is full. Change the choices and train again.");
}

} // namespace
