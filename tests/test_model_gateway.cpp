// The model gateway (lake#248, docs/api/model-gateway.md): every model call an agent makes for a
// workspace goes through lake's `model_completion`, which counts the tokens the provider reports as
// the workspace's `model_tokens`, refuses calls past the account's monthly limit, and keeps the
// prompt out of the audit. A fake provider stands in for OpenAI: tests never call it.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/model_gateway.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "one_thread_io.hpp"
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

// A provider that answers what it is told and remembers every request.
struct FakeProvider {
    std::mutex mutex;
    std::vector<ModelProviderRequest> requests;
    ModelProviderResponse answer{200, chat_answer.dump(), {}};

    lake::api::ModelProvider provider() {
        return [this](const ModelProviderRequest& request) {
            std::lock_guard lock(mutex);
            requests.push_back(request);
            return answer;
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
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::string customer = space(*store, "gateway-" + lake::control::new_uuid_v4().substr(0, 8));
    std::string home = workspaces.create(customer, "home");
    std::string lab = workspaces.create(customer, "lab");
    lake::api::UsageRecorder recorder{store.get(), workspaces.service.get()};
    lake::api::Registry registry;
    FakeProvider fake;

    GatewayRig() {
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
        << "an answer that does not stream is unchanged";

    const auto streamed = prepare_model_request(
        "chat/completions",
        {{"model", "gpt-5.5"}, {"stream", true}, {"stream_options", {{"other", 1}}}});
    ASSERT_TRUE(streamed);
    EXPECT_EQ((*streamed)["stream_options"]["include_usage"], true);
    EXPECT_EQ((*streamed)["stream_options"]["other"], 1) << "the caller's own options are kept";

    // The Responses API reports usage in its last event on its own.
    const auto responses =
        prepare_model_request("responses", {{"model", "gpt-5.5"}, {"stream", true}});
    ASSERT_TRUE(responses);
    EXPECT_FALSE(responses->contains("stream_options"));
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
        "sk-platform", "http://127.0.0.1:9/v1/", [&](const std::string& to, std::string sent,
                                                     auto sent_headers, std::chrono::seconds) {
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
                                   {"model", "gpt-5.5-2026-08-01"}}),
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
    const auto [status, answer] =
        post("/openai/v1/chat/completions", credential, chat()["request"]);
    EXPECT_EQ(status, 200U) << answer.dump();
    EXPECT_EQ(answer["object"], "chat.completion");
    EXPECT_EQ(answer["usage"]["prompt_tokens"], 1200);
    EXPECT_EQ(fake.calls(), 1U);

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
    server.stop();
}

} // namespace
