// The quickstart process (lake#1128, docs/api/quickstart.md): start_quickstart, quickstart_status,
// train_quickstart and quickstart_snippets compose upload_dataset, create_index, expose_index,
// train_model and expose_model through the registry, and keep their state in the workspace Zetta.
// The workspace engine is the test double (StreamCaller); QuickstartZetta runs the whole process
// against a real Zetta when LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN name one.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/agent_docs.hpp"
#include "lake/api/balance.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_control.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "api/quickstart_columns.hpp"
#include "workspace_fixture.hpp"

namespace {

using json = nlohmann::json;
using lake::control::ZettaControlError;
using lake::control::ZettaControlResult;

constexpr std::string_view kek = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::int64_t dollar = 1'000'000;

const std::string movies = "title,genre,rating,popular\n"
                           "The Matrix,scifi,8.7,1\n"
                           "Amelie,romance,8.3,0\n"
                           "Alien,horror,8.5,1\n"
                           "Clue,comedy,7.2,0\n";

// A workspace engine for the quickstart: it keeps the datasets, indexes, runs and models it was
// asked to make, and fails a capability on request.
struct Engine {
    std::map<std::string, ZettaControlError> fail_next;
    // Each table's rows (SELECT answers them) and analyze_dataset's profile of it.
    std::map<std::string, std::vector<json>> tables;
    std::map<std::string, json> analyses;
    std::string run_status = "running";
    bool listed_model = true;
    int index_count = 0;
    int run_count = 0;

    std::optional<ZettaControlResult<json>> answer(const std::string& capability,
                                                   const json& input) {
        if (const auto failing = fail_next.find(capability); failing != fail_next.end()) {
            ZettaControlError error = failing->second;
            fail_next.erase(failing);
            return ZettaControlResult<json>(std::unexpected(error));
        }
        if (capability == "upload_dataset") {
            return json{{"database", input.value("database", std::string("datasets"))},
                        {"table", input.at("table_name")},
                        {"rows", 4},
                        {"format", input.at("format")}};
        }
        if (capability == "analyze_dataset") {
            const std::string path = input.value("dataset_path", std::string());
            const std::string table = path.substr(path.find('.') + 1);
            if (const auto found = analyses.find(table); found != analyses.end()) {
                return found->second;
            }
            // The movies: a title per row, a genre, a rating and whether it was popular.
            const auto column = [](const char* name, const char* type, int distinct, json top) {
                json values = json::array();
                for (const json& value : top) {
                    values.push_back({{"value", value}, {"count", 2}});
                }
                return json{{"name", name},
                            {"semantic_type", type},
                            {"distinct_count", distinct},
                            {"null_rate", 0.0},
                            {"top_values", values}};
            };
            return json{{"columns",
                         json::array({column("title", "identifier", 4, {"Alien"}),
                                      column("genre", "categorical", 4, {"scifi", "romance"}),
                                      column("rating", "numeric", 4, {8.7}),
                                      column("popular", "boolean", 2, {1, 0})})},
                        {"leakage_candidates", json::array()}};
        }
        if (capability == "sql") {
            const std::string statement = input.at("statement");
            const auto from = statement.find(" FROM ") + 6;
            const auto end = statement.find(' ', from);
            const std::string path = statement.substr(from, end - from);
            const std::vector<json>& rows = tables[path.substr(path.find('.') + 1)];
            if (statement.starts_with("SELECT COUNT(*)")) {
                return json{{"columns", {"count"}}, {"rows", {{{"count", rows.size()}}}}};
            }
            std::size_t limit = rows.size();
            std::size_t offset = 0;
            if (const auto at = statement.find(" LIMIT "); at != std::string::npos) {
                limit = std::stoul(statement.substr(at + 7));
            }
            if (const auto at = statement.find(" OFFSET "); at != std::string::npos) {
                offset = std::stoul(statement.substr(at + 8));
            }
            json page = json::array();
            for (std::size_t i = offset; i < rows.size() && i < offset + limit; ++i) {
                page.push_back(rows[i]);
            }
            return json{{"rows", page}};
        }
        if (capability == "get_dataset") {
            if (const auto found = analyses.find(input.value("dataset", std::string()));
                found != analyses.end()) {
                json columns = json::array();
                for (const json& column : found->second["columns"]) {
                    columns.push_back({{"name", column["name"]}, {"type", "string"}});
                }
                return json{{"name", input.at("dataset")},
                            {"database", "datasets"},
                            {"rows", tables[input.at("dataset")].size()},
                            {"columns", columns}};
            }
            if (input.value("dataset", std::string()) == "missing") {
                return ZettaControlResult<json>(
                    std::unexpected(ZettaControlError{"not_found", "no dataset named that"}));
            }
            return json{{"name", input.at("dataset")},
                        {"database", "datasets"},
                        {"rows", 12},
                        {"columns",
                         json::array({{{"name", "title"}, {"type", "string"}},
                                      {{"name", "genre"}, {"type", "string"}},
                                      {{"name", "rating"}, {"type", "number"}},
                                      {{"name", "popular"}, {"type", "integer"}}})}};
        }
        if (capability == "create_index") {
            ++index_count;
            return json{{"index_id", "index-" + std::to_string(index_count)},
                        {"fields", json::array({"title", "genre", "rating", "popular"})},
                        {"exposed", false},
                        {"kind", "search"},
                        {"table", input.at("table")}};
        }
        if (capability == "expose_index") {
            return json{{"index_id", input.at("index")}, {"exposed", true}};
        }
        // What create_browser_key reads to bind a key (lake#1152).
        if (capability == "get_index") {
            return json{{"index_id", input.at("index_id")}, {"exposed", true}};
        }
        if (capability == "get_model") {
            return json{{"id", input.at("model_id")}, {"exposed", true}};
        }
        if (capability == "search") {
            return json{{"hits", json::array()}};
        }
        if (capability == "train_model") {
            // zetta v0.5.0's segmentation (src/core/ml/segmentation_methodology.cpp) refuses a
            // held-out share, word for word (lake#1203).
            if (input.value("methodology", std::string()) == "M-3" &&
                (input.contains("holdout_fraction") || input.contains("top_k"))) {
                return ZettaControlResult<json>(std::unexpected(ZettaControlError{
                    "invalid_input",
                    "segmentation has no supervised holdout or ranked top_k; configure its K "
                    "range in the graph hints"}));
            }
            ++run_count;
            return json{{"run_id", "run-" + std::to_string(run_count)},
                        {"status", "running"},
                        {"standalone", true}};
        }
        if (capability == "get_run") {
            json run{{"id", input.at("run_id")}, {"status", run_status}};
            if (run_status == "failed") {
                run["error"] = "the label column has one value";
            }
            return run;
        }
        if (capability == "list_models") {
            json models = json::array();
            if (listed_model && run_status == "succeeded") {
                models.push_back({{"id", "model-" + std::to_string(run_count)},
                                  {"run_id", "run-" + std::to_string(run_count)},
                                  {"exposed", false}});
            }
            return json{{"models", models}};
        }
        if (capability == "expose_model") {
            return json{{"model_id", input.at("model")}, {"exposed", true}};
        }
        return std::nullopt;
    }
};

// Two accounts, each with a Ready workspace; billing on with a price list and the balance's
// runtime, which a test turns on to see the gate.
struct Quickstart {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    lake::api::Registry registry;
    Engine engine;
    std::shared_ptr<lake::api::PaymentProviders> providers =
        lake::api::fixed_payment_providers(lake::api::manual_payment_provider());
    std::shared_ptr<lake::api::BalanceRuntime> runtime =
        std::make_shared<lake::api::BalanceRuntime>();
    bool gated = false;
    struct Account {
        std::string identity, user_id, customer_id, workspace_id;
    };
    Account first, second;

    Account account(const std::string& name, bool with_workspace = true) {
        Account out;
        out.identity = "taap:1:ed25519:quickstart-" + name;
        const auto user = store->create_user({out.identity, name + "@example.test", name});
        EXPECT_TRUE(user) << user.error().message;
        out.user_id = user->id;
        const auto customer = store->create_customer({"individual", user->id, "", "qs" + name});
        EXPECT_TRUE(customer) << customer.error().message;
        out.customer_id = customer->id;
        const auto object = store->find_policy_object("customer", out.customer_id);
        EXPECT_TRUE(object);
        EXPECT_TRUE(store->create_grant({out.identity,
                                         object->id,
                                         "admin",
                                         "attestation-" + name,
                                         "admin",
                                         "request-" + name}));
        if (with_workspace) {
            out.workspace_id = workspaces.create(out.customer_id, "home");
        }
        return out;
    }

    Quickstart() {
        lake::api::register_core_capabilities(registry, "test");
        lake::api::register_billing_capabilities(registry, providers);
        workspaces.zetta->responder = [this](const std::string& capability, const json& input) {
            return engine.answer(capability, input);
        };
        runtime->origin = "https://lake.test";
        first = account("first");
        second = account("second");
        auto op = operator_context();
        const auto published = call(
            "set_price_list",
            {{"effective_from", "2026-01"},
             {"prices",
              json::array({{{"dimension", "cpu"}, {"unit_micros", 2'000}, {"per", 60'000}},
                           {{"dimension", "searches"}, {"unit_micros", 500'000}, {"per", 1000}}})}},
            op);
        EXPECT_TRUE(published.ok) << published.error.message;
    }

    lake::api::ApiContext operator_context() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        context.payments = providers;
        context.external_origin = "https://lake.test";
        if (gated) {
            context.balance = runtime;
        }
        context.registry = &registry;
        return context;
    }

    lake::api::ApiContext as(const Account& who) {
        auto context = operator_context();
        context.operator_credential = false;
        context.acting_identity = who.identity;
        context.acting_user_id = who.user_id;
        context.session_credential = true;
        context.credential_session_id = "session-" + who.user_id;
        context.scopes = {"*"};
        return context;
    }

    lake::api::ApiResult call(const std::string& name, json input, lake::api::ApiContext& context) {
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? std::move(*completed) : lake::api::ApiResult{};
    }

    lake::api::ApiResult by(const Account& who, const std::string& name, json input) {
        auto context = as(who);
        if (!input.contains("customer_id")) {
            input["customer_id"] = who.customer_id;
        }
        return call(name, std::move(input), context);
    }

    // The engine capabilities called, in order, without the record store's own calls.
    std::vector<std::string> engine_calls() const {
        std::vector<std::string> out;
        for (const auto& [capability, input] : workspaces.zetta->calls) {
            if (!lake::test::RecordTables::handles(capability, input)) {
                out.push_back(capability);
            }
        }
        return out;
    }

    static json step(const json& status, const std::string& name) {
        for (const json& entry : status.at("steps")) {
            if (entry.at("step") == name) {
                return entry;
            }
        }
        return nullptr;
    }
};

std::size_t count(const std::vector<std::string>& calls, const std::string& name) {
    return static_cast<std::size_t>(std::ranges::count(calls, name));
}

TEST(Quickstart, ADroppedDatasetBecomesSearchThenModelAnswers) {
    Quickstart q;
    const auto& me = q.first;
    const json where{{"workspace_id", me.workspace_id}};

    // Drop a CSV: the dataset, its index and publishing it, in that order.
    auto started = q.by(me,
                        "start_quickstart",
                        {{"workspace_id", me.workspace_id},
                         {"content", movies},
                         {"format", "csv"},
                         {"name", "movies"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const json& status = started.value;
    EXPECT_EQ(status["state"], "search_ready");
    const std::string id = status["quickstart_id"];
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(status["dataset"]["id"], "datasets/movies");
    EXPECT_EQ(status["dataset"]["rows"], 4);
    // The file's columns in its order (the index lists its fields sorted), for the model's columns.
    EXPECT_EQ(status["dataset"]["columns"][0]["name"], "title");
    EXPECT_EQ(status["dataset"]["columns"][3]["name"], "popular");
    EXPECT_EQ(status["index"]["id"], "index-1");
    EXPECT_EQ(status["index"]["published"], true);
    for (const char* done : {"workspace", "dataset", "search_index", "website"}) {
        EXPECT_EQ(Quickstart::step(status, done)["state"], "done") << done;
    }
    EXPECT_EQ(Quickstart::step(status, "model")["state"], "pending");
    EXPECT_EQ(status["search"]["capability"], "search_index");
    EXPECT_EQ(status["search"]["input"]["index_id"], "index-1");
    EXPECT_EQ(
        q.engine_calls(),
        (std::vector<std::string>{
            "upload_dataset", "get_dataset", "analyze_dataset", "create_index", "expose_index"}));
    // Each column's kind, from the engine's own profile (lake#1181), and the first offer.
    EXPECT_EQ(status["dataset"]["columns"][0]["kind"], "identifier");
    EXPECT_EQ(status["dataset"]["columns"][3]["kind"], "boolean");
    EXPECT_EQ(status["dataset"]["columns"][3]["type"], "integer");
    EXPECT_EQ(status["suggested"]["target"], "popular");
    EXPECT_EQ(status["suggested"]["entity_column"], "title");
    EXPECT_EQ(status["snapshot_date"], status["created_at"]) << "the upload time";
    // Through the same capabilities a person calls: upload_dataset's name rule and rename.
    for (const auto& [capability, input] : q.workspaces.zetta->calls) {
        if (capability == "upload_dataset") {
            EXPECT_EQ(input["table_name"], "movies");
            EXPECT_EQ(input["if_exists"], "rename");
        }
    }

    // Kept sealed in the workspace's own Zetta, and nowhere else.
    const auto& rows = q.workspaces.records(me.workspace_id).rows("quickstarts");
    ASSERT_EQ(rows.size(), 1U);
    for (const auto& [key, row] : rows) {
        EXPECT_EQ(row.dump().find("movies"), std::string::npos) << "sealed at rest";
    }
    EXPECT_TRUE(q.workspaces.records(q.second.workspace_id).rows("quickstarts").empty());

    // The status reads the same, and lists the quickstart.
    auto read = q.by(me, "quickstart_status", where);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["quickstart_id"], id);
    EXPECT_EQ(read.value["state"], "search_ready");
    ASSERT_EQ(read.value["quickstarts"].size(), 1U);
    EXPECT_EQ(read.value["quickstarts"][0]["dataset_id"], "datasets/movies");

    // The website code: Lake's host and the ids filled in, the call that makes its browser key.
    auto snippets = q.by(me, "quickstart_snippets", where);
    ASSERT_TRUE(snippets.ok) << snippets.error.message;
    EXPECT_EQ(snippets.value["ready"], true);
    EXPECT_EQ(snippets.value["includes_model"], false);
    EXPECT_EQ(snippets.value["key"]["create"]["capability"], "create_browser_key");
    EXPECT_EQ(snippets.value["key"]["capabilities"], json::array({"search_index"}));
    EXPECT_EQ(snippets.value["key"]["secret"], nullptr) << "without origins nothing is made";
    ASSERT_EQ(snippets.value["snippets"].size(), 3U);
    const std::string html = snippets.value["snippets"][0]["code"];
    EXPECT_EQ(snippets.value["snippets"][0]["id"], "html");
    EXPECT_NE(html.find("\"https://lake.test\""), std::string::npos);
    EXPECT_NE(html.find("index-1"), std::string::npos);
    EXPECT_NE(html.find(me.workspace_id), std::string::npos);
    EXPECT_NE(html.find("YOUR_LAKE_SEARCH_KEY"), std::string::npos);
    EXPECT_NE(html.find("/v1/search_index"), std::string::npos);
    EXPECT_NE(html.find("const MODEL = null"), std::string::npos);
    EXPECT_EQ(snippets.value["snippets"][1]["id"], "react");
    // The call the snippets name is create_browser_key's, bound to this index and workspace.
    const auto create_key = q.registry.find("create_browser_key");
    ASSERT_TRUE(create_key);
    const json& create_input = snippets.value["key"]["create"]["input"];
    const auto valid = lake::api::validate_input(*create_key, create_input);
    EXPECT_TRUE(valid.ok) << valid.error.message;
    EXPECT_EQ(create_input["index_id"], "index-1");
    EXPECT_EQ(create_input["workspace_id"], me.workspace_id);
    EXPECT_FALSE(create_input.contains("model_id"));
    const std::string curl = snippets.value["snippets"][2]["code"];
    EXPECT_NE(curl.find("-H \"Origin: https://www.example.com\""), std::string::npos) << curl;
    EXPECT_TRUE(q.store->list_browser_keys(me.customer_id, "")->empty());

    // The agent prompt (lake#1185), search only and without a key: everything a coding agent
    // needs, and how to make the key.
    {
        const std::string prompt = snippets.value["prompt"];
        for (const std::string needed : {std::string("https://lake.test/v1/search_index"),
                                         std::string("index-1"),
                                         me.workspace_id,
                                         me.customer_id,
                                         std::string("YOUR_LAKE_SEARCH_KEY"),
                                         std::string("create_browser_key"),
                                         std::string("\"origins\""),
                                         std::string("Website Keys"),
                                         std::string("\"hits\""),
                                         std::string("\"score\""),
                                         std::string("\"fields\""),
                                         std::string("429"),
                                         std::string("no results"),
                                         std::string("framework"),
                                         std::string("Access-Control-Allow-Origin"),
                                         std::string("https://lake.test/mcp"),
                                         std::string("https://lake.test/llms.txt"),
                                         std::string("```html"),
                                         std::string("```jsx"),
                                         std::string("export function LakeSearch"),
                                         std::string("600 calls a minute"),
                                         std::string("title, genre, rating, popular")}) {
            EXPECT_NE(prompt.find(needed), std::string::npos) << needed << "\n" << prompt;
        }
        EXPECT_EQ(prompt.find("POST https://lake.test/v1/predict"), std::string::npos)
            << "no model yet";
        EXPECT_EQ(prompt.find("`predict` value"), std::string::npos);
        EXPECT_EQ(prompt.find("lbk."), std::string::npos) << "no secret it was not given";
    }

    // With the website's origin, the call makes the key and puts it in the code, shown once.
    json for_site = where;
    for_site["origins"] = json::array({"https://shop.example.test"});
    auto keyed = q.by(me, "quickstart_snippets", for_site);
    ASSERT_TRUE(keyed.ok) << keyed.error.message;
    const std::string secret = keyed.value["key"]["secret"];
    EXPECT_TRUE(secret.starts_with("lbk.")) << secret;
    EXPECT_EQ(keyed.value["key"]["browser_key"]["origins"],
              json::array({"https://shop.example.test"}));
    EXPECT_EQ(keyed.value["key"]["browser_key"]["index_id"], "index-1");
    const std::string keyed_html = keyed.value["snippets"][0]["code"];
    EXPECT_NE(keyed_html.find(secret), std::string::npos);
    EXPECT_EQ(keyed_html.find("YOUR_LAKE_SEARCH_KEY"), std::string::npos);
    EXPECT_NE(
        std::string(keyed.value["snippets"][2]["code"]).find("Origin: https://shop.example.test"),
        std::string::npos);
    ASSERT_EQ(q.store->list_browser_keys(me.customer_id, me.workspace_id)->size(), 1U);
    // The key this call made is in the prompt, with the origins it allows.
    {
        const std::string prompt = keyed.value["prompt"];
        EXPECT_NE(prompt.find("Bearer " + secret), std::string::npos);
        EXPECT_NE(prompt.find("  - https://shop.example.test"), std::string::npos) << prompt;
        EXPECT_EQ(prompt.find("YOUR_LAKE_SEARCH_KEY"), std::string::npos);
    }
    // The key it made searches the quickstart's index from that website, as no person.
    {
        auto website = q.operator_context();
        website.operator_credential = false;
        website.acting_identity.clear();
        const auto principal = q.store->context_for(secret);
        ASSERT_TRUE(principal) << principal.error().message;
        website.acting_identity = principal->taap_id;
        website.credential_kind = principal->kind;
        website.browser_key = principal->browser_key;
        website.caller_origin = "https://shop.example.test";
        const auto searched = q.call("search_index",
                                     {{"customer_id", me.customer_id},
                                      {"workspace_id", me.workspace_id},
                                      {"index_id", "index-1"},
                                      {"query", "alien"}},
                                     website);
        EXPECT_TRUE(searched.ok) << searched.error.message;
    }
    // A bad origin is create_browser_key's refusal, word for word, and makes nothing.
    for_site["origins"] = json::array({"shop.example.test/x y"});
    const auto refused_site = q.by(me, "quickstart_snippets", for_site);
    EXPECT_EQ(refused_site.error.code, "invalid_input");
    EXPECT_EQ(refused_site.error.path, "/origins/0");
    EXPECT_EQ(q.store->list_browser_keys(me.customer_id, me.workspace_id)->size(), 1U);

    // One click trains a model on the same dataset.
    auto training = q.by(
        me,
        "train_quickstart",
        {{"workspace_id", me.workspace_id}, {"target", "popular"}, {"entity_column", "title"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    EXPECT_EQ(training.value["state"], "training");
    EXPECT_EQ(training.value["training"]["run_id"], "run-1");
    EXPECT_EQ(training.value["training"]["methodology"], "M-1");
    EXPECT_EQ(training.value["training"]["holdout_fraction"], 0.2);
    // Ready as it is: no training copy, and no rows read.
    EXPECT_EQ(training.value["training"]["preparation"]["copy"], false);
    EXPECT_EQ(count(q.engine_calls(), "sql"), 0U);
    for (const auto& [capability, input] : q.workspaces.zetta->calls) {
        if (capability == "train_model") {
            EXPECT_EQ(input["dataset_path"], "datasets.movies");
            EXPECT_EQ(input["target"], "popular");
            EXPECT_EQ(input["entity_column"], "title");
            EXPECT_EQ(input["methodology"], "M-1");
        }
    }

    // Its progress, read live; a second train while it runs changes nothing.
    read = q.by(me, "quickstart_status", where);
    EXPECT_EQ(read.value["state"], "training");
    EXPECT_EQ(Quickstart::step(read.value, "model")["state"], "running");
    EXPECT_EQ(Quickstart::step(read.value, "model")["progress"]["status"], "running");
    auto again = q.by(me, "train_quickstart", where);
    EXPECT_EQ(again.value["state"], "training");
    EXPECT_EQ(count(q.engine_calls(), "train_model"), 1U);

    // Trained: the status names the model; the next train publishes it.
    q.engine.run_status = "succeeded";
    read = q.by(me, "quickstart_status", where);
    EXPECT_EQ(read.value["state"], "trained");
    EXPECT_EQ(read.value["model"]["id"], "model-1");
    EXPECT_EQ(count(q.engine_calls(), "expose_model"), 0U) << "a read publishes nothing";
    auto published = q.by(me, "train_quickstart", where);
    ASSERT_TRUE(published.ok) << published.error.message;
    EXPECT_EQ(published.value["state"], "done");
    EXPECT_EQ(published.value["model"]["published"], true);
    EXPECT_EQ(published.value["predict"]["input"]["model_id"], "model-1");
    EXPECT_EQ(count(q.engine_calls(), "expose_model"), 1U);
    for (const char* done : {"model", "model_answers"}) {
        EXPECT_EQ(Quickstart::step(published.value, done)["state"], "done") << done;
    }

    // The snippets grow the model's answer beside each result.
    snippets = q.by(me, "quickstart_snippets", where);
    EXPECT_EQ(snippets.value["includes_model"], true);
    EXPECT_EQ(snippets.value["key"]["capabilities"], json::array({"search_index", "predict"}));
    EXPECT_EQ(snippets.value["key"]["create"]["input"]["model_id"], "model-1");
    const std::string with_model = snippets.value["snippets"][0]["code"];
    EXPECT_NE(with_model.find("/v1/predict"), std::string::npos);
    EXPECT_NE(with_model.find("model-1"), std::string::npos);
    EXPECT_NE(with_model.find("\"popular\""), std::string::npos);
    const std::string react = snippets.value["snippets"][1]["code"];
    EXPECT_NE(react.find("export function LakeSearch"), std::string::npos);
    EXPECT_NE(react.find("/v1/predict"), std::string::npos);
    // With the model published, the prompt asks for its answers too (lake#1185).
    {
        const std::string prompt = snippets.value["prompt"];
        for (const std::string needed : {"https://lake.test/v1/predict",
                                         "model-1",
                                         "\"ranked\"",
                                         "\"risk\"",
                                         "how likely popular",
                                         "the tools `search_index` and `predict`",
                                         "and ask this model"}) {
            EXPECT_NE(prompt.find(needed), std::string::npos) << needed << "\n" << prompt;
        }
    }

    // Done stays done: calling again calls nothing.
    const std::size_t before = q.engine_calls().size();
    EXPECT_EQ(q.by(me, "train_quickstart", where).value["state"], "done");
    EXPECT_EQ(q.engine_calls().size(), before);

    // Another quickstart in the same workspace keeps the first.
    auto other = q.by(me,
                      "start_quickstart",
                      {{"workspace_id", me.workspace_id}, {"dataset_id", "datasets/existing"}});
    ASSERT_TRUE(other.ok) << other.error.message;
    EXPECT_NE(other.value["quickstart_id"], id);
    EXPECT_EQ(other.value["dataset"]["source"], "existing");
    read = q.by(me, "quickstart_status", where);
    EXPECT_EQ(read.value["quickstart_id"], other.value["quickstart_id"]) << "the newest";
    EXPECT_EQ(read.value["quickstarts"].size(), 2U);
    read =
        q.by(me, "quickstart_status", {{"workspace_id", me.workspace_id}, {"quickstart_id", id}});
    EXPECT_EQ(read.value["state"], "done");
}

TEST(Quickstart, AStepThatFailsIsKeptWithWhyAndResumed) {
    Quickstart q;
    const auto& me = q.first;
    const json where{{"workspace_id", me.workspace_id}};
    q.engine.fail_next["create_index"] = {"engine_error", "the index backend is busy"};
    auto started =
        q.by(me,
             "start_quickstart",
             {{"workspace_id", me.workspace_id}, {"content", movies}, {"format", "csv"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["state"], "failed");
    EXPECT_EQ(Quickstart::step(started.value, "dataset")["state"], "done");
    const json failed = Quickstart::step(started.value, "search_index");
    EXPECT_EQ(failed["state"], "failed");
    EXPECT_NE(failed["error"]["message"].get<std::string>().find("busy"), std::string::npos);
    EXPECT_NE(started.value["next"].get<std::string>().find("start_quickstart"), std::string::npos);
    const std::string id = started.value["quickstart_id"];

    // Someone who leaves and comes back sees where it stopped.
    auto read = q.by(me, "quickstart_status", where);
    EXPECT_EQ(read.value["state"], "failed");
    EXPECT_EQ(read.value["dataset"]["name"], "quickstart");

    // Resumed: the dataset is not loaded twice.
    auto resumed =
        q.by(me, "start_quickstart", {{"workspace_id", me.workspace_id}, {"quickstart_id", id}});
    ASSERT_TRUE(resumed.ok) << resumed.error.message;
    EXPECT_EQ(resumed.value["state"], "search_ready");
    EXPECT_EQ(Quickstart::step(resumed.value, "search_index")["error"], nullptr);
    EXPECT_EQ(count(q.engine_calls(), "upload_dataset"), 1U);
    EXPECT_EQ(count(q.engine_calls(), "create_index"), 2U);
    // A quickstart with its dataset takes no second file.
    auto twice = q.by(me,
                      "start_quickstart",
                      {{"workspace_id", me.workspace_id},
                       {"quickstart_id", id},
                       {"content", movies},
                       {"format", "csv"}});
    EXPECT_EQ(twice.error.code, "invalid_input");
    EXPECT_EQ(twice.error.path, "/content");

    // A training run that fails says why; the next call trains again with the same settings.
    auto training =
        q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}, {"target", "popular"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    q.engine.run_status = "failed";
    read = q.by(me, "quickstart_status", where);
    EXPECT_EQ(read.value["state"], "failed");
    // In plain words, with the engine's own reason beside it (lake#1181).
    EXPECT_NE(Quickstart::step(read.value, "model")["error"]["message"].get<std::string>().find(
                  "same answer for popular"),
              std::string::npos);
    EXPECT_EQ(Quickstart::step(read.value, "model")["error"]["detail"],
              "the label column has one value");
    auto retried = q.by(me, "train_quickstart", where);
    ASSERT_TRUE(retried.ok) << retried.error.message;
    EXPECT_EQ(retried.value["state"], "training");
    EXPECT_EQ(retried.value["training"]["attempt"], 2);
    EXPECT_EQ(retried.value["training"]["target"], "popular") << "the settings before";
    EXPECT_EQ(count(q.engine_calls(), "train_model"), 2U);
    std::vector<std::string> names;
    for (const auto& [capability, input] : q.workspaces.zetta->calls) {
        if (capability == "train_model") {
            names.push_back(input["name"]);
        }
    }
    ASSERT_EQ(names.size(), 2U);
    EXPECT_NE(names[0], names[1]) << "a standalone model's name is taken once";

    // A refused train is kept with why and tried again.
    q.engine.run_status = "succeeded";
    q.engine.fail_next["expose_model"] = {"engine_error", "serving is starting"};
    auto exposing = q.by(me, "train_quickstart", where);
    ASSERT_TRUE(exposing.ok) << exposing.error.message;
    EXPECT_EQ(exposing.value["state"], "failed");
    EXPECT_EQ(Quickstart::step(exposing.value, "model")["state"], "done");
    EXPECT_EQ(Quickstart::step(exposing.value, "model_answers")["state"], "failed");
    EXPECT_NE(exposing.value["next"].get<std::string>().find("train_quickstart"),
              std::string::npos);
    auto exposed = q.by(me, "train_quickstart", where);
    EXPECT_EQ(exposed.value["state"], "done");
}

// lake#1181: Steve's dataset on lake-dev (v0.7.0): a first name, a country and a date, with an id,
// free text, numbers, an empty column and one that never changes. Training refused with
// `transform_required: add_snapshot_date`; now every kind is detected and prepared.
struct People {
    using Top = std::vector<std::pair<json, int>>;
    static json profile(
        const char* name, const char* type, int distinct, const Top& top, double null_rate = 0.0) {
        json values = json::array();
        for (const auto& [value, count] : top) {
            values.push_back({{"value", value}, {"count", count}});
        }
        return {{"name", name},
                {"semantic_type", type},
                {"distinct_count", distinct},
                {"null_rate", null_rate},
                {"top_values", values}};
    }

    static void load(Engine& engine) {
        engine.analyses["people"] = {
            {"columns",
             json::array(
                 {profile("customer_id", "identifier", 12, {{"c-0", 1}}),
                  profile("First Name", "categorical", 3, {{"Ana", 4}, {"Ben", 4}, {"Chen", 4}}),
                  profile(
                      "Country", "categorical", 3, {{"Canada", 6}, {"France", 3}, {"Japan", 3}}),
                  profile("Signup Date", "datetime", 12, {{"2025-01-15", 1}}),
                  // A date column the profile missed: its values are dates all the same.
                  profile("Renewed", "categorical", 12, {{"2026-01-15", 1}}),
                  profile("Spend", "numeric", 12, {{10.5, 1}}),
                  profile("Bio", "free_text", 12, {{"Likes hiking", 1}}),
                  profile("Notes", "unknown", 0, Top{}, 1.0),
                  profile("Region", "categorical", 1, {{"EU", 12}})})},
            {"leakage_candidates", json::array()}};
        const std::vector<std::string> names{"Ana", "Ben", "Chen"};
        const std::vector<std::string> countries{"Canada", "France", "Canada", "Japan"};
        for (int i = 0; i < 12; ++i) {
            const std::string month = (i < 9 ? "0" : "") + std::to_string(i + 1);
            engine.tables["people"].push_back(
                {{"customer_id", "c-" + std::to_string(i)},
                 {"First Name", names[static_cast<std::size_t>(i % 3)]},
                 {"Country", countries[static_cast<std::size_t>(i % 4)]},
                 {"Signup Date", "2025-" + month + "-15"},
                 {"Renewed", "2026-" + month + "-15"},
                 {"Spend", 10.5 + i},
                 {"Bio", "Likes hiking and long walks number " + std::to_string(i)},
                 {"Notes", nullptr},
                 {"Region", "EU"}});
        }
    }
};

// The JSON Lines of every upload_dataset call of the training copy.
std::vector<std::vector<json>> copies(const Quickstart& q) {
    std::vector<std::vector<json>> out;
    for (const auto& [capability, input] : q.workspaces.zetta->calls) {
        if (capability == "upload_dataset" &&
            input.value("table_name", std::string()).ends_with("_training")) {
            std::vector<json> rows;
            std::istringstream lines(input.at("content").get<std::string>());
            for (std::string line; std::getline(lines, line);) {
                rows.push_back(json::parse(line));
            }
            out.push_back(std::move(rows));
        }
    }
    return out;
}

std::vector<json> calls_of(const Quickstart& q, const std::string& name) {
    std::vector<json> out;
    for (const auto& [capability, input] : q.workspaces.zetta->calls) {
        if (capability == name) {
            out.push_back(input);
        }
    }
    return out;
}

TEST(Quickstart, AnyColumnTypesTrainADatedDatasetIsPreparedNotRefused) {
    Quickstart q;
    People::load(q.engine);
    const auto& me = q.first;
    const json where{{"workspace_id", me.workspace_id}};
    auto started = q.by(me,
                        "start_quickstart",
                        {{"workspace_id", me.workspace_id},
                         {"content", "customer_id,First Name\nc-0,Ana\n"},
                         {"format", "csv"},
                         {"name", "people"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const json& status = started.value;
    ASSERT_EQ(status["dataset"]["columns"].size(), 9U);
    std::map<std::string, std::string> kinds;
    for (const json& column : status["dataset"]["columns"]) {
        kinds[column["name"]] = column["kind"];
    }
    EXPECT_EQ(kinds["customer_id"], "identifier");
    EXPECT_EQ(kinds["Country"], "categorical");
    EXPECT_EQ(kinds["Signup Date"], "datetime");
    EXPECT_EQ(kinds["Spend"], "numeric");
    EXPECT_EQ(kinds["Bio"], "free_text");
    EXPECT_EQ(kinds["Notes"], "empty");
    EXPECT_EQ(kinds["Region"], "constant");
    EXPECT_EQ(status["suggested"]["target"], "Country") << "no two-valued column: a category";
    EXPECT_EQ(status["suggested"]["target_value"], "Canada");
    EXPECT_EQ(status["suggested"]["entity_column"], "customer_id");
    const std::string snapshot = status["snapshot_date"];
    EXPECT_EQ(status["created_at"], snapshot);
    // The engine was told the snapshot date.
    ASSERT_FALSE(calls_of(q, "analyze_dataset").empty());
    EXPECT_EQ(calls_of(q, "analyze_dataset").back()["as_of"], snapshot);

    // Steve's exact choice: Country to predict, First Name names a row.
    auto training = q.by(me,
                         "train_quickstart",
                         {{"workspace_id", me.workspace_id},
                          {"target", "Country"},
                          {"entity_column", "First Name"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_EQ(training.value["state"], "training") << training.value.dump(2);
    const json& trained = training.value["training"];
    EXPECT_EQ(trained["target"], "Country");
    EXPECT_EQ(trained["target_value"], "Canada") << "the most common value";
    EXPECT_EQ(trained["as_of"], snapshot);
    const json& preparation = trained["preparation"];
    EXPECT_EQ(preparation["copy"], true);
    EXPECT_EQ(preparation["dataset_id"], "datasets/people_training");
    // First Name repeats, so the rows are numbered and First Name is learned from.
    EXPECT_EQ(preparation["entity"], "row_number");
    EXPECT_NE(preparation["note"].get<std::string>().find("First Name repeats"), std::string::npos);
    EXPECT_EQ(trained["entity_column"], "First Name") << "the person's choice is kept";
    EXPECT_EQ(preparation["label"]["positive"], "Canada");
    std::map<std::string, std::string> dropped;
    for (const json& entry : preparation["dropped"]) {
        dropped[entry["name"]] = entry["reason"];
    }
    for (const char* left : {"customer_id", "Signup Date", "Bio", "Notes", "Region"}) {
        ASSERT_TRUE(dropped.contains(left)) << left;
        EXPECT_FALSE(dropped[left].empty()) << left;
    }
    EXPECT_NE(dropped["Signup Date"].find("date"), std::string::npos);
    // The engine was asked with the column to predict, for its leakage checks.
    EXPECT_EQ(calls_of(q, "analyze_dataset").back()["label"], "Country");

    // What was uploaded: the usable columns and the answer as 0/1, no date the profile named.
    const auto uploaded = copies(q);
    ASSERT_EQ(uploaded.size(), 1U);
    ASSERT_EQ(uploaded[0].size(), 12U);
    EXPECT_EQ(uploaded[0][0]["Country"], 1);
    EXPECT_EQ(uploaded[0][1]["Country"], 0);
    EXPECT_EQ(uploaded[0][0]["First Name"], "Ana");
    EXPECT_FALSE(uploaded[0][0].contains("Signup Date"));
    EXPECT_FALSE(uploaded[0][0].contains("customer_id"));
    EXPECT_FALSE(uploaded[0][0].contains("Bio"));
    EXPECT_TRUE(uploaded[0][0].contains("Spend"));
    const auto trains = calls_of(q, "train_model");
    ASSERT_EQ(trains.size(), 1U);
    EXPECT_EQ(trains[0]["dataset_path"], "datasets.people_training");
    EXPECT_EQ(trains[0]["target"], "Country");
    EXPECT_EQ(trains[0]["entity_column"], "row_number");
    // The rows were read as the person, through the engine's SQL.
    EXPECT_GE(count(q.engine_calls(), "sql"), 2U);

    // Kept in the workspace: the status reads the same.
    auto read = q.by(me, "quickstart_status", where);
    EXPECT_EQ(read.value["training"]["preparation"]["dataset_id"], "datasets/people_training");
    EXPECT_EQ(read.value["snapshot_date"], snapshot);
}

// lake#1182: when no column is a yes-or-no answer, a target derived from a number column trains,
// and the column it is derived from is left out of what the model learns from.
TEST(Quickstart, ATargetDerivedFromANumberColumnTrainsOnTheTrainingCopy) {
    Quickstart q;
    People::load(q.engine);
    const auto& me = q.first;
    ASSERT_TRUE(q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id},
                      {"content", "customer_id\nc-0\n"},
                      {"format", "csv"},
                      {"name", "people"}})
                    .ok);
    auto training = q.by(me,
                         "train_quickstart",
                         {{"workspace_id", me.workspace_id},
                          {"derived_target", {{"column", "Spend"}, {"above", 15}}}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_EQ(training.value["state"], "training") << training.value.dump(2);
    const json& trained = training.value["training"];
    EXPECT_EQ(trained["target"], "spend_above_15");
    EXPECT_EQ(trained["target_value"], "yes");
    EXPECT_EQ(trained["derived_target"]["rule"], "Spend above 15");
    EXPECT_EQ(trained["derived_target"]["name"], "spend_above_15");
    EXPECT_EQ(trained["preparation"]["copy"], true);
    const auto uploaded = copies(q);
    ASSERT_EQ(uploaded.size(), 1U);
    ASSERT_EQ(uploaded[0].size(), 12U);
    EXPECT_EQ(uploaded[0][0]["spend_above_15"], 0);  // 10.5
    EXPECT_EQ(uploaded[0][11]["spend_above_15"], 1); // 21.5
    EXPECT_FALSE(uploaded[0][0].contains("Spend")) << "it would give the answer away";
    const auto trains = calls_of(q, "train_model");
    ASSERT_EQ(trains.size(), 1U);
    EXPECT_EQ(trains[0]["target"], "spend_above_15");
    EXPECT_EQ(trains[0]["dataset_path"], "datasets.people_training");

    // A rule that cannot teach anything is refused before anything is trained (the run failed, so
    // the next call trains again with what it sends).
    q.engine.run_status = "failed";
    auto refused = q.by(me,
                        "train_quickstart",
                        {{"workspace_id", me.workspace_id},
                         {"derived_target", {{"column", "Spend"}, {"above", 500}}}});
    EXPECT_FALSE(refused.ok) << refused.value.dump(2);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_NE(refused.error.message.find("No row has Spend above 500"), std::string::npos)
        << refused.error.message;
    auto wrong_kind = q.by(me,
                           "train_quickstart",
                           {{"workspace_id", me.workspace_id},
                            {"derived_target", {{"column", "Country"}, {"below", 3}}}});
    EXPECT_FALSE(wrong_kind.ok);
    EXPECT_EQ(wrong_kind.error.path, "/derived_target/column");
}

// lake#1182: segmentation / clustering (M-3) trains without a column to predict, keyed by the
// column that names a row, and its model is not published for the website.
TEST(Quickstart, AClusteringModelTrainsWithoutATargetAndIsNotPublishedForTheWebsite) {
    Quickstart q;
    People::load(q.engine);
    const auto& me = q.first;
    ASSERT_TRUE(q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id},
                      {"content", "customer_id\nc-0\n"},
                      {"format", "csv"},
                      {"name", "people"}})
                    .ok);
    auto training =
        q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}, {"methodology", "M-3"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_EQ(training.value["state"], "training") << training.value.dump(2);
    EXPECT_EQ(training.value["training"]["target"], nullptr);
    EXPECT_EQ(training.value["training"]["methodology"], "M-3");
    const auto trains = calls_of(q, "train_model");
    ASSERT_EQ(trains.size(), 1U);
    EXPECT_FALSE(trains[0].contains("target"));
    EXPECT_EQ(trains[0]["methodology"], "M-3");
    EXPECT_EQ(trains[0]["entity_column"], "customer_id") << "the detected identifier";
    EXPECT_EQ(trains[0]["dataset_path"], "datasets.people") << "unique names: as it is";
    // lake#1203: segmentation has no held-out share, and the engine chooses the segments itself.
    EXPECT_FALSE(trains[0].contains("holdout_fraction"));
    EXPECT_EQ(training.value["training"]["holdout_fraction"], nullptr);
    EXPECT_TRUE(copies(q).empty());
    // What a confirm in the conversation says, and what Ask Orama's page says, while it trains
    // (lake#1204).
    const auto started = lake::api::quickstart_columns::confirm_outcome(training.value);
    ASSERT_TRUE(started);
    EXPECT_TRUE(started->ok);
    EXPECT_NE(started->text.find("Training started: a clustering model on people"),
              std::string::npos)
        << started->text;
    EXPECT_NE(started->text.find("Train → Models shows the segments"), std::string::npos);

    q.engine.run_status = "succeeded";
    auto finished = q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}});
    ASSERT_TRUE(finished.ok) << finished.error.message;
    EXPECT_EQ(finished.value["state"], "done") << finished.value.dump(2);
    EXPECT_EQ(finished.value["model"]["serves"], false);
    EXPECT_EQ(finished.value["model"]["published"], false);
    const json answers = Quickstart::step(finished.value, "model_answers");
    EXPECT_EQ(answers["state"], "skipped");
    EXPECT_FALSE(answers["note"].get<std::string>().empty());
    EXPECT_EQ(count(q.engine_calls(), "expose_model"), 0U);
    EXPECT_EQ(finished.value["predict"], nullptr);
    const auto trained = lake::api::quickstart_columns::confirm_outcome(finished.value);
    ASSERT_TRUE(trained);
    EXPECT_NE(trained->text.find("The clustering model is trained"), std::string::npos);
}

// lake#1203, lake#1204: a clustering the engine refuses to start is said in plain words on the
// model step, the choices are kept so Try again trains the same clustering, and a confirm in the
// conversation is a failure, not "Done: train_quickstart".
TEST(Quickstart, ARefusedClusteringIsSaidPlainlyAndTryAgainKeepsIt) {
    Quickstart q;
    People::load(q.engine);
    const auto& me = q.first;
    ASSERT_TRUE(q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id},
                      {"content", "customer_id\nc-0\n"},
                      {"format", "csv"},
                      {"name", "people"}})
                    .ok);
    q.engine.fail_next["train_model"] = {
        "invalid_input", "segmentation: the table has no eligible feature columns"};
    auto refused =
        q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}, {"methodology", "M-3"}});
    ASSERT_TRUE(refused.ok) << refused.error.message;
    EXPECT_EQ(refused.value["state"], "failed");
    const json model = Quickstart::step(refused.value, "model");
    EXPECT_EQ(model["state"], "failed");
    const std::string message = model["error"]["message"];
    for (const char* internal : {"segmentation", "configure", "feature column", "train_model",
                                 "graph hints", "K range"}) {
        EXPECT_EQ(message.find(internal), std::string::npos) << internal << ": " << message;
    }
    EXPECT_NE(message.find("Clustering groups rows"), std::string::npos) << message;
    EXPECT_EQ(model["error"]["detail"], "segmentation: the table has no eligible feature columns")
        << "the engine's reason, for an agent";
    EXPECT_EQ(refused.value["training"]["methodology"], "M-3") << "kept for Try again";
    EXPECT_EQ(refused.value["training"]["run_id"], nullptr);
    const auto outcome = lake::api::quickstart_columns::confirm_outcome(refused.value);
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->ok);
    EXPECT_EQ(outcome->text, message);

    // The status reads it again without a run, and Ask Orama's page says it failed and why.
    auto status = q.by(me, "quickstart_status", {{"workspace_id", me.workspace_id}});
    ASSERT_TRUE(status.ok) << status.error.message;
    EXPECT_EQ(status.value["state"], "failed");

    // Try again: the same clustering, with nothing sent.
    auto again = q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["state"], "training") << again.value.dump(2);
    const auto trains = calls_of(q, "train_model");
    ASSERT_EQ(trains.size(), 2U);
    EXPECT_EQ(trains[1]["methodology"], "M-3");
    EXPECT_FALSE(trains[1].contains("holdout_fraction"));
    EXPECT_EQ(again.value["training"]["attempt"], 2);

    // The engine's segmentation refusal word for word (the lake-dev failure of lake#1203), and a
    // failed run: in plain words, never "configure its K range".
    const lake::api::quickstart_columns::Choice none{};
    for (const char* reason :
         {"segmentation has no supervised holdout or ranked top_k; configure its K range in the "
          "graph hints",
          "segmentation: the global sample has fewer rows than minimum_k",
          "segmentation: a row is missing entity column 'id'"}) {
        const std::string said =
            lake::api::quickstart_columns::explain_run_failure(reason, none, "M-3");
        EXPECT_EQ(said.find("configure"), std::string::npos) << said;
        EXPECT_EQ(said.find("segmentation"), std::string::npos) << said;
        EXPECT_EQ(said.find("minimum_k"), std::string::npos) << said;
        EXPECT_FALSE(said.empty());
    }
    EXPECT_EQ(lake::api::quickstart_columns::clustering_problem(json(1)).empty(), false);
    EXPECT_TRUE(lake::api::quickstart_columns::clustering_problem(json(2)).empty());
    EXPECT_TRUE(lake::api::quickstart_columns::clustering_problem(json(nullptr)).empty());
}

// Ask Orama on lake's agent bus, as the deployed Lake runs it: a turn is a task the agent claims.
struct AskOramaOnTheBus {
    class NoDirectCalls final : public lake::control::AgentCaller {
    public:
        liquid::runtime::task<ZettaControlResult<json>>
        turn(std::string, std::string, json) override {
            co_return ZettaControlResult<json>(std::unexpected(
                ZettaControlError{"store_unavailable", "library agents use the bus"}));
        }
    };
    std::shared_ptr<lake::control::WorkspaceService> service;
    std::shared_ptr<lake::control::MemoryAgentBus> dispatch =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> bus;
    NoDirectCalls agents;

    explicit AskOramaOnTheBus(Quickstart& q) {
        lake::control::WorkspaceConfig config{q.workspaces.dir.string(), 0};
        config.agent_endpoints = {{"agent-ask-orama", "http://ask:8080"}};
        config.internal_url = "http://lake.internal";
        service =
            std::make_shared<lake::control::WorkspaceService>(config, q.store, q.workspaces.zetta);
        bus = std::make_shared<lake::control::AgentTaskBus>(
            q.store,
            dispatch,
            std::vector<std::string>{"agent-ask-orama"},
            std::chrono::seconds(0));
        EXPECT_TRUE(lake::control::settled(liquid::runtime::block_on(bus->start())));
    }

    lake::api::ApiResult
    by(Quickstart& q, const Quickstart::Account& who, const std::string& name, json input) {
        auto context = q.as(who);
        context.workspaces = service.get();
        context.agent_bus = bus.get();
        context.agent_caller = &agents;
        if (!input.contains("customer_id")) {
            input["customer_id"] = who.customer_id;
        }
        return q.call(name, std::move(input), context);
    }
};

// lake#1182: the model step is an Ask Orama conversation about the dataset just added. Its first
// turn carries the dataset, its columns and the index, and quickstart_status names it.
TEST(Quickstart, TheModelStepIsAnAskOramaConversationAboutTheDataset) {
    Quickstart q;
    AskOramaOnTheBus ask(q);
    const auto& me = q.first;
    const json where{{"workspace_id", me.workspace_id}};
    ASSERT_TRUE(ask.by(q,
                       me,
                       "start_quickstart",
                       {{"workspace_id", me.workspace_id},
                        {"content", movies},
                        {"format", "csv"},
                        {"name", "movies"}})
                    .ok);
    auto before = q.by(me, "quickstart_status", where);
    ASSERT_TRUE(before.ok);
    EXPECT_EQ(before.value["conversation"], nullptr);
    EXPECT_NE(before.value["next"].get<std::string>().find("start_quickstart_conversation"),
              std::string::npos);

    auto opened = ask.by(q, me, "start_quickstart_conversation", where);
    ASSERT_TRUE(opened.ok) << opened.error.code << ": " << opened.error.message;
    const json& conversation = opened.value["conversation"];
    ASSERT_TRUE(conversation.is_object()) << opened.value.dump(2);
    const std::string id = conversation["conversation_id"];
    EXPECT_FALSE(id.empty());
    const json& page = conversation["page"];
    EXPECT_EQ(page["dataset"], "movies");
    EXPECT_EQ(page["dataset_id"], "datasets/movies");
    EXPECT_EQ(page["rows"], "4");
    EXPECT_EQ(page["search_index"], "index-1");
    EXPECT_EQ(page["quickstart_id"], opened.value["quickstart_id"]);
    EXPECT_NE(page["columns"].get<std::string>().find("popular"), std::string::npos);
    EXPECT_EQ(page["predictable_columns"], "genre, popular");
    EXPECT_EQ(page["quickstart_state"], "deciding") << "lake#1204: where the Quickstart is";
    for (const auto& [key, value] : page.items()) {
        EXPECT_TRUE(value.is_string()) << key << ": send_ask_message's page takes text";
    }
    EXPECT_EQ(conversation["read"]["capability"], "get_ask_conversation");
    EXPECT_EQ(conversation["reply"]["capability"], "send_ask_message");
    EXPECT_EQ(conversation["reply"]["input"]["conversation_id"], id);

    // The person's opening message is in the workspace's conversation, which they can read.
    auto read = ask.by(q,
                       me,
                       "get_ask_conversation",
                       {{"workspace_id", me.workspace_id}, {"conversation_id", id}});
    ASSERT_TRUE(read.ok) << read.error.message;
    ASSERT_FALSE(read.value["messages"].empty());
    EXPECT_EQ(read.value["messages"][0]["role"], "user");
    EXPECT_NE(read.value["messages"][0]["content"].get<std::string>().find("movies"),
              std::string::npos);

    // Named by the status, kept, and answered again rather than started twice.
    auto status = q.by(me, "quickstart_status", where);
    EXPECT_EQ(status.value["conversation"]["conversation_id"], id);
    auto again = ask.by(q, me, "start_quickstart_conversation", where);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(again.value["conversation"]["conversation_id"], id);
    auto restarted = ask.by(q,
                            me,
                            "start_quickstart_conversation",
                            {{"workspace_id", me.workspace_id},
                             {"restart", true},
                             {"message", "Ideas for a dashboard?"}});
    ASSERT_TRUE(restarted.ok) << restarted.error.message;
    EXPECT_NE(restarted.value["conversation"]["conversation_id"], id);

    // The turn is a task for Ask Orama carrying the page (lake#269): nothing waited on the agent.
    const std::string task = opened.value["conversation"]["task_id"];
    ASSERT_FALSE(task.empty());
    const auto asked = q.store->get_agent_task(task);
    ASSERT_TRUE(asked);
    EXPECT_EQ(asked->agent, "agent-ask-orama");
    EXPECT_EQ(asked->conversation_id, id);

    // Another account cannot open it in this workspace.
    auto other = ask.by(q,
                        q.second,
                        "start_quickstart_conversation",
                        {{"customer_id", me.customer_id}, {"workspace_id", me.workspace_id}});
    EXPECT_FALSE(other.ok);
}

TEST(Quickstart, ATransformRequiredLakeCanSatisfyIsAppliedAndTriedOnce) {
    Quickstart q;
    People::load(q.engine);
    const auto& me = q.first;
    ASSERT_TRUE(q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id},
                      {"content", "a\n1\n"},
                      {"format", "csv"},
                      {"name", "people"}})
                    .ok);
    // The engine still finds a date: the one the profile missed (Renewed).
    q.engine.fail_next["train_model"] = {
        "invalid_input",
        "transform_required: add_snapshot_date: Datetime features must be computed relative to a "
        "declared as_of or snapshot date; using the current time would change training data "
        "silently."};
    auto training = q.by(me,
                         "train_quickstart",
                         {{"workspace_id", me.workspace_id},
                          {"target", "Country"},
                          {"entity_column", "First Name"},
                          {"as_of", "2026-01-31"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_EQ(training.value["state"], "training") << training.value.dump(2);
    EXPECT_EQ(training.value["snapshot_date"], "2026-01-31") << "the date sent is kept";
    EXPECT_EQ(training.value["training"]["preparation"]["applied"],
              json::array({"add_snapshot_date"}));
    EXPECT_EQ(calls_of(q, "train_model").size(), 2U) << "tried once more, not again";
    const auto uploaded = copies(q);
    ASSERT_EQ(uploaded.size(), 2U);
    EXPECT_TRUE(uploaded[0][0].contains("Renewed")) << "the profile called it a category";
    EXPECT_FALSE(uploaded[1][0].contains("Renewed")) << "its values are dates: left out";
    EXPECT_EQ(calls_of(q, "analyze_dataset").back()["as_of"], "2026-01-31");

    // A second refusal of the same kind is explained, not tried a third time.
    Quickstart again;
    People::load(again.engine);
    ASSERT_TRUE(again
                    .by(again.first,
                        "start_quickstart",
                        {{"workspace_id", again.first.workspace_id},
                         {"content", "a\n1\n"},
                         {"format", "csv"},
                         {"name", "people"}})
                    .ok);
    again.engine.fail_next["train_model"] = {"invalid_input",
                                             "transform_required: add_snapshot_date: dates"};
    again.workspaces.zetta->responder = [&again, failures = 0](const std::string& capability,
                                                               const json& input) mutable {
        if (capability == "train_model" && failures++ < 2) {
            return std::optional<ZettaControlResult<json>>(std::unexpected(ZettaControlError{
                "invalid_input", "transform_required: add_snapshot_date: dates"}));
        }
        return again.engine.answer(capability, input);
    };
    again.engine.fail_next.clear();
    auto refused = again.by(again.first,
                            "train_quickstart",
                            {{"workspace_id", again.first.workspace_id}, {"target", "Country"}});
    ASSERT_TRUE(refused.ok) << refused.error.message;
    EXPECT_EQ(refused.value["state"], "failed");
    const json model = Quickstart::step(refused.value, "model");
    EXPECT_EQ(model["error"]["code"], "training_refused");
    const std::string message = model["error"]["message"];
    EXPECT_EQ(message.find("transform_required"), std::string::npos) << "never the raw code";
    EXPECT_NE(message.find("snapshot date"), std::string::npos) << message;
    EXPECT_NE(refused.value["next"].get<std::string>().find("train_quickstart"), std::string::npos);
    EXPECT_EQ(calls_of(again, "train_model").size(), 2U);
}

TEST(Quickstart, WhatCannotBeTrainedIsSaidInPlainWords) {
    Quickstart q;
    People::load(q.engine);
    const auto& me = q.first;
    ASSERT_TRUE(q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id},
                      {"content", "a\n1\n"},
                      {"format", "csv"},
                      {"name", "people"}})
                    .ok);
    // A column that cannot be predicted, chosen: refused before anything runs.
    auto spend =
        q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}, {"target", "Spend"}});
    EXPECT_EQ(spend.error.code, "invalid_input");
    EXPECT_EQ(spend.error.path, "/target");
    EXPECT_NE(spend.error.message.find("yes-or-no"), std::string::npos) << spend.error.message;
    auto bad_date =
        q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}, {"as_of", "yesterday"}});
    EXPECT_EQ(bad_date.error.path, "/as_of");
    auto absent =
        q.by(me,
             "train_quickstart",
             {{"workspace_id", me.workspace_id}, {"target", "Country"}, {"target_value", "Peru"}});
    ASSERT_TRUE(absent.ok) << absent.error.message;
    EXPECT_EQ(Quickstart::step(absent.value, "model")["error"]["code"], "cannot_prepare");
    EXPECT_NE(Quickstart::step(absent.value, "model")["error"]["message"].get<std::string>().find(
                  "No row has Country = Peru"),
              std::string::npos);
    EXPECT_TRUE(calls_of(q, "train_model").empty());

    // A refusal Lake cannot fix: explained, with what to choose.
    q.engine.fail_next["train_model"] = {
        "invalid_input",
        "transform_required: create_eligible_features: The plan has no feature columns."};
    auto eligible =
        q.by(me,
             "train_quickstart",
             {{"workspace_id", me.workspace_id}, {"target", "Country"}, {"target_value", "Japan"}});
    ASSERT_TRUE(eligible.ok) << eligible.error.message;
    const json model = Quickstart::step(eligible.value, "model");
    EXPECT_EQ(model["state"], "failed");
    EXPECT_EQ(model["error"]["transform"], "create_eligible_features");
    const std::string message = model["error"]["message"];
    EXPECT_EQ(message.find("transform_required"), std::string::npos);
    EXPECT_NE(message.find("Choose another column to predict"), std::string::npos) << message;
    EXPECT_EQ(calls_of(q, "train_model").size(), 1U) << "not tried again";
    // The next call with another choice trains.
    auto plan =
        q.by(me,
             "train_quickstart",
             {{"workspace_id", me.workspace_id}, {"target", "Country"}, {"target_value", "Japan"}});
    EXPECT_EQ(plan.value["state"], "training") << plan.value.dump(2);
    EXPECT_EQ(plan.value["training"]["target_value"], "Japan");
    EXPECT_EQ(plan.value["training"]["preparation"]["entity"], "customer_id")
        << "the detected identifier names a row";
}

TEST(Quickstart, ABlockedBalanceGetsTheTopUpLinkAndNothingStarts) {
    Quickstart q;
    q.gated = true;
    const auto& me = q.first;
    // Before the first top-up the account is blocked.
    auto refused =
        q.by(me,
             "start_quickstart",
             {{"workspace_id", me.workspace_id}, {"content", movies}, {"format", "csv"}});
    ASSERT_EQ(refused.error.code, "payment_required") << refused.error.message;
    EXPECT_EQ(refused.error.details["add_funds_url"],
              "https://lake.test/billing/add-funds?customer_id=" + me.customer_id);
    EXPECT_TRUE(q.engine_calls().empty()) << "no step started";
    EXPECT_TRUE(q.workspaces.records(me.workspace_id).rows("quickstarts").empty());
    auto train = q.by(me, "train_quickstart", {{"workspace_id", me.workspace_id}});
    EXPECT_EQ(train.error.code, "payment_required");
    // Reading stays open, and says what the paid steps cost and where the balance stands.
    auto read =
        q.by(me, "quickstart_status", {{"workspace_id", me.workspace_id}, {"include_costs", true}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["state"], "not_started");
    const json costs = read.value["costs"];
    EXPECT_EQ(costs["billing"], true);
    EXPECT_EQ(costs["balance"]["state"], "blocked");
    EXPECT_EQ(costs["balance"]["add_funds_url"],
              "https://lake.test/billing/add-funds?customer_id=" + me.customer_id);
    json model_cost;
    for (const json& entry : costs["steps"]) {
        if (entry["step"] == "model") {
            model_cost = entry;
        }
    }
    ASSERT_TRUE(model_cost.is_object());
    EXPECT_EQ(model_cost["capabilities"], json::array({"train_model", "expose_model"}));
    bool cpu_rated = false;
    for (const json& dimension : model_cost["billed_by"]) {
        if (dimension["dimension"] == "cpu") {
            cpu_rated = dimension["rate"] == json({{"unit_micros", 2'000}, {"per", 60'000}});
        }
    }
    EXPECT_TRUE(cpu_rated) << model_cost.dump();
    EXPECT_FALSE(read.warnings.is_null()) << "the answer carries the balance warning";

    // After the first top-up the same call goes through.
    lake::control::BalanceCredit credit;
    credit.customer_id = me.customer_id;
    credit.kind = "top_up";
    credit.amount_micros = 20 * dollar;
    credit.provider = "stripe";
    credit.reference = "cs_quickstart";
    ASSERT_TRUE(q.store->apply_balance_credit(credit));
    auto allowed =
        q.by(me,
             "start_quickstart",
             {{"workspace_id", me.workspace_id}, {"content", movies}, {"format", "csv"}});
    ASSERT_TRUE(allowed.ok) << allowed.error.message;
    EXPECT_EQ(allowed.value["state"], "search_ready");
}

TEST(Quickstart, AnotherAccountsWorkspaceIsRefused) {
    Quickstart q;
    const auto& me = q.first;
    const auto& them = q.second;
    ASSERT_TRUE(q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id}, {"content", movies}, {"format", "csv"}})
                    .ok);
    const std::size_t calls = q.engine_calls().size();
    for (const std::string capability :
         {"start_quickstart", "quickstart_status", "train_quickstart", "quickstart_snippets"}) {
        json input{{"customer_id", me.customer_id}, {"workspace_id", me.workspace_id}};
        if (capability == "start_quickstart") {
            input["content"] = movies;
            input["format"] = "csv";
        }
        // Their own credential naming my account: no grant.
        const auto refused = q.by(them, capability, input);
        EXPECT_FALSE(refused.ok) << capability;
        EXPECT_EQ(refused.error.code, "unauthorized")
            << capability << ": " << refused.error.message;
        // Their own account naming my workspace: not theirs.
        input["customer_id"] = them.customer_id;
        const auto foreign = q.by(them, capability, input);
        EXPECT_FALSE(foreign.ok) << capability;
        EXPECT_EQ(foreign.error.code, "invalid_input")
            << capability << ": " << foreign.error.message;
        EXPECT_NE(foreign.error.message.find("workspace"), std::string::npos) << capability;
    }
    EXPECT_EQ(q.engine_calls().size(), calls) << "nothing reached my workspace";
}

TEST(Quickstart, WithoutAWorkspaceOneIsMadeAndTheQuickstartWaitsForIt) {
    Quickstart q;
    const auto newcomer = q.account("newcomer", false);
    auto waiting = q.by(newcomer, "start_quickstart", {{"content", movies}, {"format", "csv"}});
    ASSERT_TRUE(waiting.ok) << waiting.error.message;
    EXPECT_EQ(waiting.value["state"], "waiting_for_workspace");
    EXPECT_TRUE(waiting.value["quickstart_id"].is_null()) << "nothing kept yet";
    EXPECT_EQ(Quickstart::step(waiting.value, "workspace")["state"], "running");
    const std::string workspace_id = waiting.value["workspace_id"];
    ASSERT_FALSE(workspace_id.empty());
    const auto made = q.store->list_workspaces(newcomer.customer_id);
    ASSERT_TRUE(made);
    ASSERT_EQ(made->size(), 1U);
    EXPECT_EQ(made->front().id, workspace_id);
    EXPECT_TRUE(q.engine_calls().empty());

    // The status says the same while it starts.
    auto read = q.by(newcomer, "quickstart_status", {{"workspace_id", workspace_id}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["state"], "waiting_for_workspace");

    // Once it is ready, the same call goes on; a second start makes no second workspace.
    q.workspaces.ready(workspace_id);
    auto started = q.by(newcomer, "start_quickstart", {{"content", movies}, {"format", "csv"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    EXPECT_EQ(started.value["workspace_id"], workspace_id);
    EXPECT_EQ(started.value["state"], "search_ready");
    EXPECT_EQ(q.store->list_workspaces(newcomer.customer_id)->size(), 1U);
}

TEST(Quickstart, TheInputSaysWhatIsMissing) {
    Quickstart q;
    const auto& me = q.first;
    auto none = q.by(me, "start_quickstart", {{"workspace_id", me.workspace_id}});
    EXPECT_EQ(none.error.code, "invalid_input");
    EXPECT_EQ(none.error.path, "/content");
    auto both = q.by(me,
                     "start_quickstart",
                     {{"workspace_id", me.workspace_id},
                      {"content", movies},
                      {"format", "csv"},
                      {"dataset_id", "datasets/existing"}});
    EXPECT_EQ(both.error.path, "/dataset_id");
    auto no_format =
        q.by(me, "start_quickstart", {{"workspace_id", me.workspace_id}, {"content", movies}});
    EXPECT_EQ(no_format.error.path, "/format");
    auto bad_name = q.by(me,
                         "start_quickstart",
                         {{"workspace_id", me.workspace_id},
                          {"content", movies},
                          {"format", "csv"},
                          {"name", "my movies"}});
    EXPECT_EQ(bad_name.error.path, "/name");
    auto malformed =
        q.by(me,
             "start_quickstart",
             {{"workspace_id", me.workspace_id}, {"content", "a,b\n1,2,3\n"}, {"format", "csv"}});
    ASSERT_TRUE(malformed.ok) << malformed.error.message;
    EXPECT_EQ(Quickstart::step(malformed.value, "dataset")["state"], "failed");
    EXPECT_NE(
        Quickstart::step(malformed.value, "dataset")["error"]["message"].get<std::string>().find(
            "malformed"),
        std::string::npos)
        << "upload_dataset's own CSV check";
    auto missing = q.by(me,
                        "start_quickstart",
                        {{"workspace_id", me.workspace_id}, {"dataset_id", "datasets/missing"}});
    ASSERT_TRUE(missing.ok) << missing.error.message;
    EXPECT_EQ(Quickstart::step(missing.value, "dataset")["state"], "failed");
    auto unknown =
        q.by(me,
             "quickstart_status",
             {{"workspace_id", me.workspace_id}, {"quickstart_id", lake::control::new_uuid_v4()}});
    EXPECT_EQ(unknown.error.code, "not_found");
    auto untrained =
        q.by(me,
             "train_quickstart",
             {{"workspace_id", q.second.workspace_id}, {"customer_id", q.second.customer_id}});
    EXPECT_FALSE(untrained.ok);
    auto no_snippets =
        q.by(q.second, "quickstart_snippets", {{"workspace_id", q.second.workspace_id}});
    EXPECT_EQ(no_snippets.error.code, "invalid_input");
}

TEST(Quickstart, AgentsFindItFirstInOpenApiMcpLlmsTxtAndTheSkill) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const std::vector<std::string> names{
        "start_quickstart", "quickstart_status", "train_quickstart", "quickstart_snippets"};
    const json openapi = lake::api::openapi_document(registry, "test");
    EXPECT_TRUE(openapi["paths"].contains("/v1/start_quickstart"));
    EXPECT_TRUE(openapi["paths"]["/v1/start_quickstart"].contains("post"));
    EXPECT_TRUE(openapi["paths"]["/v1/quickstart_status"].contains("get"));
    EXPECT_TRUE(openapi["paths"]["/v1/train_quickstart"].contains("post"));
    // POSTed: with `origins` it makes the browser key (lake#1152).
    EXPECT_TRUE(openapi["paths"]["/v1/quickstart_snippets"].contains("post"));
    for (const char* name : {"create_browser_key", "list_browser_keys", "revoke_browser_key"}) {
        EXPECT_TRUE(openapi["paths"].contains(std::string("/v1/") + name)) << name;
        EXPECT_EQ(lake::api::agent_docs::group_of(name), "Browser keys") << name;
    }
    const json tools = lake::api::mcp_tools(registry, lake::api::SchemaAudience::Caller);
    for (const std::string& name : names) {
        EXPECT_TRUE(std::ranges::any_of(tools, [&](const json& tool) {
            return tool["name"] == name;
        })) << name;
        EXPECT_EQ(lake::api::agent_docs::group_of(name), "Quickstart") << name;
    }
    EXPECT_TRUE(registry.find("start_quickstart")->starts_paid_work);
    EXPECT_TRUE(registry.find("train_quickstart")->starts_paid_work);
    EXPECT_FALSE(registry.find("quickstart_status")->starts_paid_work);
    EXPECT_EQ(registry.find("start_quickstart")->max_body_bytes,
              lake::api::maximum_capability_body_bytes)
        << "a dataset's text, as upload_dataset";

    const auto groups = lake::api::agent_docs::tool_groups(registry);
    ASSERT_FALSE(groups.empty());
    EXPECT_EQ(groups.front().title, "Quickstart");
    EXPECT_EQ(lake::api::agent_docs::flows().front().steps.front().tool, "start_quickstart");
    const std::string llms = lake::api::agent_docs::llms_txt(registry, "https://lake.test");
    const std::string full = lake::api::agent_docs::llms_full_txt(registry, "https://lake.test");
    const std::string skill = lake::api::agent_docs::skill_md(registry, "https://lake.test");
    for (const std::string* text : {&llms, &full, &skill}) {
        const auto start_here = text->find("## Start Here");
        ASSERT_NE(start_here, std::string::npos);
        EXPECT_LT(start_here, text->find("## Connect")) << "before anything else";
        EXPECT_NE(text->find("start_quickstart"), std::string::npos);
    }
    EXPECT_LT(llms.find("### Quickstart"), llms.find("### Workspace: Design, Train and Ship"));
    EXPECT_LT(llms.find("### Quickstart: search and model answers"),
              llms.find("### Build and run a pipeline"));
}

// The whole process against a real Zetta (a local `zetta --headless` from the zetta pin): the
// dataset, the index, a real search through search_index, training, publishing and predict.
TEST(QuickstartZetta, ADatasetBecomesSearchAndModelAnswersOnARealEngine) {
    const char* url = std::getenv("LAKE_TEST_ZETTA_URL");
    const char* token = std::getenv("LAKE_TEST_ZETTA_TOKEN");
    if (url == nullptr || token == nullptr || *url == '\0') {
        GTEST_SKIP() << "LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN are not set";
    }
    lake::control::install_zetta_control_transport();
    struct Shutdown {
        ~Shutdown() {
            lake::control::shutdown_zetta_control_transport();
        }
    } shutdown;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::filesystem::path dir =
        lake::test::fixture_path("quickstart-zetta-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(dir);
    auto service = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{dir.string(), 0},
        store,
        std::make_shared<lake::control::ZettaWorkspaceCaller>());
    const std::string identity = "taap:1:ed25519:quickstart-zetta";
    const auto user = store->create_user({identity, "zetta@example.test", "Zetta"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "qszetta"});
    ASSERT_TRUE(customer);
    const auto object = store->find_policy_object("customer", customer->id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({identity, object->id, "admin", "a", "admin", "r"}));
    const auto workspace = store->create_workspace({customer->id, "home", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto zetta_service = store->get_service(workspace->zetta_service_id);
    ASSERT_TRUE(zetta_service);
    ASSERT_TRUE(store->set_service_status(zetta_service->id, "Ready", "", url));
    std::ofstream(dir / (zetta_service->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", token}}.dump();

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = service.get();
        context.authorized = true;
        context.acting_identity = identity;
        context.acting_user_id = user->id;
        context.session_credential = true;
        context.scopes = {"*"};
        context.instance_kek = std::string(kek);
        context.external_origin = "https://lake.test";
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = customer->id;
        input["workspace_id"] = workspace->id;
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? std::move(*completed) : lake::api::ApiResult{};
    };

    // Each run gets its own table, so a Zetta reused across runs answers this run's data.
    std::string csv = "title,description,genre,rating,popular\n";
    const std::vector<std::vector<std::string>> films{
        {"The Matrix", "A hacker learns reality is a simulation", "scifi", "8.7", "1"},
        {"Inception", "A thief steals secrets through dreams", "scifi", "8.8", "1"},
        {"Amelie", "A shy waitress helps people in Paris", "romance", "8.3", "0"},
        {"Alien", "A crew meets a deadly creature in space", "horror", "8.5", "1"},
        {"Notting Hill", "A bookseller falls for a film star", "romance", "7.2", "0"},
        {"Heat", "A detective hunts a crew of thieves", "crime", "8.3", "1"},
        {"Clue", "Guests at a mansion solve a murder", "comedy", "7.2", "0"},
        {"Arrival", "A linguist talks with visitors from space", "scifi", "7.9", "1"},
        {"Big", "A boy wakes up as an adult", "comedy", "7.3", "0"},
        {"Ronin", "Mercenaries chase a mysterious case", "crime", "7.2", "0"}};
    for (int copy = 0; copy < 4; ++copy) {
        for (const auto& film : films) {
            csv += film[0] + " " + std::to_string(copy) + "," + film[1] + "," + film[2] + "," +
                   film[3] + "," + film[4] + "\n";
        }
    }
    const std::string table = "qs_" + lake::control::new_uuid_v4().substr(0, 8);
    auto started = call("start_quickstart", {{"content", csv}, {"format", "csv"}, {"name", table}});
    ASSERT_TRUE(started.ok) << started.error.code << ": " << started.error.message;
    ASSERT_EQ(started.value["state"], "search_ready") << started.value.dump(2);
    EXPECT_EQ(started.value["dataset"]["rows"], 40);
    ASSERT_EQ(started.value["dataset"]["columns"].size(), 5U);
    EXPECT_EQ(started.value["dataset"]["columns"][0]["name"], "title");
    EXPECT_EQ(started.value["dataset"]["columns"][4]["name"], "popular");
    const std::string index_id = started.value["index"]["id"];

    // The website's search, as the snippet makes it.
    auto searched =
        call("search_index", {{"index_id", index_id}, {"query", "space"}, {"top_k", 5}});
    ASSERT_TRUE(searched.ok) << searched.error.message;
    ASSERT_FALSE(searched.value["hits"].empty());
    EXPECT_NE(searched.value["hits"][0]["fields"]["description"].get<std::string>().find("space"),
              std::string::npos);
    auto snippets = call("quickstart_snippets", json::object());
    ASSERT_TRUE(snippets.ok) << snippets.error.message;
    EXPECT_NE(snippets.value["snippets"][0]["code"].get<std::string>().find(index_id),
              std::string::npos);

    // Train, wait for the run, publish.
    auto training = call("train_quickstart", {{"target", "popular"}, {"entity_column", "title"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_NE(training.value["state"], "failed") << training.value.dump(2);
    json status;
    for (int attempt = 0; attempt < 120; ++attempt) {
        status = call("quickstart_status", json::object()).value;
        if (status["state"] != "training") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_EQ(status["state"], "trained") << status.dump(2);
    auto published = call("train_quickstart", json::object());
    ASSERT_TRUE(published.ok) << published.error.message;
    ASSERT_EQ(published.value["state"], "done") << published.value.dump(2);
    const std::string model_id = published.value["model"]["id"];

    // The model's answer for each search result, as the snippet asks for it.
    json rows = json::array();
    for (const json& hit : searched.value["hits"]) {
        rows.push_back(hit["fields"]);
    }
    auto predicted = call("predict", {{"model_id", model_id}, {"rows", rows}});
    ASSERT_TRUE(predicted.ok) << predicted.error.message;
    ASSERT_EQ(predicted.value["ranked"].size(), rows.size());
    for (const json& entry : predicted.value["ranked"]) {
        EXPECT_TRUE(entry["risk"].is_number());
        EXPECT_TRUE(entry["row"].is_number_integer());
    }
    snippets = call("quickstart_snippets", json::object());
    EXPECT_EQ(snippets.value["includes_model"], true);
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

// lake#1181 on a real Zetta: Steve's shape (a first name, a country, a date, with an id, free text,
// numbers and a two-valued plan). v0.7.0 refused it with transform_required: add_snapshot_date.
TEST(QuickstartZetta, ADatedDatasetWithAnyColumnTypesTrainsAndAnswersOnARealEngine) {
    const char* url = std::getenv("LAKE_TEST_ZETTA_URL");
    const char* token = std::getenv("LAKE_TEST_ZETTA_TOKEN");
    if (url == nullptr || token == nullptr || *url == '\0') {
        GTEST_SKIP() << "LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN are not set";
    }
    lake::control::install_zetta_control_transport();
    struct Shutdown {
        ~Shutdown() {
            lake::control::shutdown_zetta_control_transport();
        }
    } shutdown;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::filesystem::path dir =
        lake::test::fixture_path("quickstart-dated-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(dir);
    auto service = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{dir.string(), 0},
        store,
        std::make_shared<lake::control::ZettaWorkspaceCaller>());
    const std::string identity = "taap:1:ed25519:quickstart-dated";
    const auto user = store->create_user({identity, "dated@example.test", "Dated"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "qsdated"});
    ASSERT_TRUE(customer);
    const auto object = store->find_policy_object("customer", customer->id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({identity, object->id, "admin", "a", "admin", "r"}));
    const auto workspace = store->create_workspace({customer->id, "home", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto zetta_service = store->get_service(workspace->zetta_service_id);
    ASSERT_TRUE(zetta_service);
    ASSERT_TRUE(store->set_service_status(zetta_service->id, "Ready", "", url));
    std::ofstream(dir / (zetta_service->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", token}}.dump();

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = service.get();
        context.authorized = true;
        context.acting_identity = identity;
        context.acting_user_id = user->id;
        context.session_credential = true;
        context.scopes = {"*"};
        context.instance_kek = std::string(kek);
        context.external_origin = "https://lake.test";
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = customer->id;
        input["workspace_id"] = workspace->id;
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? std::move(*completed) : lake::api::ApiResult{};
    };

    const std::vector<std::string> names{"Ana", "Ben", "Chen", "Dana", "Eli", "Fay"};
    const std::vector<std::string> countries{"Canada", "France", "Japan", "Brazil"};
    std::string csv = "customer_id,First Name,Country,Signup Date,Age,Plan,Spend,Bio\n";
    for (int i = 0; i < 60; ++i) {
        const std::string& country = countries[static_cast<std::size_t>(i % 4)];
        const std::string month = (i % 12 < 9 ? "0" : "") + std::to_string(i % 12 + 1);
        const std::string day = (i % 27 < 9 ? "0" : "") + std::to_string(i % 27 + 1);
        csv += "cust-" + std::to_string(1000 + i) + "," + names[static_cast<std::size_t>(i % 6)] +
               "," + country + ",2025-" + month + "-" + day + "," + std::to_string(20 + i % 40) +
               "," + (i % 3 == 0 ? "pro" : "free") + "," +
               std::to_string(10 + i + (country == "Canada" ? 40 : 0)) +
               ",Likes hiking and long walks number " + std::to_string(i) + " by the lake\n";
    }
    const std::string table = "qsd_" + lake::control::new_uuid_v4().substr(0, 8);
    auto started = call("start_quickstart", {{"content", csv}, {"format", "csv"}, {"name", table}});
    ASSERT_TRUE(started.ok) << started.error.code << ": " << started.error.message;
    ASSERT_EQ(started.value["state"], "search_ready") << started.value.dump(2);
    std::map<std::string, std::string> kinds;
    for (const json& column : started.value["dataset"]["columns"]) {
        kinds[column["name"]] = column.value("kind", std::string());
    }
    EXPECT_EQ(kinds["Signup Date"], "datetime") << started.value["dataset"].dump(2);
    EXPECT_EQ(kinds["Bio"], "free_text");
    EXPECT_EQ(kinds["Age"], "numeric");
    EXPECT_EQ(kinds["Plan"], "boolean");
    EXPECT_EQ(kinds["Country"], "categorical");
    EXPECT_TRUE(started.value["snapshot_date"].is_string());
    const std::string index_id = started.value["index"]["id"];

    // Steve's choice.
    auto training =
        call("train_quickstart", {{"target", "Country"}, {"entity_column", "First Name"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_EQ(training.value["state"], "training") << training.value.dump(2);
    EXPECT_EQ(training.value["training"]["preparation"]["copy"], true);
    json status;
    for (int attempt = 0; attempt < 120; ++attempt) {
        status = call("quickstart_status", json::object()).value;
        if (status["state"] != "training") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_EQ(status["state"], "trained") << status.dump(2);
    auto published = call("train_quickstart", json::object());
    ASSERT_EQ(published.value["state"], "done") << published.value.dump(2);
    const std::string model_id = published.value["model"]["id"];

    // The website's rows, as search answers them (the date and every other column in them).
    auto searched =
        call("search_index", {{"index_id", index_id}, {"query", "hiking"}, {"top_k", 5}});
    ASSERT_TRUE(searched.ok) << searched.error.message;
    ASSERT_FALSE(searched.value["hits"].empty());
    json rows = json::array();
    for (const json& hit : searched.value["hits"]) {
        rows.push_back(hit["fields"]);
    }
    ASSERT_TRUE(rows[0].contains("Signup Date"));
    auto predicted = call("predict", {{"model_id", model_id}, {"rows", rows}});
    ASSERT_TRUE(predicted.ok) << predicted.error.message;
    ASSERT_EQ(predicted.value["ranked"].size(), rows.size());
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

// lake#1203 on a real Zetta: the Quickstart's clustering (M-3), as Ask Orama proposes it, trains
// end to end. On lake-dev the engine refused it ("segmentation has no supervised holdout or ranked
// top_k; configure its K range in the graph hints") because Lake sent a held-out share. The rows
// are named by a text id, as Steve's customers file is.
TEST(QuickstartZetta, AClusteringModelTrainsOnARealEngine) {
    const char* url = std::getenv("LAKE_TEST_ZETTA_URL");
    const char* token = std::getenv("LAKE_TEST_ZETTA_TOKEN");
    if (url == nullptr || token == nullptr || *url == '\0') {
        GTEST_SKIP() << "LAKE_TEST_ZETTA_URL and LAKE_TEST_ZETTA_TOKEN are not set";
    }
    lake::control::install_zetta_control_transport();
    struct Shutdown {
        ~Shutdown() {
            lake::control::shutdown_zetta_control_transport();
        }
    } shutdown;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::filesystem::path dir =
        lake::test::fixture_path("quickstart-clustering-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(dir);
    auto service = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{dir.string(), 0},
        store,
        std::make_shared<lake::control::ZettaWorkspaceCaller>());
    const std::string identity = "taap:1:ed25519:quickstart-clustering";
    const auto user = store->create_user({identity, "clustering@example.test", "Clustering"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "qsclustering"});
    ASSERT_TRUE(customer);
    const auto object = store->find_policy_object("customer", customer->id);
    ASSERT_TRUE(object);
    ASSERT_TRUE(store->create_grant({identity, object->id, "admin", "a", "admin", "r"}));
    const auto workspace = store->create_workspace({customer->id, "home", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    const auto zetta_service = store->get_service(workspace->zetta_service_id);
    ASSERT_TRUE(zetta_service);
    ASSERT_TRUE(store->set_service_status(zetta_service->id, "Ready", "", url));
    std::ofstream(dir / (zetta_service->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", token}}.dump();

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto call = [&](const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = service.get();
        context.authorized = true;
        context.acting_identity = identity;
        context.acting_user_id = user->id;
        context.session_credential = true;
        context.scopes = {"*"};
        context.instance_kek = std::string(kek);
        context.external_origin = "https://lake.test";
        context.request_id = lake::control::new_uuid_v4();
        input["customer_id"] = customer->id;
        input["workspace_id"] = workspace->id;
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? std::move(*completed) : lake::api::ApiResult{};
    };

    // Customers in three clear groups: a text id names each row, as in Steve's file.
    const std::vector<std::string> countries{"Canada", "France", "Japan"};
    std::string csv = "Customer Id,Country,Age,Orders,Spend\n";
    for (int i = 0; i < 90; ++i) {
        const int group = i % 3;
        csv += "C" + std::to_string(48'000 + i * 7) + "x," +
               countries[static_cast<std::size_t>(group)] + "," +
               std::to_string(22 + group * 20 + i % 5) + "," + std::to_string(1 + group * 10 + i % 3) +
               "," + std::to_string(40 + group * 400 + i % 11) + "\n";
    }
    const std::string table = "qsc_" + lake::control::new_uuid_v4().substr(0, 8);
    auto started = call("start_quickstart", {{"content", csv}, {"format", "csv"}, {"name", table}});
    ASSERT_TRUE(started.ok) << started.error.code << ": " << started.error.message;
    ASSERT_EQ(started.value["state"], "search_ready") << started.value.dump(2);

    // The proposal Ask Orama makes: methodology M-3, nothing else.
    auto training = call("train_quickstart", {{"methodology", "M-3"}});
    ASSERT_TRUE(training.ok) << training.error.message;
    ASSERT_EQ(training.value["state"], "training") << training.value.dump(2);
    EXPECT_EQ(training.value["training"]["methodology"], "M-3");
    EXPECT_EQ(training.value["training"]["holdout_fraction"], nullptr);
    json status;
    for (int attempt = 0; attempt < 240; ++attempt) {
        status = call("quickstart_status", json::object()).value;
        if (status["state"] != "training") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_EQ(status["state"], "trained") << status.dump(2);
    auto finished = call("train_quickstart", json::object());
    ASSERT_TRUE(finished.ok) << finished.error.message;
    ASSERT_EQ(finished.value["state"], "done") << finished.value.dump(2);
    EXPECT_EQ(finished.value["model"]["serves"], false);
    const std::string model_id = finished.value["model"]["id"];
    ASSERT_FALSE(model_id.empty());
    EXPECT_EQ(Quickstart::step(finished.value, "model_answers")["state"], "skipped");
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

} // namespace
