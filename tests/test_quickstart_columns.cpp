// SPDX-License-Identifier: MIT

// The quickstart's column rules (lake#1181, docs/api/quickstart.md#columns): each column's kind
// from the engine's profile, the columns offered first, the training copy, and the engine's
// refusals in plain words. Pure: no engine.

#include <algorithm>
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

// The reported dataset (lake#1181): a first name, a country, a date, and the rest of what files hold.
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
    // Anything else is said without the engine's own words (lake#1204): they stay in `detail`.
    EXPECT_EQ(qc::explain_run_failure("a: b: the disk is full", choice),
              "Lake could not train the model on these choices. Try again, or let Ask Orama fix "
              "it.");
}

// lake#1182: a dataset with no yes-or-no column trains on a target derived from a number column
// (a threshold or a band) or from the values that count as yes, and the source column is left out.
TEST(QuickstartColumns, ATargetDerivedFromAColumnTrainsWhenNoColumnIsAYesOrNoAnswer) {
    const json columns = people_columns();
    const std::vector<json> rows = people_rows();

    auto parsed = qc::parse_derived({{"column", "Age"}, {"above", 23}}, columns);
    ASSERT_TRUE(parsed.derived) << parsed.problem.message;
    EXPECT_EQ(parsed.derived->name, "age_above_23");
    EXPECT_EQ(qc::derived_rule(*parsed.derived), "Age above 23");

    const qc::Derive made = qc::derive_target(columns, rows, *parsed.derived);
    ASSERT_TRUE(made.problem.empty()) << made.problem;
    const json* derived = qc::find(made.columns, "age_above_23");
    ASSERT_NE(derived, nullptr);
    EXPECT_EQ((*derived)["kind"], "boolean");
    EXPECT_EQ((*derived)["predict"]["can"], true);
    EXPECT_EQ((*derived)["derived"]["from"], "Age");
    EXPECT_EQ((*qc::find(made.columns, "Age"))["feature"]["use"], false)
        << "the source gives the answer away";
    EXPECT_EQ(made.rows[0]["age_above_23"], "no");  // 20
    EXPECT_EQ(made.rows[7]["age_above_23"], "yes"); // 27

    // It trains through the same training copy, with the answer as 0/1 and Age left out.
    const qc::Choice choice{"age_above_23", "yes", "customer_id"};
    EXPECT_TRUE(qc::check_choice(made.columns, choice).message.empty());
    EXPECT_FALSE(qc::trains_as_is(made.columns, choice, {}));
    const qc::Prepare prepared = qc::prepare_rows(made.columns, made.rows, choice, {});
    ASSERT_TRUE(prepared.prepared) << prepared.problem;
    EXPECT_EQ(prepared.prepared->label["rows_yes"], 4);
    EXPECT_EQ(prepared.prepared->label["rows_no"], 4);
    EXPECT_EQ(std::ranges::find(prepared.prepared->used, json("Age")),
              prepared.prepared->used.end());

    // A band, and values of a category.
    auto band = qc::parse_derived({{"column", "Age"}, {"at_least", 22}, {"below", 25}}, columns);
    ASSERT_TRUE(band.derived) << band.problem.message;
    EXPECT_EQ(qc::derived_rule(*band.derived), "Age at least 22 and below 25");
    EXPECT_EQ(band.derived->name, "age_at_least_22_and_below_25");
    const qc::Derive banded = qc::derive_target(columns, rows, *band.derived);
    EXPECT_TRUE(banded.problem.empty()) << banded.problem;
    EXPECT_EQ(banded.rows[2]["age_at_least_22_and_below_25"], "yes");
    EXPECT_EQ(banded.rows[5]["age_at_least_22_and_below_25"], "no");
    auto valued = qc::parse_derived(
        {{"column", "Country"}, {"values", {"Japan"}}, {"name", "in_japan"}}, columns);
    ASSERT_TRUE(valued.derived) << valued.problem.message;
    EXPECT_EQ(qc::derived_rule(*valued.derived), "Country is Japan");
    EXPECT_EQ(qc::derive_target(columns, rows, *valued.derived).rows[2]["in_japan"], "yes");
}

TEST(QuickstartColumns, ADerivedTargetThatCannotTeachAnythingIsSaidInPlainWords) {
    const json columns = people_columns();
    const std::vector<json> rows = people_rows();
    const auto problem = [&](const json& input) {
        return qc::parse_derived(input, columns).problem.message;
    };
    EXPECT_NE(problem({{"column", "Nope"}, {"above", 1}}).find("no column named Nope"),
              std::string::npos);
    EXPECT_NE(problem({{"column", "Age"}}).find("needs a rule"), std::string::npos);
    EXPECT_NE(problem({{"column", "Country"}, {"above", 1}}).find("is not a number"),
              std::string::npos);
    EXPECT_NE(problem({{"column", "Age"}, {"above", 1}, {"values", {"x"}}}).find("not both"),
              std::string::npos);
    EXPECT_NE(problem({{"column", "Age"}, {"at_least", 30}, {"below", 25}}).find("lower bound"),
              std::string::npos);
    EXPECT_NE(problem({{"column", "Age"}, {"above", 1}, {"name", "Plan"}}).find("already has"),
              std::string::npos);
    auto all = qc::parse_derived({{"column", "Age"}, {"above", 1}}, columns);
    ASSERT_TRUE(all.derived);
    EXPECT_NE(qc::derive_target(columns, rows, *all.derived).problem.find("Every row has"),
              std::string::npos);
    auto none = qc::parse_derived({{"column", "Age"}, {"above", 99}}, columns);
    ASSERT_TRUE(none.derived);
    EXPECT_NE(qc::derive_target(columns, rows, *none.derived).problem.find("No row has"),
              std::string::npos);
}

// lake#1182: segmentation / clustering (M-3) has no column to predict; its training copy keeps the
// usable columns and numbers the rows when no column names each one.
TEST(QuickstartColumns, ClusteringNeedsNoTargetAndNumbersTheRowsWhenNothingNamesThem) {
    EXPECT_TRUE(qc::label_free("M-3"));
    EXPECT_TRUE(qc::label_free("clustering"));
    EXPECT_FALSE(qc::label_free("M-1"));
    const json columns = people_columns();
    const std::vector<json> rows = people_rows();

    const qc::Prepare named = qc::prepare_unlabeled(columns, rows, "customer_id");
    ASSERT_TRUE(named.prepared) << named.problem;
    EXPECT_EQ(named.prepared->entity, "customer_id") << "unique: it names each row";
    EXPECT_TRUE(named.prepared->copy) << "free text and a date are left out or derived, so a copy";
    EXPECT_EQ(named.prepared->rows, 8U);

    const qc::Prepare numbered = qc::prepare_unlabeled(columns, rows, "");
    ASSERT_TRUE(numbered.prepared) << numbered.problem;
    EXPECT_EQ(numbered.prepared->entity, "row_number");
    EXPECT_NE(numbered.prepared->content.find("\"row_number\":8"), std::string::npos);
    EXPECT_NE(std::ranges::find(numbered.prepared->used, json("Age")),
              numbered.prepared->used.end());
    EXPECT_EQ(std::ranges::find(numbered.prepared->used, json("Bio")),
              numbered.prepared->used.end())
        << "free text is left out";

    const qc::Prepare repeated = qc::prepare_unlabeled(columns, rows, "First Name");
    ASSERT_TRUE(repeated.prepared);
    EXPECT_EQ(repeated.prepared->entity, "row_number");
    EXPECT_FALSE(repeated.prepared->note.empty());
}


// lake#1242: the Quickstart's clustering of customers-10000 (Index, Customer Id, First/Last Name,
// Company, City, Country, Phone 1/2, Email, Subscription Date, Website) trained on the row number
// `Index` alone, and its two segments were "low Index" and "high Index". The engine's profile
// reads `Index` as a number (zetta#1216: a number is a key only when its name ends in _id or
// _key), so it was the one usable input, and with `Customer Id` unique the dataset trained as it
// was, which hands the engine every column.
json customers_analysis() {
    return {{"columns",
             json::array({profiled("Index", "numeric", 10000, {{1, 1}, {2, 1}}),
                          profiled("Customer Id", "identifier", 10000, {{"DD37Cf93aecA6Dc", 1}}),
                          profiled("First Name", "categorical", 690, {{"Sheryl", 30}}),
                          profiled("Last Name", "categorical", 1000, {{"Baxter", 25}}),
                          profiled("Company", "identifier", 9900, {{"Rasmussen Group", 2}}),
                          profiled("City", "categorical", 8000, {{"East Leonard", 3}}),
                          profiled("Country", "categorical", 243, {{"Chile", 60}}),
                          profiled("Phone 1", "identifier", 10000, {{"229.077.5154", 1}}),
                          profiled("Phone 2", "identifier", 10000, {{"397.884.0519x718", 1}}),
                          profiled("Email", "identifier", 10000, {{"zunigavanessa@smith.info", 1}}),
                          profiled("Subscription Date", "datetime", 870, {{"2020-08-24", 20}}),
                          profiled("Website", "identifier", 9900, {{"http://www.stephenson.com/", 2}})})}};
}

json customers_columns() {
    json stored = json::array();
    for (const char* name : {"Index", "Customer Id", "First Name", "Last Name", "Company", "City",
                             "Country", "Phone 1", "Phone 2", "Email", "Subscription Date",
                             "Website"}) {
        stored.push_back({{"name", name}, {"type", std::string(name) == "Index" ? "integer" : "string"}});
    }
    return qc::detect_columns(customers_analysis(), stored);
}

std::vector<json> customers_rows(int count) {
    std::vector<json> rows;
    for (int i = 0; i < count; ++i) {
        const std::string n = std::to_string(i);
        const int month = 1 + (i % 12);
        rows.push_back({{"Index", i + 1},
                        {"Customer Id", "CID" + n},
                        {"First Name", "First" + n},
                        {"Last Name", "Last" + n},
                        {"Company", "Company " + n},
                        {"City", "City " + n},
                        {"Country", "Country " + std::to_string(i % 40)},
                        {"Phone 1", "229.077." + std::to_string(1000 + i)},
                        {"Phone 2", "+1-397-884-" + std::to_string(1000 + i) + "x7"},
                        {"Email", "person" + n + "@example.com"},
                        {"Subscription Date",
                         std::string(i % 2 == 0 ? "2021-" : "2022-") + (month < 10 ? "0" : "") +
                             std::to_string(month) + "-1" + std::to_string(i % 10)},
                        {"Website", "https://www.company" + n + ".com/"}});
    }
    return rows;
}

bool holds(const json& list, const std::string& name) {
    return std::ranges::find(list, json(name)) != list.end();
}

TEST(QuickstartColumns, ClusteringNeverGroupsByTheRowNumberOrIdentifiersOrContactDetails) {
    const json columns = customers_columns();
    const json* index = qc::find(columns, "Index");
    ASSERT_NE(index, nullptr);
    EXPECT_EQ((*index)["kind"], "identifier") << "its name says it numbers the rows";
    EXPECT_EQ((*index)["feature"]["use"], false);
    EXPECT_EQ(qc::suggest(columns)["entity_column"], "Index");

    const std::vector<json> rows = customers_rows(120);
    for (const std::string entity : {"Customer Id", "Index", ""}) {
        const qc::Prepare made = qc::prepare_unlabeled(columns, rows, entity);
        ASSERT_TRUE(made.prepared) << made.problem;
        const qc::Prepared& prepared = *made.prepared;
        EXPECT_TRUE(prepared.copy) << entity << ": columns were left out, so the engine gets a copy";
        EXPECT_FALSE(holds(prepared.used, "Index")) << entity << ": " << prepared.used.dump();
        for (const char* left : {"Customer Id", "First Name", "Last Name", "Company", "City",
                                 "Phone 1", "Phone 2", "Email", "Website", "Subscription Date"}) {
            EXPECT_FALSE(holds(prepared.used, left)) << left;
        }
        EXPECT_TRUE(holds(prepared.used, "Subscription Date month")) << prepared.used.dump();
        EXPECT_TRUE(holds(prepared.used, "Subscription Date days before latest"));
        EXPECT_EQ(prepared.derived.size(), 2U);
        // The copy holds the row's name and the derived parts, and never the row number.
        const auto first = json::parse(prepared.content.substr(0, prepared.content.find('\n')));
        EXPECT_FALSE(first.contains("Index") && prepared.entity != "Index") << first.dump();
        EXPECT_EQ(first["Subscription Date month"], 1);
        EXPECT_TRUE(first["Subscription Date days before latest"].is_number_integer());
        EXPECT_GT(first["Subscription Date days before latest"].get<long long>(), 300);
        bool said = false;
        for (const json& dropped : prepared.dropped) {
            if (dropped["name"] == "Subscription Date") {
                said = dropped["reason"].get<std::string>().find("Subscription Date month") !=
                       std::string::npos;
            }
        }
        EXPECT_TRUE(said) << prepared.dropped.dump();
    }
}

// A row number the name does not give away (`n`) is told by its values in a table large enough to
// judge, and a small table's unique, rising measure (ages 20 to 27) is kept (zetta#1216).
TEST(QuickstartColumns, ClusteringLeavesOutAColumnWhoseValuesNumberTheRows) {
    const json columns = qc::detect_columns(
        {{"columns",
          json::array({profiled("n", "numeric", 60, {{1, 1}}),
                       profiled("Spend", "numeric", 60, {{10.5, 1}}),
                       profiled("Plan", "categorical", 3, {{"free", 20}, {"pro", 20}})})}},
        json::array());
    std::vector<json> rows;
    const std::vector<std::string> plans{"free", "pro", "team"};
    for (int i = 0; i < 60; ++i) {
        rows.push_back({{"n", 60 - i}, {"Spend", 3.5 * ((i * 7) % 13)}, {"Plan", plans[i % 3]}});
    }
    const qc::Prepare made = qc::prepare_unlabeled(columns, rows, "");
    ASSERT_TRUE(made.prepared) << made.problem;
    EXPECT_FALSE(holds(made.prepared->used, "n")) << made.prepared->used.dump();
    EXPECT_TRUE(holds(made.prepared->used, "Spend"));
    EXPECT_TRUE(holds(made.prepared->used, "Plan"));

    // Shuffled, it still fills 1..60 with no gaps.
    for (int i = 0; i < 60; ++i) {
        rows[static_cast<std::size_t>(i)]["n"] = 1 + (i * 7) % 60;
    }
    EXPECT_FALSE(holds(qc::prepare_unlabeled(columns, rows, "").prepared->used, "n"));

    const qc::Prepare small = qc::prepare_unlabeled(people_columns(), people_rows(), "");
    ASSERT_TRUE(small.prepared);
    EXPECT_TRUE(holds(small.prepared->used, "Age")) << "8 rows are too few to call it a row number";
}

TEST(QuickstartColumns, ClusteringLeavesOutNamesAndContactDetailsInASmallTable) {
    const json columns = qc::detect_columns(
        {{"columns",
          json::array({profiled("First Name", "categorical", 3, {{"Ana", 2}}),
                       profiled("Contact", "categorical", 4, {{"ana@example.com", 1}}),
                       profiled("Spend", "numeric", 4, {{1, 1}})})}},
        json::array());
    const std::vector<json> rows{
        {{"First Name", "Ana"}, {"Contact", "ana@example.com"}, {"Spend", 1}},
        {{"First Name", "Ben"}, {"Contact", "+1 (415) 555-0101"}, {"Spend", 5}},
        {{"First Name", "Ana"}, {"Contact", "https://ana.example.com"}, {"Spend", 9}},
        {{"First Name", "Chen"}, {"Contact", "chen@example.org"}, {"Spend", 2}}};
    const qc::Prepare made = qc::prepare_unlabeled(columns, rows, "");
    ASSERT_TRUE(made.prepared) << made.problem;
    EXPECT_EQ(made.prepared->used, json::array({"Spend"}));
}

// Nothing to group by: said in words, with why each column was left out, instead of training.
TEST(QuickstartColumns, ClusteringSaysSoWhenNoColumnQualifies) {
    json analysis = customers_analysis();
    analysis["columns"].erase(analysis["columns"].begin() + 10); // Subscription Date
    std::vector<json> rows = customers_rows(120);
    for (json& row : rows) {
        row.erase("Subscription Date");
    }
    const json columns = qc::detect_columns(analysis, json::array());
    const qc::Prepare made = qc::prepare_unlabeled(columns, rows, "Customer Id");
    EXPECT_FALSE(made.prepared);
    EXPECT_NE(made.problem.find("found nothing in this dataset to group rows by"), std::string::npos)
        << made.problem;
    EXPECT_NE(made.problem.find("Index is an identifier"), std::string::npos) << made.problem;
}

// A Lake-only identifier (the engine reads `Index` as a number) keeps the dataset from training as
// it is for the default model too: the engine would learn from it.
TEST(QuickstartColumns, ARowNumberTheEngineReadsAsANumberIsNotLeftOutByTheEngine) {
    const json columns = qc::detect_columns(
        {{"columns",
          json::array({profiled("Index", "numeric", 60, {{1, 1}}),
                       profiled("id", "identifier", 60, {{"a", 1}}),
                       profiled("churned", "numeric", 2, {{0, 40}, {1, 20}})})}},
        json::array({{{"name", "Index"}, {"type", "integer"}},
                     {{"name", "id"}, {"type", "string"}},
                     {{"name", "churned"}, {"type", "integer"}}}));
    EXPECT_EQ(qc::find(columns, "Index")->at("kind"), "identifier");
    EXPECT_FALSE(qc::trains_as_is(columns, {"churned", "", "id"}, {}));
}

} // namespace
