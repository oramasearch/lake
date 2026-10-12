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
#include <sstream>
#include <thread>
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

TEST(ModelCompletion, AuditsARelayedStreamWithoutUsageAsUnreported) {
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
    EXPECT_EQ(answered.value["usage_reported"], false);
    EXPECT_EQ(answered.value["recorded"], false);
    EXPECT_EQ(context.audit_details["usage_reported"], false);
    EXPECT_EQ(context.audit_details["input_tokens"], 0);
    EXPECT_EQ(context.audit_details["output_tokens"], 0);
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

// A model provider on loopback that streams a chat completion in chunked transfer: it sends its
// first event, then holds the last until `release` is set (or a deadline passes) and records
// whether it was released before it sent the last.
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
    std::atomic<bool> released_before_last{false};
    std::atomic<bool> sent_last{false};
    std::string request_head;

    static constexpr std::string_view first =
        "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"FIRST\"}}],\"usage\":null}\n\n";
    static constexpr std::string_view last =
        "data: {\"id\":\"c\",\"model\":\"gpt-5.5-2026-08-01\",\"choices\":[],\"usage\":"
        "{\"prompt_tokens\":40,\"completion_tokens\":2,\"total_tokens\":42}}\n\n"
        "data: [DONE]\n\n";

private:
    static void send_all(int fd, std::string_view bytes) {
        while (!bytes.empty()) {
            const auto sent = ::send(fd, bytes.data(), bytes.size(), 0);
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

} // namespace
