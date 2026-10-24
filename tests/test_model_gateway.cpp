// The model gateway (lake#248, docs/api/model-gateway.md): every model call an agent makes for a
// workspace goes through lake's `model_completion`, which counts the tokens the provider reports as
// the workspace's `model_tokens`, refuses calls past the account's monthly limit, and keeps the
// prompt out of the audit. A fake provider stands in for OpenAI: tests never call it.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/billing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/model_gateway.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "one_thread_io.hpp"
#include "postgres_fixture.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::ApiResult;
using lake::api::ModelProviderRequest;
using lake::api::ModelProviderResponse;
using nlohmann::json;

constexpr std::string_view kek = "6d6f64656c2d676174657761792d746573742d696e7374616e63652d6b656b31";

const json chat_answer{
    {"id", "chatcmpl-1"},
    {"object", "chat.completion"},
    {"model", "gpt-5.5-2026-08-01"},
    {"choices",
     json::array({{{"index", 0},
                   {"message", {{"role", "assistant"}, {"content", "Churn is 4.2% this month."}}},
                   {"finish_reason", "stop"}}})},
    {"usage", {{"prompt_tokens", 1200}, {"completion_tokens", 300}, {"total_tokens", 1500}}}};

const std::string chat_stream =
    "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[{\"index\":0,\"delta\":"
    "{\"content\":\"Hel\"}}],\"usage\":null}\n\n"
    "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[{\"index\":0,\"delta\":"
    "{\"content\":\"lo\"}}],\"usage\":null}\n\n"
    "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[],\"usage\":"
    "{\"prompt_tokens\":40,\"completion_tokens\":2,\"total_tokens\":42}}\n\n"
    "data: [DONE]\n\n";

// A provider that answers what it is told and remembers every request. A request that takes
// its body as it arrives gets a 2xx answer in small pieces that cut across the events' lines.
struct FakeProvider {
    std::mutex mutex;
    std::vector<ModelProviderRequest> requests;
    ModelProviderResponse answer{200, chat_answer.dump(), {}};
    std::size_t piece_size = 13;

    lake::api::ModelProvider provider() {
        return [this](const ModelProviderRequest& request) {
            ModelProviderResponse out;
            {
                std::lock_guard lock(mutex);
                requests.push_back(request);
                out = answer;
            }
            if (request.on_body && out.status >= 200 && out.status < 300) {
                const std::string body = std::exchange(out.body, std::string());
                for (std::size_t at = 0; at < body.size(); at += piece_size) {
                    if (!request.on_body(std::string_view(body).substr(at, piece_size))) {
                        out.error = "the response body's reader stopped";
                        break;
                    }
                }
            }
            return out;
        };
    }
    std::size_t calls() {
        std::lock_guard lock(mutex);
        return requests.size();
    }
};

std::string space(lake::control::Store& store, const std::string& handle) {
    const auto user = store.create_user(
        {"taap:1:ed25519:gateway-" + handle, handle + "@example.test", handle});
    EXPECT_TRUE(user) << user.error().message;
    const auto customer = store.create_customer({"individual", user->id, "", handle});
    EXPECT_TRUE(customer) << customer.error().message;
    return customer->id;
}

struct GatewayRig {
    std::shared_ptr<lake::control::Store> store = std::make_shared<lake::control::InMemoryStore>();
    std::string database_uri;
    lake::test::RecordWorkspaces workspaces{store, database_uri};
    std::string customer = space(*store, "gateway-" + lake::control::new_uuid_v4().substr(0, 8));
    std::string home = workspaces.create(customer, "home");
    std::string lab = workspaces.create(customer, "lab");
    lake::api::UsageRecorder recorder{store.get(), workspaces.service.get()};
    lake::api::Registry registry;
    FakeProvider fake;

    GatewayRig() {
        lake::api::register_core_capabilities(registry, "test");
    }
    // Over a PostgreSQL control store at `uri`, with the rows the schema seeds.
    GatewayRig(std::shared_ptr<lake::control::Store> backing, std::string uri)
        : store(std::move(backing)), database_uri(std::move(uri)) {
        lake::api::register_core_capabilities(registry, "test");
    }

    lake::api::ApiContext admin() {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.usage_recorder = &recorder;
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        context.model_provider = fake.provider();
        return context;
    }

    // What a task credential resolves to (registry.cpp acting_agent): the task's own context.
    lake::api::ApiContext task_context(const lake::control::AgentTask& task) {
        auto context = admin();
        context.acting_identity = "taap:task-person";
        context.operator_credential = false;
        context.session_credential = true;
        context.credential_kind = "agent_task";
        context.credential_session_id = "task:" + task.id;
        context.credential_token_hash = task.credential_sha256;
        context.scopes = task.scopes;
        return context;
    }

    lake::control::AgentTask task(const std::string& workspace_id, const std::string& agent) {
        const auto owner = store->get_customer(customer);
        EXPECT_TRUE(owner);
        lake::control::AgentTask row;
        row.id = lake::control::new_uuid_v4();
        row.customer_id = customer;
        row.agent = agent;
        row.kind = "turn";
        row.workspace_id = workspace_id;
        row.scopes = {"workspace_query"};
        row.requested_by_user_id = owner->user_id;
        const auto created = store->create_agent_task(row);
        EXPECT_TRUE(created) << created.error().message;
        // Claimed, as the agent that holds its credential has: each call renews the lease.
        const auto claimed = store->claim_agent_task(
            row.id, agent, lake::control::identity_crypto::sha256_hex("lat." + row.id + ".secret"));
        EXPECT_TRUE(claimed) << claimed.error().message;
        return *claimed;
    }

    ApiResult call(json input, lake::api::ApiContext& context) {
        auto completed =
            liquid::runtime::block_on(registry.call_async("model_completion", input, context));
        EXPECT_TRUE(completed);
        return completed ? std::move(*completed)
                         : ApiResult::failure("store_error", completed.error().message);
    }
};

json chat(bool stream = false) {
    json request{{"model", "gpt-5.5"},
                 {"messages",
                  json::array({{{"role", "user"}, {"content", "What is churn this month?"}}})}};
    if (stream) {
        request["stream"] = true;
    }
    return {{"endpoint", "chat/completions"}, {"request", request}};
}

TEST(ModelGateway, ParsesAnExactModelAllowlistAndRejectsBadConfiguration) {
    using lake::api::gateway_model_allowed;
    using lake::api::parse_gateway_models;
    ASSERT_TRUE(parse_gateway_models(""));
    EXPECT_TRUE(gateway_model_allowed(*parse_gateway_models(""), "gpt-5.5"));
    EXPECT_FALSE(gateway_model_allowed(*parse_gateway_models("[]"), "gpt-5.5"));
    const auto custom = parse_gateway_models(R"([{"model":"private-model","provider":"openai"}])");
    ASSERT_TRUE(custom);
    EXPECT_TRUE(gateway_model_allowed(*custom, "private-model"));
    EXPECT_FALSE(gateway_model_allowed(*custom, "gpt-5.5"));
    for (const auto text :
         {"null",
          "{}",
          "broken",
          R"([{"model":"x"}])",
          R"([{"model":"x","provider":"other"}])",
          R"([{"model":" x","provider":"openai"}])",
          R"([{"model":"","provider":"openai"}])",
          R"([{"model":"x","provider":"openai","extra":1}])",
          R"([{"model":"x","provider":"openai"},{"model":"x","provider":"openai"}])"}) {
        EXPECT_FALSE(parse_gateway_models(text)) << text;
    }
}

TEST(ModelCompletion, RejectsUnlistedModelsBeforeEveryEndpointCallsTheProvider) {
    GatewayRig rig;
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    for (const auto endpoint : lake::api::model_gateway_endpoints) {
        for (const std::string model : {"premium-model",
                                        "GPT-5.5",
                                        "gpt-5.5 ",
                                        "gpt-5.5-2026-08-01",
                                        "gpt-5.5%00",
                                        "ft:gpt-5.5:premium"}) {
            json request{{"model", model}};
            if (endpoint == "embeddings") {
                request["input"] = "hello";
            } else if (endpoint == "responses") {
                request["input"] = "hello";
            } else {
                request["messages"] = chat()["request"]["messages"];
            }
            const auto refused = rig.call({{"endpoint", endpoint}, {"request", request}}, context);
            EXPECT_FALSE(refused.ok) << model;
            EXPECT_EQ(refused.error.code, "invalid_input") << model;
            EXPECT_EQ(refused.error.path, "/request/model") << model;
        }
    }
    EXPECT_EQ(rig.fake.calls(), 0U);
    context.model_gateway_models = {{"private-model", "openai"}};
    auto custom = chat();
    custom["request"]["model"] = "private-model";
    EXPECT_TRUE(rig.call(custom, context).ok);
    context.model_gateway_models.clear();
    EXPECT_FALSE(rig.call(custom, context).ok);
    EXPECT_EQ(rig.fake.calls(), 1U);
}

TEST(ModelCompletion, BillingRequiresBothPositiveExactModelPricesBeforeDispatch) {
    GatewayRig rig;
    lake::api::register_billing_capabilities(rig.registry);
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    const auto reject = [&] {
        const auto refused = rig.call(chat(), context);
        EXPECT_FALSE(refused.ok);
        EXPECT_EQ(refused.error.code, "invalid_input");
        EXPECT_EQ(refused.error.path, "/request/model");
        EXPECT_EQ(rig.fake.calls(), 0U);
    };
    reject(); // A missing list never grants models.
    lake::control::BillingPlan plan;
    plan.id = "usage-200001-00000000000000001";
    plan.name = "Usage";
    plan.currency = "USD";
    plan.id = "usage-209901-00000000000000001";
    plan.prices = {{"model_tokens", 6'000'000, 1'000'000, 0, "gpt-5.5/input"},
                   {"model_tokens", 36'000'000, 1'000'000, 0, "gpt-5.5/output"}};
    ASSERT_TRUE(rig.store->put_billing_plan(plan));
    reject(); // A future price list is not permission to spend now.
    plan.id = "usage-200001-00000000000000001";
    for (const auto& prices : std::vector<std::vector<lake::control::BillingPrice>>{
             {{"model_tokens", 9'000'000, 1'000'000, 0}},
             {{"model_tokens", 6'000'000, 1'000'000, 0, "gpt-5.5/input"}},
             {{"model_tokens", 6'000'000, 1'000'000, 0, "gpt-5.5/input"},
              {"model_tokens", 0, 1'000'000, 0, "gpt-5.5/output"}},
             {{"model_tokens", 6'000'000, 1'000'000, 0, "other/input"},
              {"model_tokens", 36'000'000, 1'000'000, 0, "other/output"}}}) {
        plan.prices = prices;
        ASSERT_TRUE(rig.store->put_billing_plan(plan));
        reject();
    }
    auto unlisted = chat();
    unlisted["request"]["model"] = "other";
    const auto priced_but_unlisted = rig.call(unlisted, context);
    EXPECT_FALSE(priced_but_unlisted.ok);
    EXPECT_EQ(priced_but_unlisted.error.code, "invalid_input");
    EXPECT_EQ(rig.fake.calls(), 0U);
    plan.prices = {{"model_tokens", 6'000'000, 1'000'000, 0, "gpt-5.5/input"},
                   {"model_tokens", 36'000'000, 1'000'000, 0, "gpt-5.5/output"}};
    ASSERT_TRUE(rig.store->put_billing_plan(plan));
    ASSERT_TRUE(rig.call(chat(), context).ok);
    EXPECT_EQ(rig.fake.calls(), 1U);
    EXPECT_EQ(context.audit_details["answered_model"], "gpt-5.5-2026-08-01");
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    auto& records = rig.workspaces.records(rig.home);
    EXPECT_EQ(records.metric_total("lake_usage", {{"class", "gpt-5.5/input"}}), 1200);
    EXPECT_EQ(records.metric_total("lake_usage", {{"class", "gpt-5.5/output"}}), 300);
}

// The default gateway models' token prices the schema seeds (migration 0095, Steve 2026-10-23):
// OpenAI's published standard price, https://developers.openai.com/api/docs/pricing read
// 2026-10-23, plus 30%, in millionths of a dollar per million tokens (docs/api/billing.md#rates).
struct SeededModelPrice {
    const char* model;
    std::uint64_t input;
    std::uint64_t output;
};
constexpr SeededModelPrice seeded_model_prices[] = {
    {"gpt-5.5", 6'500'000, 39'000'000},          // $5.00 / $30.00
    {"gpt-5", 1'625'000, 13'000'000},            // $1.25 / $10.00
    {"gpt-4o", 3'250'000, 13'000'000},           // $2.50 / $10.00
    {"text-embedding-3-small", 26'000, 26'000}}; // $0.02, no output tokens

// A fresh control plane (the schema applied to an empty PostgreSQL database, as the operator
// applies it) can make a model call with billing on, for every default model, and the bill prices
// the call's input and output at the seeded rates.
TEST(ModelCompletion, ASeededInstanceCallsTheDefaultModelsWithBillingOnAtTheSeededPrices) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    std::shared_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, std::string(kek));
    ASSERT_TRUE(store->ping());
    GatewayRig rig(store, uri);
    lake::api::register_billing_capabilities(rig.registry);
    auto admin = rig.admin();

    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const std::string period =
        std::format("{:04}-{:02}", int(today.year()), unsigned(today.month()));
    const auto listed = rig.registry.call("price_list", {{"period", period}}, admin);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_TRUE(listed.value["list"].is_object()) << listed.value.dump();
    const auto rate = [&](const std::string& name) -> json {
        for (const json& price : listed.value["list"]["prices"]) {
            if (price["dimension"] == "model_tokens" && price["class"] == name) {
                return price;
            }
        }
        return nullptr;
    };
    for (const auto& seeded : seeded_model_prices) {
        SCOPED_TRACE(seeded.model);
        EXPECT_TRUE(
            lake::api::gateway_model_allowed(rig.admin().model_gateway_models, seeded.model));
        const json input = rate(std::string(seeded.model) + "/input");
        const json output = rate(std::string(seeded.model) + "/output");
        ASSERT_TRUE(input.is_object());
        ASSERT_TRUE(output.is_object());
        EXPECT_EQ(input["unit_micros"], seeded.input);
        EXPECT_EQ(output["unit_micros"], seeded.output);
        EXPECT_EQ(input["per"], 1'000'000);
        EXPECT_EQ(output["per"], 1'000'000);
        auto context = rig.admin();
        EXPECT_TRUE(lake::api::validate_gateway_model(seeded.model, context));
    }
    EXPECT_EQ(std::size(seeded_model_prices), lake::api::default_gateway_models().size());

    // A gpt-5.5 call answered with 1,200 input and 300 output tokens (chat_answer).
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    const auto answered = rig.call(chat(), context);
    ASSERT_TRUE(answered.ok) << answered.error.code << ": " << answered.error.message;
    EXPECT_EQ(rig.fake.calls(), 1U);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));

    auto billed = liquid::runtime::block_on(rig.registry.call_async(
        "bill_preview", {{"customer_id", rig.customer}, {"period", period}}, admin));
    ASSERT_TRUE(billed);
    const ApiResult preview = std::move(*billed);
    ASSERT_TRUE(preview.ok) << preview.error.code << ": " << preview.error.message;
    const auto line = [&](const std::string& name) -> json {
        for (const json& entry : preview.value["lines"]) {
            if (entry["dimension"] == "model_tokens" && entry["class"] == name) {
                return entry;
            }
        }
        return nullptr;
    };
    const json input = line("gpt-5.5/input");
    const json output = line("gpt-5.5/output");
    ASSERT_TRUE(input.is_object()) << preview.value.dump();
    ASSERT_TRUE(output.is_object()) << preview.value.dump();
    EXPECT_EQ(input["quantity"], 1200);
    EXPECT_EQ(input["unit_micros"], 6'500'000);
    EXPECT_EQ(input["per"], 1'000'000);
    EXPECT_EQ(input["amount_micros"], 7'800) << "1,200 tokens at $6.50 a million";
    EXPECT_EQ(output["quantity"], 300);
    EXPECT_EQ(output["unit_micros"], 39'000'000);
    EXPECT_EQ(output["amount_micros"], 11'700) << "300 tokens at $39.00 a million";
}

TEST(RecordModelUsage, UnpricedReportsCannotPoisonABilledAccountsLedger) {
    GatewayRig rig;
    lake::api::register_billing_capabilities(rig.registry);
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    const json tokens{{"model_name", "outside-model"}, {"input_tokens", 7}, {"output_tokens", 3}};
    const auto refused = rig.registry.call("record_model_usage", tokens, context);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/model_name");
    EXPECT_EQ(context.audit_workspace_id, rig.home);
    EXPECT_EQ(context.audit_object, "agent_usage/" + task.id);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"dimension", "model_tokens"}}),
              0);

    lake::control::BillingPlan plan;
    plan.id = "usage-200001-00000000000000001";
    plan.name = "Usage";
    plan.currency = "USD";
    plan.prices = {{"model_tokens", 6, 1, 0, "outside-model/input"},
                   {"model_tokens", 36, 1, 0, "outside-model/output"}};
    ASSERT_TRUE(rig.store->put_billing_plan(plan));
    const auto recorded = rig.registry.call("record_model_usage", tokens, context);
    ASSERT_TRUE(recorded.ok) << recorded.error.message;
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"class", "outside-model/input"}}),
              7);
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"class", "outside-model/output"}}),
              3);
    // Reporting outside usage grants no access to the platform provider.
    auto gateway = chat();
    gateway["request"]["model"] = "outside-model";
    EXPECT_FALSE(rig.call(gateway, context).ok);
    EXPECT_EQ(rig.fake.calls(), 0U);

    GatewayRig enterprise;
    const auto enterprise_task = enterprise.task(enterprise.home, "agent-project");
    auto enterprise_context = enterprise.task_context(enterprise_task);
    const auto unbilled =
        enterprise.registry.call("record_model_usage", tokens, enterprise_context);
    EXPECT_TRUE(unbilled.ok) << unbilled.error.message;
}

TEST(ModelGateway, PreparesEachRequestAndAsksAStreamForItsUsage) {
    using lake::api::prepare_model_request;
    EXPECT_EQ(prepare_model_request("images/generations", json::object()).error().path,
              "/endpoint");
    EXPECT_EQ(prepare_model_request("chat/completions", json::array()).error().path, "/request");
    EXPECT_EQ(prepare_model_request("chat/completions", {{"messages", json::array()}}).error().path,
              "/request/model");
    EXPECT_EQ(
        prepare_model_request("embeddings", {{"model", "text-embedding-3-small"}, {"stream", true}})
            .error()
            .path,
        "/request/stream");

    const auto plain = prepare_model_request("chat/completions", {{"model", "gpt-5.5"}});
    ASSERT_TRUE(plain);
    EXPECT_FALSE(plain->contains("stream_options"))
        << "a non-streaming request needs no stream options";

    const auto streamed = prepare_model_request(
        "chat/completions",
        {{"model", "gpt-5.5"}, {"stream", true}, {"stream_options", {{"include_usage", false}}}});
    ASSERT_TRUE(streamed);
    EXPECT_EQ((*streamed)["stream_options"]["include_usage"], true);
    EXPECT_EQ((*streamed)["store"], false);
    EXPECT_FALSE(prepare_model_request(
        "chat/completions",
        {{"model", "gpt-5.5"}, {"stream", true}, {"stream_options", {{"other", 1}}}}));

    // The Responses API reports usage in its last event on its own.
    const auto responses =
        prepare_model_request("responses", {{"model", "gpt-5.5"}, {"stream", true}});
    ASSERT_TRUE(responses);
    EXPECT_FALSE(responses->contains("stream_options"));
}

// lake#939: a background answer goes on being generated after the provider has answered, with no
// usage in that answer, so the gateway refuses it.
TEST(ModelGateway, RefusesABackgroundAnswer) {
    using lake::api::prepare_model_request;
    for (const json& background : {json(true), json("yes"), json(1)}) {
        const auto refused =
            prepare_model_request("responses", {{"model", "gpt-5.5"}, {"background", background}});
        ASSERT_FALSE(refused) << background.dump();
        EXPECT_EQ(refused.error().path, "/request/background");
    }
    EXPECT_TRUE(prepare_model_request("responses", {{"model", "gpt-5.5"}, {"background", false}}));
    EXPECT_TRUE(
        prepare_model_request("responses", {{"model", "gpt-5.5"}, {"background", nullptr}}));
}

// Each rejected request would spend the platform key on work outside token accounting.
TEST(ModelCompletion, RefusesUnmeteredFeaturesBeforeCallingTheProvider) {
    GatewayRig rig;
    const auto task = rig.task(rig.home, "agent-project");
    const std::vector<std::pair<std::string, json>> features{
        {"background", true},
        {"tools", json::array({{{"type", "web_search_preview"}}})},
        {"tools", json::array({{{"type", "file_search"}, {"vector_store_ids", {"vs_other"}}}})},
        {"tools", json::array({{{"type", "code_interpreter"}, {"container", {{"type", "auto"}}}}})},
        {"tools", json::array({{{"type", "image_generation"}}})},
        {"tools", json::array({{{"type", "mcp"}, {"server_url", "https://example.test"}}})},
        {"tools", json::array({{{"type", "future_paid_tool"}}})},
        {"previous_response_id", "resp_other"},
        {"conversation", "conv_other"},
        {"prompt", {{"id", "pmpt_other"}}},
        {"store", true},
        {"store", "false"},
        {"service_tier", "priority"},
        {"modalities", {"text", "audio"}},
        {"audio", {{"voice", "alloy"}, {"format", "wav"}}},
        {"web_search_options", json::object()},
        {"future_paid_feature", true},
        {"Background", true},
        {"stream_options", {{"future_paid_feature", true}}},
        {"tools", json::array({{{"type", "function"}, {"name", "f"}, {"container", "auto"}}})},
        {"tools",
         json::array(
             {{{"type", "function"}, {"function", {{"name", "f"}, {"container", "auto"}}}}})},
        {"tool_choice", {{"type", "web_search_preview"}}},
        {"input", json::array({{{"type", "item_reference"}, {"id", "msg_other"}}})},
        {"input", json::array({nullptr})},
        {"input",
         json::array({{{"role", "user"},
                       {"content",
                        json::array({{{"type", "input_image"},
                                      {"image_url", "https://example.test/x"}}})}}})},
        {"input",
         json::array(
             {{{"role", "user"},
               {"content", json::array({{{"type", "input_file"}, {"file_id", "file_other"}}})}}})},
        {"messages",
         json::array(
             {{{"role", "user"},
               {"content",
                json::array({{{"type", "input_audio"},
                              {"input_audio", {{"data", "AA=="}, {"format", "wav"}}}}})}}})},
        {"messages", json::array({{{"role", "assistant"}, {"audio", {{"id", "audio_other"}}}}})},
        {"text", {{"format", {{"type", "future_paid_format"}}}}},
        {"logit_bias", {{"tools", {{"type", "web_search_preview"}}}}},
    };
    for (const std::string endpoint : {"chat/completions", "responses", "embeddings"}) {
        for (const auto& [field, value] : features) {
            SCOPED_TRACE(endpoint + ": " + field + "=" + value.dump());
            json request{{"model", endpoint == "embeddings" ? "text-embedding-3-small" : "gpt-5.5"},
                         {field, value}};
            auto context = rig.task_context(task);
            const auto refused = rig.call({{"endpoint", endpoint}, {"request", request}}, context);
            EXPECT_FALSE(refused.ok);
            EXPECT_EQ(refused.error.code, "invalid_input");
            EXPECT_TRUE(refused.error.path.starts_with("/request/"));
            EXPECT_EQ(rig.fake.calls(), 0U);
        }
    }
}

TEST(ModelGateway, RejectsEncodedFieldsAndReturnsEscapedPointers) {
    using lake::api::prepare_model_request;
    const auto encoded = prepare_model_request(
        "responses",
        json::parse(
            R"({"model":"gpt-5.5","\u0074ools":[{"type":"web_search_preview","name":"lookup"}]})"));
    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error().path, "/request/tools/0/type");
    const auto escaped =
        prepare_model_request("responses", {{"model", "gpt-5.5"}, {"future/paid~feature", true}});
    ASSERT_FALSE(escaped);
    EXPECT_EQ(escaped.error().path, "/request/future~1paid~0feature");
}

TEST(ModelCompletion, CountsTransportFailuresWithoutAnHttpStatus) {
    GatewayRig rig;
    const auto task = rig.task(rig.home, "agent-project");
    // A timeout cannot establish whether the provider generated a billable answer.
    rig.fake.answer = {0, {}, "outbound request timed out"};
    auto context = rig.task_context(task);
    const auto failed = rig.call(chat(), context);
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.error.code, "store_unavailable");
    ASSERT_EQ(rig.fake.calls(), 1U);
    const auto input = lake::api::estimated_model_tokens(rig.fake.requests[0].body.size());
    EXPECT_EQ(context.audit_details["input_tokens"], input);
    EXPECT_EQ(context.audit_details["usage_reported"], false);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"dimension", "model_tokens"}}),
              static_cast<double>(input));
}

TEST(ModelGateway, AcceptsTextAndCallerExecutedFunctions) {
    using lake::api::prepare_model_request;
    // A schema's property names are data, even when they match disabled provider features.
    const json parameters{
        {"type", "object"},
        {"properties", {{"background", {{"type", "boolean"}}}, {"tools", {{"type", "string"}}}}}};
    const json function{{"name", "lookup"}, {"parameters", parameters}, {"strict", true}};
    auto completion = chat()["request"];
    completion["tools"] = json::array({{{"type", "function"}, {"function", function}}});
    completion["tool_choice"] = {{"type", "function"}, {"function", {{"name", "lookup"}}}};
    completion["messages"].push_back(
        {{"role", "assistant"},
         {"content", nullptr},
         {"tool_calls",
          json::array({{{"id", "call_1"},
                        {"type", "function"},
                        {"function", {{"name", "lookup"}, {"arguments", "{}"}}}}})}});
    completion["messages"].push_back(
        {{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "ok"}});
    completion["response_format"] = {{"type", "json_schema"},
                                     {"json_schema", {{"name", "result"}, {"schema", parameters}}}};
    const auto chat_request = prepare_model_request("chat/completions", completion);
    ASSERT_TRUE(chat_request) << chat_request.error().message;
    EXPECT_EQ((*chat_request)["tools"], completion["tools"]);
    EXPECT_EQ((*chat_request)["store"], false);

    json tool = function;
    tool["type"] = "function";
    const auto response = prepare_model_request(
        "responses",
        {{"model", "gpt-5.5"},
         {"tools", json::array({tool})},
         {"tool_choice", {{"type", "function"}, {"name", "lookup"}}},
         {"input",
          json::array(
              {{{"role", "user"},
                {"content", json::array({{{"type", "input_text"}, {"text", "hello"}}})}},
               {{"type", "function_call"},
                {"call_id", "call_1"},
                {"name", "lookup"},
                {"arguments", "{}"}},
               {{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "ok"}}})},
         {"text",
          {{"format", {{"type", "json_schema"}, {"name", "result"}, {"schema", parameters}}}}}});
    ASSERT_TRUE(response) << response.error().message;
    EXPECT_EQ((*response)["store"], false);
    for (const json& input :
         {json("hello"), json({"hello", "world"}), json({1, 2}), json({{1, 2}, {3, 4}})}) {
        const auto embedding = prepare_model_request(
            "embeddings", {{"model", "text-embedding-3-small"}, {"input", input}});
        ASSERT_TRUE(embedding) << embedding.error().message;
    }
}

// An agent's second tool round sends back the assistant message the provider answered, as the
// OpenAI SDK dumps it (`message.model_dump(exclude_none=True)` in agent-runtime's loop.py,
// agent-ask-orama, agent-application and agent-project). The answer below is a chat completion
// with a tool call as OpenAI returns it; its message carries the output-only `annotations`,
// `refusal` and `audio`. The gateway drops them and forwards only the closed message fields.
TEST(ModelCompletion, AcceptsTheProvidersAssistantMessageBackAsHistory) {
    const json answer = json::parse(R"({
      "id": "chatcmpl-CUq7tqH1u8wDwLHvGdv0fT1Q",
      "object": "chat.completion",
      "created": 1761200000,
      "model": "gpt-5.5-2026-08-01",
      "choices": [{
        "index": 0,
        "message": {
          "role": "assistant",
          "content": null,
          "tool_calls": [{
            "id": "call_Zl3kq2f0bS1yYvYp0aR7tq9B",
            "type": "function",
            "function": {"name": "workspace_query", "arguments": "{\"sql\":\"select 1\"}"}
          }],
          "refusal": null,
          "annotations": [],
          "audio": null
        },
        "logprobs": null,
        "finish_reason": "tool_calls"
      }],
      "usage": {"prompt_tokens": 120, "completion_tokens": 18, "total_tokens": 138},
      "service_tier": "default",
      "system_fingerprint": null
    })");
    const json& message = answer["choices"][0]["message"];
    // What model_dump(exclude_none=True) leaves: the nulls go, the empty annotations stay.
    json dumped = json::object();
    for (const auto& [key, value] : message.items()) {
        if (!value.is_null()) {
            dumped[key] = value;
        }
    }
    ASSERT_TRUE(dumped.contains("annotations"));
    // A refusal string, as an answer that refused carries it.
    json refused = dumped;
    refused["refusal"] = "I can't help with that.";
    refused["content"] = "I can't help with that.";

    GatewayRig rig;
    const auto task = rig.task(rig.home, "agent-runtime");
    for (const json& history : {message, dumped, refused}) {
        SCOPED_TRACE(history.dump());
        auto request = chat()["request"];
        request["tools"] = json::array(
            {{{"type", "function"},
              {"function", {{"name", "workspace_query"}, {"parameters", {{"type", "object"}}}}}}});
        request["messages"].push_back(history);
        request["messages"].push_back({{"role", "tool"},
                                       {"tool_call_id", "call_Zl3kq2f0bS1yYvYp0aR7tq9B"},
                                       {"content", "[{\"?column?\":1}]"}});
        const auto before = rig.fake.calls();
        auto context = rig.task_context(task);
        const auto second =
            rig.call({{"endpoint", "chat/completions"}, {"request", request}}, context);
        ASSERT_TRUE(second.ok) << second.error.code << ": " << second.error.message << " at "
                               << second.error.path;
        ASSERT_EQ(rig.fake.calls(), before + 1);
        const json sent = json::parse(rig.fake.requests.back().body);
        const json& forwarded = sent.at("messages").at(1);
        EXPECT_EQ(forwarded.at("role"), "assistant");
        EXPECT_EQ(forwarded.at("tool_calls"), message.at("tool_calls"));
        for (const char* output_only : {"annotations", "refusal", "audio"}) {
            EXPECT_FALSE(forwarded.contains(output_only)) << output_only;
        }
        for (const auto& [key, value] : forwarded.items()) {
            EXPECT_TRUE(key == "role" || key == "content" || key == "tool_calls") << key;
        }
    }

    // Only answers' fields are dropped: a user message with them, and stored audio, stay refused.
    for (const json& history :
         {json{{"role", "user"}, {"content", "hi"}, {"annotations", json::array()}},
          json{{"role", "assistant"}, {"content", "hi"}, {"audio", {{"id", "audio_other"}}}},
          json{{"role", "assistant"}, {"content", "hi"}, {"future_output_field", true}}}) {
        SCOPED_TRACE(history.dump());
        auto request = chat()["request"];
        request["messages"].push_back(history);
        const auto before = rig.fake.calls();
        auto context = rig.task_context(task);
        const auto refused_call =
            rig.call({{"endpoint", "chat/completions"}, {"request", request}}, context);
        EXPECT_FALSE(refused_call.ok);
        EXPECT_EQ(refused_call.error.code, "invalid_input");
        EXPECT_EQ(rig.fake.calls(), before);
    }
}

TEST(ModelGateway, ReadsTheUsageOfEveryAnswerShape) {
    using lake::api::model_usage;
    const auto completion = model_usage(chat_answer.dump(), false);
    EXPECT_TRUE(completion.reported);
    EXPECT_EQ(completion.model, "gpt-5.5-2026-08-01");
    EXPECT_EQ(completion.input_tokens, 1200U);
    EXPECT_EQ(completion.output_tokens, 300U);

    const auto response = model_usage(
        json{{"object", "response"},
             {"model", "gpt-5.5"},
             {"usage", {{"input_tokens", 70}, {"output_tokens", 9}, {"total_tokens", 79}}}}
            .dump(),
        false);
    EXPECT_EQ(response.input_tokens, 70U);
    EXPECT_EQ(response.output_tokens, 9U);

    const auto embedding = model_usage(
        json{{"object", "list"},
             {"model", "text-embedding-3-small"},
             {"usage", {{"prompt_tokens", 8}, {"total_tokens", 8}}}}
            .dump(),
        false);
    EXPECT_TRUE(embedding.reported);
    EXPECT_EQ(embedding.input_tokens, 8U);
    EXPECT_EQ(embedding.output_tokens, 0U);

    const auto streamed = model_usage(chat_stream, true);
    EXPECT_TRUE(streamed.reported);
    EXPECT_EQ(streamed.model, "gpt-5.5-2026-08-01");
    EXPECT_EQ(streamed.input_tokens, 40U);
    EXPECT_EQ(streamed.output_tokens, 2U);

    const std::string responses_stream =
        "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"Hi\"}\n\n"
        "event: response.completed\r\ndata: {\"type\":\"response.completed\",\"response\":"
        "{\"model\":\"gpt-5.5\",\"usage\":{\"input_tokens\":11,\"output_tokens\":3}}}\r\n\r\n";
    const auto completed = model_usage(responses_stream, true);
    EXPECT_TRUE(completed.reported);
    EXPECT_EQ(completed.model, "gpt-5.5");
    EXPECT_EQ(completed.input_tokens, 11U);
    EXPECT_EQ(completed.output_tokens, 3U);

    EXPECT_FALSE(model_usage("{\"choices\":[]}", false).reported);
    EXPECT_FALSE(model_usage("not json", false).reported);
    EXPECT_FALSE(model_usage("data: [DONE]\n\n", true).reported);
}

TEST(ModelGateway, SaysWhatTheProviderRefusedWithoutEchoingLakesKey) {
    using lake::api::model_provider_failure;
    const auto key = model_provider_failure(
        401, R"({"error":{"message":"Incorrect API key provided: sk-abc***xyz"}})", "");
    EXPECT_EQ(key.code, "store_unavailable");
    EXPECT_EQ(key.message.find("sk-"), std::string::npos);
    const auto refused = model_provider_failure(
        400,
        R"({"error":{"message":"This model's maximum context length is 128000 tokens."}})",
        "");
    EXPECT_EQ(refused.code, "engine_error");
    EXPECT_NE(refused.message.find("maximum context length"), std::string::npos);
    EXPECT_EQ(model_provider_failure(429, "{}", "").code, "rate_limited");
    EXPECT_EQ(model_provider_failure(503, "", "").code, "store_unavailable");
    EXPECT_EQ(model_provider_failure(0, "", "connection refused").code, "store_unavailable");
}

TEST(ModelGateway, OpenAIProviderSendsLakesKeyToTheConfiguredBase) {
    std::string url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    const auto provider = lake::api::openai_model_provider(
        "sk-platform",
        "http://127.0.0.1:9/v1/",
        [&](const std::string& to,
            std::string sent,
            auto sent_headers,
            std::chrono::seconds,
            const lake::api::ModelBodySink&) {
            url = to;
            body = std::move(sent);
            headers = std::move(sent_headers);
            return ModelProviderResponse{200, "{}", {}};
        });
    const auto answered = provider({"chat/completions", R"({"model":"gpt-5.5"})", true});
    EXPECT_EQ(answered.status, 200);
    EXPECT_EQ(url, "http://127.0.0.1:9/v1/chat/completions");
    EXPECT_EQ(body, R"({"model":"gpt-5.5"})");
    EXPECT_NE(std::ranges::find(headers, std::pair<std::string, std::string>{
                                             "Authorization", "Bearer sk-platform"}),
              headers.end());
    EXPECT_NE(std::ranges::find(headers, std::pair<std::string, std::string>{
                                             "Accept", "text/event-stream"}),
              headers.end());
}

TEST(ModelCompletion, CountsATurnsTokensInItsWorkspaceAndKeepsThePromptOutOfTheAudit) {
    GatewayRig rig;
    const auto task = rig.task(rig.lab, "agent-ask-orama");
    auto context = rig.task_context(task);
    const auto answered = rig.call(chat(), context);
    ASSERT_TRUE(answered.ok) << answered.error.code << ": " << answered.error.message;
    EXPECT_EQ(answered.value["workspace_id"], rig.lab);
    EXPECT_EQ(answered.value["agent"], "agent-ask-orama");
    EXPECT_EQ(answered.value["launch_id"], task.id);
    EXPECT_EQ(answered.value["input_tokens"], 1200);
    EXPECT_EQ(answered.value["output_tokens"], 300);
    EXPECT_EQ(answered.value["response"]["choices"][0]["message"]["content"],
              "Churn is 4.2% this month.");

    ASSERT_EQ(rig.fake.calls(), 1U);
    const json sent = json::parse(rig.fake.requests[0].body);
    EXPECT_EQ(rig.fake.requests[0].endpoint, "chat/completions");
    EXPECT_EQ(sent["model"], "gpt-5.5");
    EXPECT_EQ(sent["messages"][0]["content"], "What is churn this month?");

    // The audit says which model, how many tokens, for which run; never what was asked.
    EXPECT_EQ(context.audit_workspace_id, rig.lab);
    EXPECT_EQ(context.audit_object, "agent_usage/" + task.id);
    EXPECT_EQ(context.audit_details["model"], "gpt-5.5");
    EXPECT_EQ(context.audit_details["input_tokens"], 1200);
    const std::string audited = context.audit_details.dump();
    EXPECT_EQ(audited.find("churn"), std::string::npos) << audited;
    EXPECT_EQ(audited.find("4.2%"), std::string::npos) << audited;

    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    auto& series = rig.workspaces.records(rig.lab);
    EXPECT_EQ(series.metric_total("lake_usage",
                                  {{"dimension", "model_tokens"},
                                   {"kind", "input"},
                                   {"agent", "agent-ask-orama"},
                                   {"launch_id", task.id},
                                   {"model", "gpt-5.5"},
                                   {"class", "gpt-5.5/input"}}),
              1200);
    EXPECT_EQ(
        series.metric_total("lake_usage", {{"dimension", "model_tokens"}, {"kind", "output"}}),
        300);
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage"), 0)
        << "only the task's own workspace is counted";
}

TEST(ModelCompletion, CountsAStreamFromItsLastEventAndAnswersTheWholeStream) {
    GatewayRig rig;
    rig.fake.answer = {200, chat_stream, {}};
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    const auto answered = rig.call(chat(true), context);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(answered.value["streamed"], true);
    EXPECT_EQ(answered.value["events"], chat_stream);
    EXPECT_EQ(answered.value["input_tokens"], 40);
    ASSERT_EQ(rig.fake.calls(), 1U);
    EXPECT_TRUE(rig.fake.requests[0].stream);
    EXPECT_EQ(json::parse(rig.fake.requests[0].body)["stream_options"]["include_usage"], true);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"dimension", "model_tokens"}}),
              42);
}

// lake#665: a stream read piece by piece, however the pieces cut its events, says what the
// whole stream says.
TEST(ModelGateway, ReadsAStreamsUsageHoweverItsPiecesFall) {
    const auto whole = lake::api::model_usage(chat_stream, true);
    for (const std::size_t size : {std::size_t{1}, std::size_t{7}, std::size_t{64}}) {
        lake::api::ModelStreamUsage reader;
        for (std::size_t at = 0; at < chat_stream.size(); at += size) {
            reader.feed(std::string_view(chat_stream).substr(at, size));
        }
        reader.finish();
        EXPECT_TRUE(reader.usage().reported) << size;
        EXPECT_EQ(reader.usage().model, whole.model) << size;
        EXPECT_EQ(reader.usage().input_tokens, 40U) << size;
        EXPECT_EQ(reader.usage().output_tokens, 2U) << size;
    }
    // A last event without its line end is still read.
    lake::api::ModelStreamUsage unterminated;
    unterminated.feed("data: {\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}");
    EXPECT_FALSE(unterminated.usage().reported);
    unterminated.finish();
    EXPECT_TRUE(unterminated.usage().reported);
    EXPECT_EQ(unterminated.usage().input_tokens, 5U);
}

// lake#665: the `/openai/v1/*` route hands model_completion its caller's stream; each piece goes
// on as it arrives, and the usage is still counted from the event that carries it.
TEST(ModelCompletion, RelaysAStreamToItsCallerAndCountsItsLastEvent) {
    GatewayRig rig;
    rig.fake.answer = {200, chat_stream, {}};
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    std::vector<std::string> relayed;
    context.model_stream = [&](std::string_view piece) {
        relayed.emplace_back(piece);
        return true;
    };
    const auto answered = rig.call(chat(true), context);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_GT(relayed.size(), 1U) << "the stream went on in pieces";
    std::string joined;
    for (const auto& piece : relayed) {
        joined += piece;
    }
    EXPECT_EQ(joined, chat_stream);
    EXPECT_FALSE(answered.value.contains("events")) << "the caller already has the stream";
    EXPECT_EQ(answered.value["streamed"], true);
    EXPECT_EQ(answered.value["model"], "gpt-5.5-2026-08-01");
    EXPECT_EQ(answered.value["input_tokens"], 40);
    EXPECT_EQ(answered.value["output_tokens"], 2);
    EXPECT_EQ(context.audit_details["usage_reported"], true);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"dimension", "model_tokens"}}),
              42);
}

// lake#939: an answer without usage is counted by estimate, never as zero.
TEST(ModelCompletion, CountsARelayedStreamWithoutUsageByEstimate) {
    GatewayRig rig;
    const std::string no_usage =
        "data: {\"id\":\"c\",\"model\":\"gpt-5.5\",\"choices\":[{\"index\":0,\"delta\":"
        "{\"content\":\"Hi\"}}]}\n\n"
        "data: [DONE]\n\n";
    rig.fake.answer = {200, no_usage, {}};
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    std::string joined;
    context.model_stream = [&](std::string_view piece) {
        joined.append(piece);
        return true;
    };
    const auto answered = rig.call(chat(true), context);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(joined, no_usage);
    ASSERT_EQ(rig.fake.calls(), 1U);
    const std::uint64_t input = lake::api::estimated_model_tokens(rig.fake.requests[0].body.size());
    const std::uint64_t output = lake::api::estimated_model_tokens(no_usage.size());
    EXPECT_GT(input, 0U);
    EXPECT_GT(output, 0U);
    EXPECT_EQ(answered.value["usage_reported"], false);
    EXPECT_EQ(answered.value["recorded"], true);
    EXPECT_EQ(answered.value["input_tokens"], input);
    EXPECT_EQ(answered.value["output_tokens"], output);
    EXPECT_EQ(context.audit_details["usage_reported"], false);
    EXPECT_EQ(context.audit_details["input_tokens"], input);
    EXPECT_EQ(context.audit_details["output_tokens"], output);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"dimension", "model_tokens"}}),
              static_cast<double>(input + output));
}

// lake#939: a caller that stops reading a relayed stream is sent nothing more, but the provider's
// stream is still read to its end and the usage in its last event counted.
TEST(ModelCompletion, CountsAStreamWhoseCallerStoppedReading) {
    GatewayRig rig;
    rig.fake.answer = {200, chat_stream, {}};
    const auto task = rig.task(rig.home, "agent-project");
    auto context = rig.task_context(task);
    std::size_t offered = 0;
    context.model_stream = [&](std::string_view) {
        ++offered;
        return false; // gone at the first piece
    };
    const auto answered = rig.call(chat(true), context);
    ASSERT_TRUE(answered.ok) << answered.error.code << ": " << answered.error.message;
    EXPECT_EQ(offered, 1U) << "a caller that has gone is offered nothing more";
    EXPECT_EQ(answered.value["usage_reported"], true);
    EXPECT_EQ(answered.value["input_tokens"], 40);
    EXPECT_EQ(answered.value["output_tokens"], 2);
    EXPECT_EQ(context.audit_details["usage_reported"], true);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                            {{"dimension", "model_tokens"}}),
              42);
}

// lake#939: an answer that fails part way (the provider drops the connection, the deadline
// passes) was already charged for by the provider: it is counted by estimate, relayed or not,
// streamed or not, before the failure is answered.
TEST(ModelCompletion, CountsAnAnswerThatFailedPartWay) {
    const std::string first_event = chat_stream.substr(0, chat_stream.find("\n\n") + 2);
    struct Case {
        bool stream;
        bool relayed;
        std::string body;
    };
    for (const Case& shape : {Case{true, true, first_event},
                              Case{true, false, first_event},
                              Case{false, false, chat_answer.dump().substr(0, 40)}}) {
        GatewayRig rig;
        rig.fake.answer = {200, shape.body, "outbound request timed out"};
        const auto task = rig.task(rig.home, "agent-project");
        auto context = rig.task_context(task);
        std::string joined;
        if (shape.relayed) {
            context.model_stream = [&](std::string_view piece) {
                joined.append(piece);
                return true;
            };
        }
        const auto failed = rig.call(chat(shape.stream), context);
        EXPECT_FALSE(failed.ok) << shape.stream << shape.relayed;
        EXPECT_EQ(failed.error.code, "store_unavailable");
        ASSERT_EQ(rig.fake.calls(), 1U);
        const std::uint64_t input =
            lake::api::estimated_model_tokens(rig.fake.requests[0].body.size());
        const std::uint64_t output = lake::api::estimated_model_tokens(shape.body.size());
        EXPECT_EQ(context.audit_details["usage_reported"], false);
        EXPECT_EQ(context.audit_details["input_tokens"], input);
        EXPECT_EQ(context.audit_details["output_tokens"], output);
        static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
        EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage",
                                                                {{"dimension", "model_tokens"}}),
                  static_cast<double>(input + output))
            << shape.stream << shape.relayed;
    }
}

// lake#939: while the provider answers one call, the estimate of its input is held against the
// account's limit, so a call made meanwhile cannot pass a limit the first will reach. A call the
// provider refuses releases it.
TEST(ModelCompletion, HoldsAnEstimateOfTheInputWhileTheProviderAnswers) {
    GatewayRig rig;
    auto admin = rig.admin();
    ASSERT_TRUE(liquid::runtime::block_on(rig.registry.call_async("set_usage_limit",
                                                                  {{"customer_id", rig.customer},
                                                                   {"dimension", "model_tokens"},
                                                                   {"limit", 10}},
                                                                  admin))
                    ->ok);
    const auto task = rig.task(rig.home, "agent-application");
    auto context = rig.task_context(task);
    ApiResult meanwhile = ApiResult::failure("not_called", "");
    context.model_provider = [&](const ModelProviderRequest& request) {
        EXPECT_GE(lake::api::estimated_model_tokens(request.body.size()), 10U);
        auto second = rig.task_context(task);
        meanwhile = rig.call(chat(), second);
        return ModelProviderResponse{
            400, R"({"error":{"message":"Invalid value for 'temperature'."}})", {}};
    };
    EXPECT_EQ(rig.call(chat(), context).error.code, "engine_error");
    EXPECT_EQ(meanwhile.error.code, "usage_limit_reached");
    EXPECT_EQ(rig.fake.calls(), 0U);

    auto after = rig.task_context(task);
    const auto answered = rig.call(chat(), after);
    EXPECT_TRUE(answered.ok) << "the refused call counted nothing and held nothing after it: "
                             << answered.error.code;
    EXPECT_EQ(rig.fake.calls(), 1U);
}

TEST(ModelCompletion, CountsALaunchedAgentsCallsAndRefusesEveryOtherCaller) {
    GatewayRig rig;
    const auto owner = rig.store->get_customer(rig.customer);
    ASSERT_TRUE(owner);
    const auto launch = rig.store->create_agent_launch({.customer_id = rig.customer,
                                                        .agent_id = "agent-1",
                                                        .agent_name = "Churn Watcher",
                                                        .mode = "job",
                                                        .trigger = "manual",
                                                        .scopes = {"workspace_query"},
                                                        .created_by_user_id = owner->user_id,
                                                        .workspace_id = rig.home});
    ASSERT_TRUE(launch) << launch.error().message;
    auto launched = rig.admin();
    launched.acting_identity = "taap:launch-person";
    launched.operator_credential = false;
    launched.credential_kind = "agent_launch";
    launched.credential_session_id = "launch:" + launch->id;
    const auto answered = rig.call(chat(), launched);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(answered.value["agent"], "Churn Watcher");
    EXPECT_EQ(answered.value["launch_id"], launch->id);

    // A person's own key, the operator's, a library agent's bus credential, or none: refused
    // before the provider is called.
    auto person = rig.admin();
    person.acting_identity = "taap:someone";
    person.operator_credential = false;
    person.credential_kind = "session";
    EXPECT_EQ(rig.call(chat(), person).error.code, "unauthorized");
    auto operator_key = rig.admin();
    EXPECT_EQ(rig.call(chat(), operator_key).error.code, "unauthorized");
    auto bus = rig.admin();
    bus.credential_kind = "agent_service";
    bus.agent_service = "agent-ask-orama";
    EXPECT_EQ(rig.call(chat(), bus).error.code, "unauthorized");
    auto nobody = rig.admin();
    nobody.authorized = false;
    nobody.acting_identity.clear();
    EXPECT_EQ(rig.call(chat(), nobody).error.code, "unauthenticated");
    EXPECT_EQ(rig.fake.calls(), 1U) << "only the launch's call reached the provider";

    // No provider configured: nothing is sent.
    auto unconfigured = launched;
    unconfigured.model_provider = nullptr;
    EXPECT_EQ(rig.call(chat(), unconfigured).error.code, "not_supported");
}

// orama-operator#779: a launched agent's LAKE_URL is lake followed by /w/<its workspace>, so its
// gateway calls name a workspace in the path. A call naming another workspace than its run's is
// refused before the provider is called; the run's own workspace is answered.
TEST(ModelCompletion, ACallUnderAWorkspacePathMustNameItsRunsWorkspace) {
    GatewayRig rig;
    const auto owner = rig.store->get_customer(rig.customer);
    ASSERT_TRUE(owner);
    const auto launch = rig.store->create_agent_launch({.customer_id = rig.customer,
                                                        .agent_id = "agent-1",
                                                        .agent_name = "Churn Watcher",
                                                        .mode = "job",
                                                        .trigger = "manual",
                                                        .scopes = {"workspace_query"},
                                                        .created_by_user_id = owner->user_id,
                                                        .workspace_id = rig.home});
    ASSERT_TRUE(launch) << launch.error().message;
    auto launched = rig.admin();
    launched.acting_identity = "taap:launch-person";
    launched.operator_credential = false;
    launched.credential_kind = "agent_launch";
    launched.credential_session_id = "launch:" + launch->id;

    auto elsewhere = launched;
    elsewhere.path_workspace_id = rig.lab;
    elsewhere.workspace_id = rig.lab;
    EXPECT_EQ(rig.call(chat(), elsewhere).error.code, "unauthorized");
    EXPECT_EQ(rig.fake.calls(), 0U) << "a call naming another workspace never reaches the provider";

    auto own = launched;
    own.path_workspace_id = rig.home;
    own.workspace_id = rig.home;
    const auto answered = rig.call(chat(), own);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(rig.fake.calls(), 1U);
}

TEST(ModelCompletion, PassesOnTheProvidersRefusalAndCountsNothing) {
    GatewayRig rig;
    rig.fake.answer = {400,
                       R"({"error":{"message":"Invalid value for 'temperature'.",)"
                       R"("type":"invalid_request_error"}})",
                       {}};
    const auto task = rig.task(rig.home, "agent-autosuggest");
    auto context = rig.task_context(task);
    const auto refused = rig.call(chat(), context);
    EXPECT_EQ(refused.error.code, "engine_error");
    EXPECT_NE(refused.error.message.find("temperature"), std::string::npos);
    EXPECT_EQ(context.audit_details["provider_status"], 400);
    static_cast<void>(liquid::runtime::block_on(rig.recorder.flush()));
    EXPECT_EQ(rig.workspaces.records(rig.home).metric_total("lake_usage"), 0);
}

TEST(ModelCompletion, RefusesCallsOnceTheAccountsMonthlyLimitIsUsed) {
    GatewayRig rig;
    auto admin = rig.admin();
    ASSERT_TRUE(liquid::runtime::block_on(
                    rig.registry.call_async("set_usage_limit",
                                            {{"customer_id", rig.customer},
                                             {"dimension", "model_tokens"},
                                             {"limit", 1000}},
                                            admin))
                    ->ok);
    const auto task = rig.task(rig.home, "agent-application");
    auto context = rig.task_context(task);
    // Nothing used yet: the first call is answered, and its 1,500 tokens use up the limit.
    ASSERT_TRUE(rig.call(chat(), context).ok);
    const auto refused = rig.call(chat(), context);
    EXPECT_EQ(refused.error.code, "usage_limit_reached");
    EXPECT_NE(refused.error.message.find("1000"), std::string::npos);
    EXPECT_EQ(rig.fake.calls(), 1U) << "the refused call never reached the provider";
    EXPECT_EQ(lake::api::find_error_code("usage_limit_reached")->status, 429);

    auto read = rig.admin();
    const auto limits = liquid::runtime::block_on(
        rig.registry.call_async("usage_limits", {{"customer_id", rig.customer}}, read));
    ASSERT_TRUE(limits && limits->ok);
    bool found = false;
    for (const json& limit : limits->value["limits"]) {
        if (limit["dimension"] == "model_tokens") {
            found = true;
            EXPECT_EQ(limit["enforced"], true);
        }
    }
    EXPECT_TRUE(found);
}

// The OpenAI-compatible route over the real server: an OpenAI SDK pointed at `<lake>/openai/v1`
// with a task credential as its key gets the provider's own answer, and OpenAI's error shape.
TEST(ModelGatewayRoute, AnswersTheOpenAIShapeForATaskCredential) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "gateway-route");
    const std::string home = workspaces.create(customer, "home");
    const auto owner = store->get_customer(customer);
    ASSERT_TRUE(owner);
    lake::control::AgentTask row;
    row.id = lake::control::new_uuid_v4();
    row.customer_id = customer;
    row.agent = "agent-ask-orama";
    row.kind = "turn";
    row.workspace_id = home;
    row.scopes = {"workspace_query"};
    row.requested_by_user_id = owner->user_id;
    ASSERT_TRUE(store->create_agent_task(row));
    const std::string credential =
        std::string(lake::control::agent_task_credential_prefix) + row.id +
        ".0123456789abcdef0123456789abcdef";
    ASSERT_TRUE(store->claim_agent_task(
        row.id, row.agent, lake::control::identity_crypto::sha256_hex(credential)));

    FakeProvider fake;
    lake::api::ApiServer server({.port = 0,
                                 .node_name = "gateway-test",
                                 .store = store,
                                 .workspaces = workspaces.service,
                                 .admin_token = "gateway-admin",
                                 .instance_kek = std::string(kek),
                                 .model_provider = fake.provider()});
    ASSERT_TRUE(server.start()) << server.reason();
    const auto post = [&](const std::string& path, const std::string& key, const json& body) {
        const std::string data = body.dump();
        const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
        const auto response = lake::test::http_request(
            {.url = "http://127.0.0.1:" + std::to_string(server.port()),
             .method = "POST",
             .suffix = path,
             .headers = {{"Authorization", "Bearer " + key}, {"Content-Type", "application/json"}},
             .body = std::vector<std::byte>(bytes, bytes + data.size())});
        EXPECT_TRUE(response);
        return std::pair<unsigned, json>{
            response->status,
            json::parse(std::string(reinterpret_cast<const char*>(response->body.data()),
                                    response->body.size()),
                        nullptr,
                        false)};
    };
    auto unlisted = chat();
    unlisted["request"]["model"] = "premium-model";
    for (const auto& path : {std::string("/openai/v1/chat/completions"),
                             "/w/" + home + "/openai/v1/chat/completions"}) {
        const auto [code, refused] = post(path, credential, unlisted["request"]);
        EXPECT_EQ(code, 400U);
        EXPECT_EQ(refused["error"]["code"], "invalid_input");
    }
    const auto [rest_code, rest_refused] = post("/v1/model_completion", credential, unlisted);
    EXPECT_EQ(rest_code, 400U);
    const auto [mcp_code, mcp_refused] =
        post("/mcp",
             credential,
             {{"jsonrpc", "2.0"},
              {"id", 1},
              {"method", "tools/call"},
              {"params", {{"name", "model_completion"}, {"arguments", unlisted}}}});
    EXPECT_EQ(mcp_code, 200U);
    EXPECT_EQ(mcp_refused["result"]["isError"], true);
    EXPECT_NE(mcp_refused.dump().find("invalid_input"), std::string::npos);
    EXPECT_EQ(fake.calls(), 0U);

    const auto [status, answer] =
        post("/openai/v1/chat/completions", credential, chat()["request"]);
    EXPECT_EQ(status, 200U) << answer.dump();
    EXPECT_EQ(answer["object"], "chat.completion");
    EXPECT_EQ(answer["usage"]["prompt_tokens"], 1200);
    EXPECT_EQ(fake.calls(), 1U);

    for (const std::string prefix : {std::string("/openai/v1/"), "/w/" + home + "/openai/v1/"}) {
        for (const std::string endpoint : {"chat/completions", "responses", "embeddings"}) {
            json request{{"model", endpoint == "embeddings" ? "text-embedding-3-small" : "gpt-5.5"},
                         {"tools", json::array({{{"type", "web_search_preview"}}})}};
            const auto [blocked_status, blocked] = post(prefix + endpoint, credential, request);
            EXPECT_EQ(blocked_status, 400U) << blocked.dump();
            EXPECT_EQ(blocked["error"]["code"], "invalid_input");
            EXPECT_EQ(fake.calls(), 1U);
            request.erase("tools");
            request["background"] = true;
            const auto [async_status, async] = post(prefix + endpoint, credential, request);
            EXPECT_EQ(async_status, 400U) << async.dump();
            EXPECT_EQ(async["error"]["param"], "/request/background");
            EXPECT_EQ(fake.calls(), 1U);
        }
    }

    const auto [refused_status, refused] =
        post("/openai/v1/chat/completions", "gateway-admin", chat()["request"]);
    EXPECT_EQ(refused_status, 403U);
    EXPECT_EQ(refused["error"]["code"], "unauthorized");
    EXPECT_TRUE(refused["error"]["message"].is_string());

    const auto [invalid_status, invalid] =
        post("/openai/v1/embeddings", credential, {{"input", "no model"}});
    EXPECT_EQ(invalid_status, 400U);
    EXPECT_EQ(invalid["error"]["param"], "/request/model");
    EXPECT_EQ(fake.calls(), 1U);

    // orama-operator#779: an agent in a workspace namespace has LAKE_URL = <lake>/w/<workspace>,
    // and its SDK calls <LAKE_URL>/openai/v1. Under its own workspace the gateway answers; under
    // another it refuses before the provider; under a workspace that does not exist it is not
    // found.
    const auto [scoped_status, scoped] =
        post("/w/" + home + "/openai/v1/chat/completions", credential, chat()["request"]);
    EXPECT_EQ(scoped_status, 200U) << scoped.dump();
    EXPECT_EQ(scoped["object"], "chat.completion");
    EXPECT_EQ(fake.calls(), 2U);
    const std::string other = workspaces.create(customer, "other");
    const auto [other_status, other_answer] =
        post("/w/" + other + "/openai/v1/chat/completions", credential, chat()["request"]);
    EXPECT_EQ(other_status, 403U) << other_answer.dump();
    EXPECT_EQ(other_answer["error"]["code"], "unauthorized");
    const auto [missing_status, missing] =
        post("/w/" + lake::control::new_uuid_v4() + "/openai/v1/chat/completions",
             credential,
             chat()["request"]);
    EXPECT_EQ(missing_status, 404U);
    EXPECT_EQ(fake.calls(), 2U) << "neither refused call reached the provider";
    server.stop();
}

// A model provider on loopback that streams a chat completion in chunked transfer: it sends its
// first event, then holds the last until `release` is set (or a deadline passes) and records
// whether it was released before it sent the last. Once released it sends `middle_events` more
// events, 20 ms apart, before the last.
class StreamingProvider {
public:
    StreamingProvider() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 4);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~StreamingProvider() {
        release.store(true);
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

    std::atomic<bool> release{false};
    std::atomic<int> middle_events{0};
    std::atomic<bool> released_before_last{false};
    std::atomic<bool> sent_last{false};
    std::string request_head;

    static constexpr std::string_view first =
        "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"FIRST\"}}],\"usage\":null}\n\n";
    static constexpr std::string_view middle =
        "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"MIDDLE\"}}],\"usage\":null}\n\n";
    static constexpr std::string_view last =
        "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[],\"usage\":"
        "{\"prompt_tokens\":40,\"completion_tokens\":2,\"total_tokens\":42}}\n\n"
        "data: [DONE]\n\n";

private:
    static void send_all(int fd, std::string_view bytes) {
        // A send to a connection lake closed fails instead of raising SIGPIPE.
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        while (!bytes.empty()) {
            const auto sent = ::send(fd, bytes.data(), bytes.size(), flags);
            if (sent <= 0) {
                return;
            }
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
    }
    static std::string chunk(std::string_view data) {
        std::ostringstream out;
        out << std::hex << data.size() << "\r\n" << data << "\r\n";
        return out.str();
    }
    void serve() {
        const int fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        std::string received;
        char buffer[4096];
        std::size_t head_end = std::string::npos;
        std::size_t wanted = 0;
        for (;;) {
            const auto got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(got));
            if (head_end == std::string::npos) {
                head_end = received.find("\r\n\r\n");
                if (head_end != std::string::npos) {
                    std::string head = received.substr(0, head_end);
                    std::ranges::transform(head, head.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    const auto length = head.find("content-length:");
                    wanted = length == std::string::npos
                                 ? 0
                                 : std::stoul(head.substr(length + 15));
                }
            }
            if (head_end != std::string::npos && received.size() >= head_end + 4 + wanted) {
                break;
            }
        }
        request_head = received.substr(0, head_end);
        send_all(fd,
                 "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                 "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        send_all(fd, chunk(first));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!release.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        released_before_last.store(release.load());
        for (int sent = 0; sent < middle_events.load(); ++sent) {
            send_all(fd, chunk(middle));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        send_all(fd, chunk(last));
        sent_last.store(true);
        send_all(fd, "0\r\n\r\n");
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread thread_;
};

// lake#665, over the real server and its outbound client: a caller streaming through
// `/openai/v1/chat/completions` reads the provider's first event while the provider still holds
// its last, and the usage in the last event is counted.
TEST(ModelGatewayRoute, RelaysEachEventAsTheProviderSendsIt) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "gateway-stream");
    const std::string home = workspaces.create(customer, "home");
    const auto owner = store->get_customer(customer);
    ASSERT_TRUE(owner);
    lake::control::AgentTask row;
    row.id = lake::control::new_uuid_v4();
    row.customer_id = customer;
    row.agent = "agent-ask-orama";
    row.kind = "turn";
    row.workspace_id = home;
    row.scopes = {"workspace_query"};
    row.requested_by_user_id = owner->user_id;
    ASSERT_TRUE(store->create_agent_task(row));
    const std::string credential =
        std::string(lake::control::agent_task_credential_prefix) + row.id +
        ".0123456789abcdef0123456789abcdef";
    ASSERT_TRUE(store->claim_agent_task(
        row.id, row.agent, lake::control::identity_crypto::sha256_hex(credential)));

    StreamingProvider provider;
    lake::api::ApiServer server({.port = 0,
                                 .node_name = "gateway-stream-test",
                                 .store = store,
                                 .workspaces = workspaces.service,
                                 .admin_token = "gateway-admin",
                                 .instance_kek = std::string(kek),
                                 .openai_api_key = "sk-fake-provider",
                                 .model_provider_url = provider.base_url()});
    ASSERT_TRUE(server.start()) << server.reason();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(server.port()));
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    timeval wait{20, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    const std::string body = chat(true)["request"].dump();
    const std::string sent = "POST /openai/v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                             "Authorization: Bearer " +
                             credential +
                             "\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\n"
                             "Connection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body;
    ASSERT_EQ(::send(fd, sent.data(), sent.size(), 0), static_cast<ssize_t>(sent.size()));

    std::string received;
    char buffer[4096];
    bool saw_first_early = false;
    for (;;) {
        const auto got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            break;
        }
        received.append(buffer, static_cast<std::size_t>(got));
        if (!provider.release.load() && received.find("FIRST") != std::string::npos) {
            // The first event is here; the provider has not sent its last.
            saw_first_early = !provider.sent_last.load();
            provider.release.store(true);
        }
    }
    ::close(fd);

    EXPECT_TRUE(saw_first_early) << received;
    EXPECT_TRUE(provider.released_before_last.load())
        << "the caller read the first event before the provider sent its last";
    EXPECT_NE(received.find("HTTP/1.1 200"), std::string::npos) << received;
    std::string head = received.substr(0, received.find("\r\n\r\n"));
    std::ranges::transform(
        head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    EXPECT_NE(head.find("transfer-encoding: chunked"), std::string::npos) << head;
    EXPECT_NE(head.find("content-type: text/event-stream"), std::string::npos) << head;
    EXPECT_NE(received.find("\"total_tokens\":42"), std::string::npos) << received;
    EXPECT_NE(received.find("data: [DONE]"), std::string::npos) << received;
    EXPECT_NE(provider.request_head.find("Bearer sk-fake-provider"), std::string::npos);

    // The usage of the last event is counted: stopping the server writes what it counted.
    server.stop();
    EXPECT_EQ(
        workspaces.records(home).metric_total("lake_usage", {{"dimension", "model_tokens"}}), 42);
}

// lake#939, over the real server and its outbound client: a caller that reads the first event of
// a stream and then drops its connection is sent nothing more, but lake reads the provider's stream
// to its end, and the usage in its last event is counted. Ending a stream early never makes it
// free.
TEST(ModelGatewayRoute, CountsAStreamItsCallerAbandoned) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const std::string customer = space(*store, "gateway-abandon");
    const std::string home = workspaces.create(customer, "home");
    const auto owner = store->get_customer(customer);
    ASSERT_TRUE(owner);
    lake::control::AgentTask row;
    row.id = lake::control::new_uuid_v4();
    row.customer_id = customer;
    row.agent = "agent-ask-orama";
    row.kind = "turn";
    row.workspace_id = home;
    row.scopes = {"workspace_query"};
    row.requested_by_user_id = owner->user_id;
    ASSERT_TRUE(store->create_agent_task(row));
    const std::string credential = std::string(lake::control::agent_task_credential_prefix) +
                                   row.id + ".0123456789abcdef0123456789abcdef";
    ASSERT_TRUE(store->claim_agent_task(
        row.id, row.agent, lake::control::identity_crypto::sha256_hex(credential)));

    StreamingProvider provider;
    // Enough events after the caller has gone for lake to find it gone before the last.
    provider.middle_events.store(25);
    lake::api::ApiServer server({.port = 0,
                                 .node_name = "gateway-abandon-test",
                                 .store = store,
                                 .workspaces = workspaces.service,
                                 .admin_token = "gateway-admin",
                                 .instance_kek = std::string(kek),
                                 .openai_api_key = "sk-fake-provider",
                                 .model_provider_url = provider.base_url()});
    ASSERT_TRUE(server.start()) << server.reason();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(server.port()));
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    timeval wait{20, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    const std::string body = chat(true)["request"].dump();
    const std::string sent = "POST /openai/v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                             "Authorization: Bearer " +
                             credential +
                             "\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\n"
                             "Connection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body;
    ASSERT_EQ(::send(fd, sent.data(), sent.size(), 0), static_cast<ssize_t>(sent.size()));
    std::string received;
    char buffer[4096];
    while (received.find("FIRST") == std::string::npos) {
        const auto got = ::recv(fd, buffer, sizeof(buffer), 0);
        ASSERT_GT(got, 0) << received;
        received.append(buffer, static_cast<std::size_t>(got));
    }
    // The caller goes at once (a reset, not a graceful close), and the provider goes on.
    const linger reset{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    ::close(fd);
    provider.release.store(true);

    // The request is over once lake counts it in its request metrics.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool finished = false;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        const auto metrics =
            lake::test::http_request({.url = "http://127.0.0.1:" + std::to_string(server.port()),
                                      .method = "GET",
                                      .suffix = "/metrics",
                                      .headers = {{"Authorization", "Bearer gateway-admin"}}});
        finished =
            metrics &&
            std::string(reinterpret_cast<const char*>(metrics->body.data()), metrics->body.size())
                    .find("route=\"/openai/v1/chat/completions\"") != std::string::npos;
        if (!finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    EXPECT_TRUE(finished);
    EXPECT_TRUE(provider.sent_last.load());

    // Stopping the server writes what it counted: the provider's own usage, from its last event.
    server.stop();
    EXPECT_EQ(workspaces.records(home).metric_total("lake_usage", {{"dimension", "model_tokens"}}),
              42);
}

} // namespace
