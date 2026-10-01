#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "stream_fixture.hpp"

namespace {

using nlohmann::json;

lake::control::ClerkIdentity build_identity(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity("build-test-kek");
    return lake::control::ClerkIdentity{subject,
                                        subject + "@example.test",
                                        "",
                                        "browser",
                                        identity->taap_id,
                                        identity->public_key,
                                        identity->wrapped_seed};
}

struct Builds {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::control::Principal owner;
    lake::control::Principal other;
    std::string customer_id;
    std::string customer_name;
    std::string repository_id;
    std::string apps_runtime_image = "oramasearch/app-runtime:20260930";
    std::unique_ptr<lake::test::StreamWorkspace> workspace;
    lake::api::Registry registry;
    std::shared_ptr<lake::control::MemoryAgentBus> raw_bus =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> bus =
        std::make_shared<lake::control::AgentTaskBus>(store, raw_bus, std::vector<std::string>{});

    Builds() {
        const auto owner_session = store->sign_in_clerk(build_identity("build-owner"));
        const auto other_session = store->sign_in_clerk(build_identity("build-other"));
        EXPECT_TRUE(owner_session);
        EXPECT_TRUE(other_session);
        owner = *store->context_for(owner_session->secret);
        other = *store->context_for(other_session->secret);
        const auto customers = store->list_customers(2, owner.user_id);
        EXPECT_TRUE(customers);
        customer_id = customers->front().id;
        customer_name = customers->front().name;
        workspace = std::make_unique<lake::test::StreamWorkspace>(store, customer_id);
        const auto repository = store->create_repository({customer_id, "source", "", "private"});
        EXPECT_TRUE(repository);
        repository_id = repository->id;
        lake::api::register_core_capabilities(registry, "test");
    }

    lake::api::ApiResult
    call(const lake::control::Principal& principal, const std::string& name, json input) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.agent_bus = bus.get();
        context.workspaces = workspace->service.get();
        context.internal_url = "http://lake-prod.prod.svc:80";
        context.apps_runtime_image = apps_runtime_image;
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.credential_session_id = principal.session_id;
        context.credential_kind = principal.kind;
        context.credential_workload_id = principal.workload_id;
        context.credential_customer_id = principal.workload_customer_id;
        context.credential_repository_id = principal.workload_repository_id;
        context.credential_revision = principal.workload_revision;
        context.credential_token_hash = principal.workload_token_hash;
        context.pool_id = principal.pool_id;
        context.scopes = principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        if (registry.find(name)->input_schema["properties"].contains("customer_id") &&
            principal.kind != "build_events" && !input.contains("customer_id")) {
            input["customer_id"] = customer_id;
        }
        if (registry.find(name)->input_schema["properties"].contains("workspace_id") &&
            !input.contains("workspace_id")) {
            input["workspace_id"] = workspace->id;
        }
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return std::move(*completed);
    }

    lake::api::ApiResult call(const std::string& name, json input = json::object()) {
        return call(owner, name, std::move(input));
    }

    lake::api::ApiResult create(std::string target = "service:v1") {
        return call("build_image",
                    {{"repository", repository_id},
                     {"revision", "main"},
                     {"context", "services/api"},
                     {"dockerfile", "services/api/Dockerfile"},
                     {"target", std::move(target)},
                     {"build_args", {{"PYTHON_VERSION", "3.13"}}}});
    }
};

} // namespace

TEST(Builds, DesiredRowStatusAndCancellationFollowTheOperatorContract) {
    Builds builds;
    const auto created = builds.create();
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("build_id");
    const auto row = builds.store->get_build(id);
    ASSERT_TRUE(row) << row.error().message;
    EXPECT_EQ(row->target, "lake-prod.prod.svc:80/" + builds.customer_name + "/service:v1");
    EXPECT_EQ(row->desired, "queued");
    EXPECT_EQ(row->phase, "Queued");
    EXPECT_EQ(row->build_args, json({{"PYTHON_VERSION", "3.13"}}));

    ASSERT_TRUE(
        builds.store->set_build_status(id, {.phase = "Running", .message = "builder running"}));
    const auto observed = builds.call("get_build", {{"build_id", id}});
    ASSERT_TRUE(observed.ok) << observed.error.message;
    EXPECT_EQ(observed.value["phase"], "Running");
    EXPECT_EQ(observed.value["message"], "builder running");

    const auto listed = builds.call("list_builds");
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["builds"].size(), 1U);
    EXPECT_EQ(listed.value["builds"][0]["id"], id);
    EXPECT_EQ(
        builds.call(builds.other, "list_builds", {{"customer_id", builds.customer_id}}).error.code,
        "unauthorized");
    EXPECT_EQ(builds
                  .call(builds.other,
                        "get_build",
                        {{"customer_id", builds.customer_id}, {"build_id", id}})
                  .error.code,
              "unauthorized");

    const auto cancelled = builds.call("cancel_build", {{"build_id", id}});
    ASSERT_TRUE(cancelled.ok) << cancelled.error.message;
    EXPECT_EQ(cancelled.value["desired"], "cancelled");
    EXPECT_EQ(builds.call("cancel_build", {{"build_id", id}}).error.code, "conflict");
    ASSERT_TRUE(builds.store->set_build_status(id, {.phase = "Cancelled", .message = "cancelled"}));
    EXPECT_EQ(builds.store->get_build(id)->phase, "Cancelled");
}

TEST(Builds, OperatorTokensArePurposeAndBuildScopedAndEventsAreRetained) {
    Builds builds;
    const auto first = builds.create();
    const auto second = builds.create("worker:v2");
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    const std::string first_id = first.value.at("build_id");
    const std::string second_id = second.value.at("build_id");
    const json hashes{{"source", lake::control::identity_crypto::sha256_hex("build-source")},
                      {"push", lake::control::identity_crypto::sha256_hex("build-push")},
                      {"events", lake::control::identity_crypto::sha256_hex("build-events")}};
    ASSERT_TRUE(builds.store->set_build_status(
        first_id, {.phase = "Running", .message = "running", .token_hashes = hashes}));
    for (const std::string purpose : {"source", "push", "events"}) {
        const auto principal = builds.store->context_for("build-" + purpose);
        ASSERT_TRUE(principal) << principal.error().message;
        EXPECT_EQ(principal->kind, "build_" + purpose);
        EXPECT_EQ(principal->workload_id, first_id);
        EXPECT_EQ(principal->workload_customer_id, builds.customer_id);
    }

    const auto source = *builds.store->context_for("build-source");
    EXPECT_EQ(
        builds.call(source, "repository_tree", {{"id", builds.repository_id}, {"ref", "other"}})
            .error.code,
        "unauthorized");
    EXPECT_EQ(
        builds.call(source, "repository_tree", {{"id", builds.repository_id}, {"ref", "main"}})
            .error.code,
        "store_unavailable");
    EXPECT_EQ(
        builds
            .call(source,
                  "repository_tree",
                  {{"repository", builds.repository_id}, {"revision", "main"}, {"recursive", true}})
            .error.code,
        "store_unavailable");
    EXPECT_EQ(builds.call(source, "get_build", {{"build_id", first_id}}).error.code,
              "unauthorized");

    const auto events = *builds.store->context_for("build-events");
    const auto published = builds.call(events,
                                       "publish_build_event",
                                       {{"build_id", first_id},
                                        {"type", "log"},
                                        {"phase", "building"},
                                        {"lines", json::array({"step one", "step two"})}});
    ASSERT_TRUE(published.ok) << published.error.message;
    EXPECT_EQ(
        builds.call(events, "publish_build_event", {{"build_id", second_id}, {"type", "phase"}})
            .error.code,
        "unauthorized");
    const auto received = builds.call("receive_build_events", {{"build_id", first_id}});
    ASSERT_TRUE(received.ok) << received.error.message;
    ASSERT_EQ(received.value["events"].size(), 1U);
    EXPECT_EQ(received.value["events"][0]["sequence"], 1);
    EXPECT_EQ(received.value["events"][0]["principal"], "build:" + first_id + ":events");
    const auto logs = builds.call("build_logs", {{"build_id", first_id}});
    ASSERT_TRUE(logs.ok) << logs.error.message;
    EXPECT_EQ(logs.value["lines"], json::array({"step one", "step two"}));

    ASSERT_TRUE(builds.store->set_build_status(first_id,
                                               {.phase = "Succeeded",
                                                .digest = "sha256:" + std::string(64, 'a'),
                                                .size = 42,
                                                .message = "image pushed"}));
    EXPECT_FALSE(builds.store->context_for("build-source"));
    EXPECT_FALSE(builds.store->context_for("build-push"));
    EXPECT_FALSE(builds.store->context_for("build-events"));
}

TEST(Builds, AppBuildGeneratesAnAppRuntimeDockerfileAndAllowedDependencies) {
    Builds builds;
    lake::control::NewWebApp app;
    app.customer_id = builds.customer_id;
    app.workspace_id = builds.workspace->id;
    app.name = "reports";
    app.mode = "runtime";
    app.source_repository = builds.repository_id;
    app.source_revision = "release";
    app.source_path = "web";
    app.framework = "python";
    app.port = 8080;
    app.host = "reports.apps.example.test";
    app.env = {{"APP_DEPS", "fastapi uvicorn"}};
    const auto stored = builds.store->create_web_app(app);
    ASSERT_TRUE(stored) << stored.error().message;

    builds.apps_runtime_image.clear();
    const auto unavailable = builds.call("build_app", {{"app", stored->id}});
    EXPECT_FALSE(unavailable.ok);
    EXPECT_EQ(unavailable.error.code, "store_unavailable");
    EXPECT_EQ(unavailable.error.message, "LAKE_APPS_RUNTIME_IMAGE is not configured");

    builds.apps_runtime_image = "oramasearch/app-runtime:20260930";
    const auto queued = builds.call("build_app", {{"app", stored->id}});
    ASSERT_TRUE(queued.ok) << queued.error.message;
    const auto build = builds.store->get_build(queued.value.at("build_id"));
    ASSERT_TRUE(build) << build.error().message;
    EXPECT_EQ(build->repository, builds.repository_id);
    EXPECT_EQ(build->revision, "release");
    EXPECT_EQ(build->context, "web");
    EXPECT_TRUE(
        build->dockerfile.starts_with("inline:ARG APP_RUNTIME_IMAGE\nFROM ${APP_RUNTIME_IMAGE}\n"));
    EXPECT_EQ(build->build_args,
              json({{"APP_DEPS", "fastapi uvicorn"},
                    {"APP_RUNTIME_IMAGE", "oramasearch/app-runtime:20260930"}}));
    EXPECT_TRUE(build->target.contains("/" + builds.customer_name + "/reports:build-"));
}

TEST(Builds, InputsRefuseForeignRepositoriesUnsafePathsAndDisallowedArguments) {
    Builds builds;
    const auto foreign = builds.store->list_customers(2, builds.other.user_id);
    ASSERT_TRUE(foreign);
    const auto repository =
        builds.store->create_repository({foreign->front().id, "foreign", "", "private"});
    ASSERT_TRUE(repository);
    EXPECT_EQ(
        builds
            .call("build_image",
                  {{"repository", repository->id}, {"revision", "main"}, {"target", "service:v1"}})
            .error.code,
        "not_found");
    EXPECT_EQ(builds
                  .call("build_image",
                        {{"repository", builds.repository_id},
                         {"revision", "main"},
                         {"context", "../outside"},
                         {"target", "service:v1"}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(builds
                  .call("build_image",
                        {{"repository", builds.repository_id},
                         {"revision", "main"},
                         {"target", "space/service:v1"}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(builds
                  .call("build_image",
                        {{"repository", builds.repository_id},
                         {"revision", "main"},
                         {"target", "service:v1"},
                         {"build_args", {{"SECRET", "no"}}}})
                  .error.code,
              "invalid_input");
}
