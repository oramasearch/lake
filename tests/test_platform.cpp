#include "audit_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include <boost/beast2/http_server.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/http.hpp>

#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/rest.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/platform.hpp"
#include "lake/control/workspace_audit.hpp"
#include "lake/control/zetta_control.hpp"
#include "repository_test_support.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using lake::control::PlatformResult;
using lake::control::ZettaControlResult;
using nlohmann::json;

class FakeZettaControl final : public lake::control::ZettaControl {
public:
    bool unavailable = false;
    std::vector<json> deployments;
    std::vector<json> environment_env;
    std::vector<json> domains;

    liquid::runtime::task<ZettaControlResult<lake::control::ZettaRows>>
    sql(std::string statement, std::string database) override {
        if (unavailable) {
            co_return ZettaControlResult<lake::control::ZettaRows>(std::unexpected(
                lake::control::ZettaControlError{"store_unavailable", "fake unavailable"}));
        }
        if (database != lake::control::operator_control_database) {
            co_return ZettaControlResult<lake::control::ZettaRows>(std::unexpected(
                lake::control::ZettaControlError{"invalid_input", "wrong database"}));
        }
        if (statement.find("environment_env") != std::string::npos) {
            co_return ZettaControlResult<lake::control::ZettaRows>(environment_env);
        }
        if (statement.find("deployments") != std::string::npos) {
            co_return ZettaControlResult<lake::control::ZettaRows>(deployments);
        }
        if (statement.find("domains") != std::string::npos) {
            co_return ZettaControlResult<lake::control::ZettaRows>(domains);
        }
        co_return ZettaControlResult<lake::control::ZettaRows>(
            std::unexpected(lake::control::ZettaControlError{"invalid_input", "unknown table"}));
    }

    liquid::runtime::task<ZettaControlResult<void>> append(std::string table, json row) override {
        if (unavailable) {
            co_return ZettaControlResult<void>(std::unexpected(
                lake::control::ZettaControlError{"store_unavailable", "fake unavailable"}));
        }
        table_rows(table).push_back(std::move(row));
        co_return ZettaControlResult<void>{};
    }

private:
    std::vector<json>& table_rows(const std::string& table) {
        if (table == lake::control::environment_env_table) {
            return environment_env;
        }
        if (table == lake::control::domains_table) {
            return domains;
        }
        return deployments;
    }
};

class FakeLoki final : public lake::control::LokiSource {
public:
    std::string requested_namespace;
    std::string requested_since;
    int requested_limit = 0;
    std::string requested_direction;

    liquid::runtime::task<PlatformResult<json>> query(std::string namespace_name,
                                                      std::string since,
                                                      int limit,
                                                      std::string direction) override {
        requested_namespace = std::move(namespace_name);
        requested_since = std::move(since);
        requested_limit = limit;
        requested_direction = std::move(direction);
        co_return PlatformResult<json>(json{
            {"items",
             json::array(
                 {{{"ts", "200"}, {"pod", "web-1"}, {"container", "app"}, {"line", "ready"}}})},
            {"next_since", "200"},
        });
    }
};

class LokiDouble {
public:
    LokiDouble() {
        namespace http = boost::http;
        http::router<http::route_params> router;
        router.add(
            http::method::get,
            "/loki/api/v1/query_range",
            [this](http::route_params& route) -> http::route_task {
                {
                    std::lock_guard lock(mutex_);
                    target_ = std::string(route.url.buffer());
                    authorization_ = route.req.count(http::field::authorization) == 0
                                         ? std::string()
                                         : std::string(route.req.at(http::field::authorization));
                }
                const std::string body =
                    json{{"status", "success"},
                         {"data",
                          {{"result",
                            json::array(
                                {{{"stream", {{"pod", "web-1"}, {"container", "app"}}},
                                  {"values", json::array({json::array({"300", "served"})})}}})}}}}
                        .dump();
                route.status(http::status::ok);
                route.res.set(http::field::content_type, "application/json");
                auto [error] = co_await route.send(body);
                if (error) {
                    co_return http::route_error(error);
                }
                co_return http::route_done;
            });
        server_ = std::make_unique<boost::beast2::http_server>(
            io_context_,
            1,
            std::move(router),
            http::make_parser_config(http::parser_config{true}),
            http::make_serializer_config(http::serializer_config()));
        const std::error_code error =
            server_->bind(boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), 0));
        if (error) {
            return;
        }
        port_ = server_->local_endpoint().port();
        server_->start();
        thread_ = std::thread([this] { io_context_.run(); });
    }

    ~LokiDouble() {
        if (server_) {
            server_->stop();
        }
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    [[nodiscard]] std::string target() const {
        std::lock_guard lock(mutex_);
        return target_;
    }

    [[nodiscard]] std::string authorization() const {
        std::lock_guard lock(mutex_);
        return authorization_;
    }

private:
    boost::corosio::io_context io_context_;
    std::unique_ptr<boost::beast2::http_server> server_;
    std::thread thread_;
    unsigned short port_ = 0;
    mutable std::mutex mutex_;
    std::string target_;
    std::string authorization_;
};

json deployment_row(const std::string& repository,
                    std::string phase,
                    std::string updated_at,
                    std::string ordering_token = {}) {
    return {{"operator", "operator-a"},
            {"lake", "lake.example"},
            {"repository", repository},
            {"environment", "production"},
            {"kind", "application"},
            {"change", "change-0123456789"},
            {"image", "registry.example/app@sha256:0123456789abcdef"},
            {"deployment_id", "deployment-1"},
            {"requested_by", "admin"},
            {"requested_at", "2026-09-06T00:00:00Z"},
            {"phase", std::move(phase)},
            {"namespace", "lake-production"},
            {"url", "https://app.example"},
            {"message", ""},
            {"updated_at", std::move(updated_at)},
            {"ordering_token", std::move(ordering_token)}};
}

json environment_env_row(const std::string& repository,
                         std::string key,
                         std::string value,
                         bool deleted,
                         std::string updated_at,
                         std::string ordering_token = {}) {
    return {{"lake", "lake.example"},
            {"repository", repository},
            {"environment", "production"},
            {"key", std::move(key)},
            {"value", std::move(value)},
            {"deleted", deleted},
            {"set_by", "admin"},
            {"updated_at", std::move(updated_at)},
            {"ordering_token", std::move(ordering_token)}};
}

constexpr std::string_view platform_kek =
    "706c6174666f726d2d746573742d6b656b2d706c6174666f726d2d746573742d";

// The repository's environments and mirror live in its workspace's Zetta (lake#358, lake#359):
// every fixture repository sits in a Ready workspace with its own record store.
class PlatformFixture : public testing::Test {
protected:
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces;
    std::shared_ptr<FakeZettaControl> control = std::make_shared<FakeZettaControl>();
    std::shared_ptr<FakeLoki> loki = std::make_shared<FakeLoki>();
    std::unique_ptr<lake::control::PlatformService> platform;
    lake::control::Repository repository;
    Registry registry;
    ApiContext context;

    void reset_platform() {
        store = std::make_shared<lake::control::InMemoryStore>();
        workspaces = std::make_unique<lake::test::RecordWorkspaces>(store);
        control = std::make_shared<FakeZettaControl>();
        loki = std::make_shared<FakeLoki>();
        const auto user = store->create_user({"admin", "admin@example.test", "Admin"});
        ASSERT_TRUE(user);
        const auto customer = store->create_customer({"individual", user->id, "", "admin"});
        ASSERT_TRUE(customer);
        const auto created = store->create_repository(
            {customer->id, "platform", "", "private", workspaces->create(customer->id, "home")});
        ASSERT_TRUE(created);
        repository = *created;
        platform = std::make_unique<lake::control::PlatformService>(
            store,
            control,
            loki,
            lake::control::PlatformConfig{
                .lake = "lake.example",
                .identity_seed = "",
                .instance_kek = std::string(platform_kek),
            });
        context.store = store.get();
        context.platform = platform.get();
        context.workspaces = workspaces->service.get();
    }

    void SetUp() override {
        lake::api::register_core_capabilities(registry, "test");
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.instance_kek = std::string(platform_kek);
        reset_platform();
        control->deployments.push_back(
            deployment_row(repository.id, "deploying", "2026-09-06T00:01:00Z"));
    }

    // What the server's owner loop hands the sync: the workspace caller.
    lake::control::PlatformRecordsOpener opener() {
        return [service = workspaces->service](const std::string& customer_id,
                                                const std::string& workspace_id) {
            return lake::control::WorkspaceRecords::open(
                [service, customer_id, workspace_id](std::string capability, bool read,
                                                     json input) {
                    return service->call(customer_id, workspace_id, std::move(capability), read,
                                         std::move(input));
                },
                workspace_id, platform_kek);
        };
    }

    PlatformResult<void> sync() {
        auto completed = liquid::runtime::block_on(platform->sync(opener()));
        EXPECT_TRUE(completed) << (completed ? "" : completed.error().message);
        if (!completed) {
            return std::unexpected(
                lake::control::PlatformError{"store_error", completed.error().message});
        }
        return std::move(*completed);
    }

    lake::control::WorkspaceRecords records() {
        auto opened =
            lake::api::workspace_records(context, repository.customer_id, repository.workspace_id);
        EXPECT_TRUE(opened) << (opened ? "" : opened.error().message);
        return std::move(*opened);
    }

    // The repository's workspace audit, in chain order (decision 52: a deployment's phases are
    // the repository's data). The chain is verified on every read.
    std::vector<lake::audit::Event> workspace_audit() {
        auto opened = records();
        auto listed = liquid::runtime::block_on(lake::control::list_workspace_audit(opened, {}, 0));
        EXPECT_TRUE(listed && *listed);
        if (!listed || !*listed) {
            return {};
        }
        auto found = std::move(**listed);
        std::ranges::sort(found, {}, &lake::control::WorkspaceAuditEvent::sequence);
        std::vector<lake::audit::Event> events;
        for (auto& event : found) {
            events.push_back(std::move(event.event));
        }
        const auto verified =
            liquid::runtime::block_on(lake::control::verify_workspace_audit(opened, *store));
        EXPECT_TRUE(verified && *verified && (*verified)->verified);
        return events;
    }

    template <class Task> PlatformResult<json> finish(Task task) {
        auto completed = liquid::runtime::block_on(std::move(task));
        if (!completed) {
            return std::unexpected(
                lake::control::PlatformError{"store_error", completed.error().message});
        }
        return std::move(*completed);
    }

    PlatformResult<json> list_environments() {
        auto opened = records();
        return finish(platform->list_environments(opened, repository.id));
    }

    PlatformResult<json> get_environment(const std::string& name) {
        auto opened = records();
        return finish(platform->get_environment(opened, repository.id, name));
    }

    PlatformResult<json> list_deployments(const std::string& name, int limit) {
        auto opened = records();
        return finish(platform->list_deployments(opened, repository.id, name, limit));
    }

    ApiResult call_async(const std::string& name, json input) {
        context.request_id.clear();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed) << (completed ? "" : completed.error().message);
        if (!completed) {
            return ApiResult::failure("store_error", completed.error().message);
        }
        return std::move(*completed);
    }
};

TEST(ZettaControlLiteral, QuotesApostrophesAndRejectsControlCharacters) {
    const auto quoted = lake::control::quote_zetta_literal("Lake's production");
    ASSERT_TRUE(quoted);
    EXPECT_EQ(*quoted, "'Lake''s production'");
    const auto rejected = lake::control::quote_zetta_literal(std::string("line\nvalue"));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "invalid_input");
}

TEST(MemoryStoreTimestamps, EncodeLexicographicallySortableMicroseconds) {
    const auto value = std::chrono::system_clock::time_point{} + std::chrono::seconds(1) +
                       std::chrono::microseconds(234567);
    const auto later = value + std::chrono::microseconds(1);
    EXPECT_EQ(lake::control::iso8601_at(value), "1970-01-01T00:00:01.234567Z");
    EXPECT_LT(lake::control::iso8601_at(value), lake::control::iso8601_at(later));
}

TEST(LokiHttpSource, UsesQueryRangeAndReturnsAStableCursor) {
    LokiDouble server;
    ASSERT_NE(server.endpoint(), "http://127.0.0.1:0");
    lake::control::install_zetta_control_transport();
    {
        lake::control::LokiHttpSource source(server.endpoint(), "logs-token");
        auto completed =
            liquid::runtime::block_on(source.query("lake-production", "250", 25, "forward"));
        ASSERT_TRUE(completed) << completed.error().message;
        ASSERT_TRUE(*completed) << completed->error().message;
        EXPECT_EQ(completed->value().at("items").at(0).at("line"), "served");
        EXPECT_EQ(completed->value().at("next_since"), "300");
    }
    lake::control::shutdown_zetta_control_transport();
    EXPECT_NE(server.target().find("/loki/api/v1/query_range?"), std::string::npos);
    EXPECT_NE(server.target().find("start=250"), std::string::npos);
    EXPECT_NE(server.target().find("limit=25"), std::string::npos);
    EXPECT_EQ(server.authorization(), "Bearer logs-token");
}

TEST_F(PlatformFixture, SyncCreatesEnvironmentLatestPhaseAndAuditEvent) {
    ASSERT_TRUE(sync());
    auto environments = list_environments();
    ASSERT_TRUE(environments);
    ASSERT_EQ(environments->at("items").size(), 1U);
    EXPECT_EQ(environments->at("items").at(0).at("phase"), "deploying");
    const std::string object_id = environments->at("items").at(0).at("object_id");
    const auto object = store->get_policy_object(object_id);
    ASSERT_TRUE(object);
    EXPECT_EQ(object->object_type, "environment");
    // The phase is in the repository's workspace audit, not the central log (decision 52).
    auto central = lake::test::audit_events(*store, {});
    ASSERT_TRUE(central);
    EXPECT_FALSE(std::ranges::any_of(*central, [](const lake::audit::Event& event) {
        return event.draft.kind == "platform.deployment.phase";
    }));
    auto events = workspace_audit();
    EXPECT_TRUE(std::ranges::any_of(events, [](const lake::audit::Event& event) {
        return event.draft.kind == "platform.deployment.phase";
    }));

    control->deployments.push_back(deployment_row(repository.id, "ready", "2026-09-06T00:02:00Z"));
    control->deployments.push_back(
        deployment_row(repository.id, "healthy", "2026-09-06T00:03:00Z"));
    ASSERT_TRUE(sync());
    const auto environment = get_environment("production");
    ASSERT_TRUE(environment);
    EXPECT_EQ(environment->at("deployment").at("phase"), "healthy");
    events = workspace_audit();
    EXPECT_EQ(std::ranges::count_if(events,
                                    [](const lake::audit::Event& event) {
                                        return event.draft.kind == "platform.deployment.phase";
                                    }),
              3);
}

TEST_F(PlatformFixture, EnvironmentValuesDomainsAndSecretScanningUseAppendOnlyRows) {
    control->environment_env.push_back(
        environment_env_row(repository.id, "COLOR", "blue", false, "2026-09-06T00:01:00Z"));
    control->environment_env.push_back(
        environment_env_row(repository.id, "COLOR", "", true, "2026-09-06T00:02:00Z"));
    ASSERT_TRUE(sync());
    auto environment = get_environment("production");
    ASSERT_TRUE(environment);
    EXPECT_TRUE(environment->at("env").empty());

    ApiResult set = call_async("set_environment_env",
                               {{"repository_id", repository.id},
                                {"environment", "production"},
                                {"key", "COLOR"},
                                {"value", "green"}});
    ASSERT_TRUE(set.ok) << set.error.message;
    ASSERT_EQ(control->environment_env.back().at("value"), "green");
    EXPECT_FALSE(control->environment_env.back().at("ordering_token").get<std::string>().empty());
    const std::string credential = std::string("AKIA") + "0123456789ABCDEF";
    ApiResult secret = call_async("set_environment_env",
                                  {{"repository_id", repository.id},
                                   {"environment", "production"},
                                   {"key", "AWS_ACCESS_KEY_ID"},
                                   {"value", credential}});
    EXPECT_FALSE(secret.ok);
    EXPECT_EQ(secret.error.message, "use a sealed secret");

    ApiResult domain = call_async("create_domain",
                                  {{"repository_id", repository.id},
                                   {"environment", "production"},
                                   {"host", "www.example.test"},
                                   {"tls", "acme"}});
    ASSERT_TRUE(domain.ok) << domain.error.message;
    EXPECT_FALSE(control->domains.back().at("ordering_token").get<std::string>().empty());
    environment = get_environment("production");
    ASSERT_TRUE(environment);
    ASSERT_EQ(environment->at("domains").size(), 1U);
    EXPECT_EQ(environment->at("domains").at(0).at("status"), "pending");

    json ready_domain = control->domains.back();
    ready_domain["status"] = "ready";
    ready_domain["updated_at"] = "2099-09-06T00:03:00Z";
    control->domains.push_back(std::move(ready_domain));
    ASSERT_TRUE(sync());
    environment = get_environment("production");
    ASSERT_TRUE(environment);
    EXPECT_EQ(environment->at("domains").at(0).at("status"), "ready");
}

TEST_F(PlatformFixture, SameInstantEnvironmentEditsRespectOrderingToken) {
    constexpr std::string_view instant = "2026-09-06T00:01:00.000000Z";
    control->environment_env = {
        environment_env_row(repository.id,
                            "SET_THEN_UNSET",
                            "green",
                            false,
                            std::string(instant),
                            "00000000-0000-4000-8000-000000000001"),
        environment_env_row(repository.id,
                            "SET_THEN_UNSET",
                            "",
                            true,
                            std::string(instant),
                            "00000000-0000-4000-8000-000000000002"),
        environment_env_row(repository.id,
                            "UNSET_THEN_SET",
                            "",
                            true,
                            std::string(instant),
                            "00000000-0000-4000-8000-000000000001"),
        environment_env_row(repository.id,
                            "UNSET_THEN_SET",
                            "blue",
                            false,
                            std::string(instant),
                            "00000000-0000-4000-8000-000000000002"),
    };

    ASSERT_TRUE(sync());
    const auto environment = get_environment("production");
    ASSERT_TRUE(environment);
    ASSERT_EQ(environment->at("env").size(), 1U);
    EXPECT_EQ(environment->at("env").at(0).at("key"), "UNSET_THEN_SET");
    EXPECT_EQ(environment->at("env").at(0).at("value"), "blue");
}

TEST_F(PlatformFixture, PlatformReducersProduceTheSameResultForEveryInputOrder) {
    constexpr std::string_view instant = "2026-09-06T00:01:00.000000Z";
    std::array<int, 3> order = {0, 1, 2};
    do {
        reset_platform();
        const std::vector<json> environment_rows = {
            environment_env_row(repository.id, "COLOR", "red", false, std::string(instant), "01"),
            environment_env_row(repository.id, "COLOR", "green", false, std::string(instant), "02"),
            environment_env_row(repository.id, "COLOR", "blue", false, std::string(instant), "03"),
        };
        const std::vector<json> deployment_rows = {
            deployment_row(repository.id, "pending", std::string(instant), "01"),
            deployment_row(repository.id, "deploying", std::string(instant), "02"),
            deployment_row(repository.id, "ready", std::string(instant), "03"),
        };
        for (const int index : order) {
            control->environment_env.push_back(environment_rows.at(index));
            control->deployments.push_back(deployment_rows.at(index));
        }
        ASSERT_TRUE(sync());
        const auto environment = get_environment("production");
        ASSERT_TRUE(environment);
        ASSERT_EQ(environment->at("env").size(), 1U);
        EXPECT_EQ(environment->at("env").at(0).at("value"), "blue");
        EXPECT_EQ(environment->at("deployment").at("phase"), "ready");

        const auto events = workspace_audit();
        std::vector<std::string> phases;
        for (const lake::audit::Event& event : events) {
            if (event.draft.kind == "platform.deployment.phase") {
                phases.push_back(event.draft.details.at("phase"));
            }
        }
        EXPECT_EQ(phases, (std::vector<std::string>{"pending", "deploying", "ready"}));
    } while (std::ranges::next_permutation(order).found);
}

TEST_F(PlatformFixture, LogsReturnLinesAndCursor) {
    ASSERT_TRUE(sync());
    ApiResult result = call_async("environment_logs",
                                  {{"repository_id", repository.id},
                                   {"environment", "production"},
                                   {"since", "100"},
                                   {"limit", 25},
                                   {"direction", "forward"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("items").at(0).at("line"), "ready");
    EXPECT_EQ(result.value.at("next_since"), "200");
    EXPECT_EQ(loki->requested_namespace, "lake-production");
    EXPECT_EQ(loki->requested_since, "100");
    EXPECT_EQ(loki->requested_limit, 25);
}

TEST_F(PlatformFixture, UnreachableControlServesStaleMirrorAndRefusesWrites) {
    ASSERT_TRUE(sync());
    control->unavailable = true;
    const auto failed = sync();
    ASSERT_FALSE(failed);
    EXPECT_TRUE(platform->degraded());
    const auto environment = get_environment("production");
    ASSERT_TRUE(environment);
    EXPECT_TRUE(environment->contains("stale_since"));
    ApiResult write = call_async("set_environment_env",
                                 {{"repository_id", repository.id},
                                  {"environment", "production"},
                                  {"key", "COLOR"},
                                  {"value", "green"}});
    EXPECT_FALSE(write.ok);
    EXPECT_EQ(write.error.code, "store_unavailable");
}

TEST_F(PlatformFixture, ReadRoleCannotWriteAndDeployRoleIsRequiredForDomains) {
    ASSERT_TRUE(sync());
    const auto environment = get_environment("production");
    ASSERT_TRUE(environment);
    ASSERT_TRUE(store->create_grant(
        {"reader", environment->at("object_id"), "read", "attestation", "admin", "read-grant"}));
    context.acting_identity = "reader";
    context.operator_credential = false;
    ApiResult read = call_async(
        "get_environment", {{"repository_id", repository.id}, {"environment", "production"}});
    EXPECT_TRUE(read.ok) << read.error.message;
    ApiResult set = call_async("set_environment_env",
                               {{"repository_id", repository.id},
                                {"environment", "production"},
                                {"key", "COLOR"},
                                {"value", "green"}});
    EXPECT_FALSE(set.ok);
    EXPECT_EQ(set.error.code, "unauthorized");
    ApiResult domain = call_async("create_domain",
                                  {{"repository_id", repository.id},
                                   {"environment", "production"},
                                   {"host", "www.example.test"},
                                   {"tls", "acme"}});
    EXPECT_FALSE(domain.ok);
    EXPECT_EQ(domain.error.code, "unauthorized");
    ASSERT_TRUE(store->create_grant({"deployer",
                                     environment->at("object_id"),
                                     "deploy",
                                     "attestation",
                                     "admin",
                                     "deploy-grant"}));
    context.acting_identity = "deployer";
    domain = call_async("create_domain",
                        {{"repository_id", repository.id},
                         {"environment", "production"},
                         {"host", "www.example.test"},
                         {"tls", "acme"}});
    EXPECT_TRUE(domain.ok) << domain.error.message;
}

// A lake without the platform mirror answers its reads and writes `not_supported` (501), which a
// caller does not retry, and says what an operator configures (lake#575).
TEST_F(PlatformFixture, WithoutThePlatformMirrorEveryCallSaysDeploymentsAreNotSetUp) {
    context.platform = nullptr;
    const std::vector<std::pair<std::string, json>> calls = {
        {"list_environments", {{"repository_id", repository.id}}},
        {"get_environment", {{"repository_id", repository.id}, {"environment", "production"}}},
        {"set_environment_env",
         {{"repository_id", repository.id},
          {"environment", "production"},
          {"key", "COLOR"},
          {"value", "green"}}},
    };
    for (const auto& [name, input] : calls) {
        const ApiResult result = call_async(name, input);
        EXPECT_FALSE(result.ok) << name;
        EXPECT_EQ(result.error.code, "not_supported") << name;
        EXPECT_EQ(lake::api::rest_status(result), 501) << name;
        EXPECT_NE(result.error.message.find("not set up on this Lake"), std::string::npos) << name;
        EXPECT_NE(result.error.message.find("an operator turns them on"), std::string::npos)
            << name;
        EXPECT_EQ(result.error.message.find("Zetta"), std::string::npos) << name;
    }
}

// `server_info` says whether Deployments are set up, so the web app asks once instead of every
// repository (lake#605).
TEST_F(PlatformFixture, ServerInfoSaysWhetherDeploymentsAreSetUp) {
    ApiResult with = call_async("server_info", json::object());
    ASSERT_TRUE(with.ok) << with.error.message;
    EXPECT_EQ(with.value.at("deployments"), true);
    context.platform = nullptr;
    ApiResult without = call_async("server_info", json::object());
    ASSERT_TRUE(without.ok) << without.error.message;
    EXPECT_EQ(without.value.at("deployments"), false);
}

TEST_F(PlatformFixture, DiscoveryExposesPlatformSurface) {
    ASSERT_TRUE(sync());
    const json openapi = lake::api::openapi_document(registry, "test");
    EXPECT_TRUE(openapi.at("paths").contains("/v1/get_environment"));
    EXPECT_TRUE(openapi.at("paths").contains("/v1/create_domain"));
    ApiContext mcp_context = context;
    const auto response = lake::api::mcp_dispatch(
        registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, mcp_context, "test");
    ASSERT_TRUE(response);
    EXPECT_NE(response->find("environment_logs"), std::string::npos);
    EXPECT_NE(response->find("set_environment_env"), std::string::npos);
}

} // namespace
