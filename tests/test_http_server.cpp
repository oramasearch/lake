#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <functional>

#include "lake/control/identity_crypto.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <liquid/identity.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <pqxx/pqxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "lake/api/server.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"

namespace {

class StderrPipe {
public:
    StderrPipe() {
        if (::pipe(pipe_) != 0) {
            return;
        }
        original_ = ::dup(STDERR_FILENO);
        if (original_ < 0) {
            close_pipe();
            return;
        }
        std::fflush(stderr);
        std::cerr.flush();
        if (::dup2(pipe_[1], STDERR_FILENO) < 0) {
            ::close(original_);
            original_ = -1;
            close_pipe();
            return;
        }
        ::close(pipe_[1]);
        pipe_[1] = -1;
        capturing_ = true;
    }

    ~StderrPipe() {
        restore();
        close_pipe();
    }

    [[nodiscard]] bool ready() const {
        return capturing_;
    }

    std::string finish() {
        restore();
        std::string output;
        char buffer[256];
        for (;;) {
            const ssize_t count = ::read(pipe_[0], buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
        }
        ::close(pipe_[0]);
        pipe_[0] = -1;
        return output;
    }

private:
    void restore() {
        if (!capturing_) {
            return;
        }
        std::fflush(stderr);
        std::cerr.flush();
        static_cast<void>(::dup2(original_, STDERR_FILENO));
        ::close(original_);
        original_ = -1;
        capturing_ = false;
    }

    void close_pipe() {
        for (int& descriptor : pipe_) {
            if (descriptor >= 0) {
                ::close(descriptor);
                descriptor = -1;
            }
        }
    }

    int pipe_[2]{-1, -1};
    int original_{-1};
    bool capturing_{false};
};

int connect_loopback(unsigned short port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    timeval timeout{};
    timeout.tv_sec = 5;
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return -1;
    }
    return socket_fd;
}

bool send_all(int socket_fd, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t count = ::send(socket_fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::string read_response(int socket_fd, bool wait_for_close) {
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
        if (wait_for_close) {
            continue;
        }
        const std::size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            continue;
        }
        if (response.starts_with("HTTP/1.0 204") || response.starts_with("HTTP/1.1 204")) {
            return response.substr(0, header_end + 4);
        }
        std::string headers = response.substr(0, header_end);
        std::ranges::transform(headers, headers.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        constexpr std::string_view prefix = "\r\ncontent-length: ";
        const std::size_t length_start = headers.find(prefix);
        if (length_start == std::string::npos) {
            return {};
        }
        std::size_t length_end = headers.find("\r\n", length_start + prefix.size());
        if (length_end == std::string::npos) {
            length_end = headers.size();
        }
        const char* first = headers.data() + length_start + prefix.size();
        const char* last = headers.data() + length_end;
        std::size_t body_size = 0;
        const auto parsed = std::from_chars(first, last, body_size);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return {};
        }
        const std::size_t expected_size = header_end + 4 + body_size;
        if (response.size() >= expected_size) {
            response.resize(expected_size);
            return response;
        }
    }
    return wait_for_close ? response : std::string{};
}

std::string response_header(const std::string& response, std::string_view name) {
    const auto header_end = response.find("\r\n\r\n");
    std::string headers = response.substr(0, header_end);
    std::ranges::transform(headers, headers.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const auto start = headers.find(prefix);
    if (start == std::string::npos) {
        return {};
    }
    const auto value_start = start + prefix.size();
    const auto end = headers.find("\r\n", value_start);
    return response.substr(value_start,
                           end == std::string::npos ? header_end - value_start : end - value_start);
}

class HttpConnection {
public:
    explicit HttpConnection(unsigned short port) : descriptor_(connect_loopback(port)) {}

    ~HttpConnection() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    std::string request(std::string_view method,
                        std::string_view path,
                        std::string_view body = {},
                        std::string_view cookie = {},
                        std::string_view version = "HTTP/1.1",
                        std::string_view bearer = {},
                        std::string_view content_type = "application/x-www-form-urlencoded") {
        std::string wire = std::string(method) + " " + std::string(path) + " " +
                           std::string(version) + "\r\nHost: localhost\r\n";
        wire += "Connection: keep-alive\r\nContent-Type: " + std::string(content_type) + "\r\n";
        if (!bearer.empty()) {
            wire += "Authorization: Bearer " + std::string(bearer) + "\r\n";
        }
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        if (!cookie.empty()) {
            wire += "Cookie: " + std::string(cookie) + "\r\n";
        }
        wire += "\r\n";
        wire += body;
        if (descriptor_ < 0 || !send_all(descriptor_, wire)) {
            return {};
        }
        return read_response(descriptor_, false);
    }

private:
    int descriptor_;
};

// A workspace cluster that answers from a table, recording what reached it.
class RecordingWorkspace final : public lake::control::WorkspaceCaller {
public:
    std::vector<std::tuple<std::string, bool, nlohmann::json>> calls;
    std::map<std::string, nlohmann::json> answers;

    liquid::runtime::task<lake::control::ZettaControlResult<nlohmann::json>>
    call(std::string, std::string, std::string capability, bool read, nlohmann::json input)
        override {
        calls.emplace_back(capability, read, input);
        const auto answer = answers.find(capability);
        co_return lake::control::ZettaControlResult<nlohmann::json>(
            answer == answers.end() ? nlohmann::json::object() : answer->second);
    }
};

// An agent service that answers one scripted turn, recording what it was sent.
class ScriptedAgents final : public lake::control::AgentCaller {
public:
    std::vector<nlohmann::json> turns;
    nlohmann::json answer = {{"ok", true}, {"reply", "hello"}};

    liquid::runtime::task<lake::control::ZettaControlResult<nlohmann::json>>
    turn(std::string, std::string, nlohmann::json body) override {
        turns.push_back(std::move(body));
        co_return lake::control::ZettaControlResult<nlohmann::json>(answer);
    }
};

// Parameters select the production Postgres store and a fresh client connection.
class HttpResponseIsolation : public testing::TestWithParam<std::tuple<bool, bool>> {
protected:
    void SetUp() override {
        if (std::get<0>(GetParam())) {
            const char* uri = lake::test::postgres_uri();
            if (uri == nullptr || *uri == '\0') {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            postgres_uri_ = uri;
            schema_ = "http_response_" + lake::control::new_uuid_v4();
            std::erase(schema_, '-');
            pqxx::connection connection(postgres_uri_);
            pqxx::work tx(connection);
            tx.exec("SET client_min_messages TO warning");
            tx.exec("CREATE SCHEMA " + tx.quote_name(schema_));
            tx.exec("SET search_path TO " + tx.quote_name(schema_));
            std::ifstream schema_file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) /
                                      "db/schema.sql");
            const std::string schema_sql((std::istreambuf_iterator<char>(schema_file)), {});
            ASSERT_FALSE(schema_sql.empty());
            tx.exec(schema_sql);
            tx.commit();
            database_uri_ = postgres_uri_;
            database_uri_ += " options='-csearch_path=" + schema_ + "'";
            store_ = lake::control::make_postgres_store(database_uri_, instance_kek_);
            create_identity_document();
            ASSERT_FALSE(identity_document_.empty());
        } else {
            store_ = std::make_shared<lake::control::InMemoryStore>();
            identity_document_ = "{}";
        }
        email_ = "response-isolation-" + lake::control::new_uuid_v4() + "@example.test";
        const auto account = store_->sign_up_password(
            {"lake", email_, password_, "Isolation test", "fixture", identity_document_});
        ASSERT_TRUE(account.has_value());
        lake::api::ServerOptions options;
        options.port = 0;
        // One worker deterministically reuses its response even after a socket closes.
        options.worker_coroutines = 1;
        options.store = store_;
        options.admin_token = "front-door-operator";
        options.instance_kek = instance_kek_;
        options.store_root =
            lake::test::fixture_path("http-store-" + lake::control::new_uuid_v4()).string();
        options.metering.storage_quota_bytes = 1ULL << 30;
        options.metering.storage_gib_hour_micros = 1000;
        options.metering.transfer_in_gib_micros = 1000;
        options.metering.transfer_out_gib_micros = 1000;
        options.metering.compute_second_micros = 1000;
        workspace_dir_ = lake::test::fixture_path("http-services-" + lake::control::new_uuid_v4());
        std::filesystem::create_directories(workspace_dir_);
        lake::control::WorkspaceConfig workspace_config{workspace_dir_.string(), 0};
        workspace_config.agent_endpoints = {{"agent-ask-orama", "http://ask:8080"},
                                            {"agent-project", "http://project:8080"},
                                            {"agent-autosuggest", "http://suggest:8080"},
                                            {"agent-creator", "http://creator:8080"}};
        workspace_config.internal_url = "http://lake.internal";
        workspace_config.agent_service_token = "the-service-token";
        for (const auto& [agent, endpoint] : workspace_config.agent_endpoints) {
            workspace_config.agent_bus_tokens[agent] = "bus-token-" + agent;
        }
        std::vector<std::string> bus_agents;
        for (const auto& [agent, endpoint] : workspace_config.agent_endpoints) {
            bus_agents.push_back(agent);
        }
        workspaces_ = std::make_shared<lake::control::WorkspaceService>(
            workspace_config, store_, workspace_caller_);
        options.workspaces = workspaces_;
        options.agent_caller = agents_;
        agent_bus_ = std::make_shared<lake::control::AgentTaskBus>(store_, bus_, bus_agents);
        ASSERT_TRUE(liquid::runtime::block_on(agent_bus_->start()).value());
        options.agent_bus = agent_bus_;
        server_ = std::make_unique<lake::api::ApiServer>(options);
        ASSERT_TRUE(server_->start()) << server_->reason();
    }

    void TearDown() override {
        server_.reset();
        store_.reset();
        if (schema_.empty()) {
            return;
        }
        pqxx::connection connection(postgres_uri_);
        pqxx::work tx(connection);
        tx.exec("SET client_min_messages TO warning");
        tx.exec("DROP SCHEMA " + tx.quote_name(schema_) + " CASCADE");
        tx.commit();
    }

    // A library agent's part of a background turn (docs/api/agent-bus.md), done here through
    // lake's own capabilities with the agent service token: the task is received from the agent's
    // topic, claimed (its body arrives with the task's credential in place of "{{credential}}"),
    // handed to the scripted agent, and its answer published on agent-events. Lake's bus loop then
    // applies it. Answers the credential the agent was handed.
    std::string run_agent_task(HttpConnection& connection, const std::string& task_id) {
        const auto task = store_->get_agent_task(task_id);
        EXPECT_TRUE(task) << task_id;
        if (!task) {
            return {};
        }
        const auto call = [&](std::string_view method, const std::string& path,
                              const nlohmann::json& body) {
            const std::string response =
                connection.request(method, path, method == "GET" ? "" : body.dump(), "",
                                    "HTTP/1.1", "bus-token-" + task->agent, "application/json");
            const auto at = response.find("\r\n\r\n");
            return nlohmann::json::parse(
                at == std::string::npos ? std::string() : response.substr(at + 4), nullptr, false);
        };
        const std::string shared = connection.request(
            "GET", "/v1/receive_agent_tasks?agent=" + task->agent, "", "", "HTTP/1.1",
            "the-service-token");
        EXPECT_NE(shared.find("\"unauthorized\""), std::string::npos)
            << "the service token user-created agents also hold never reaches the bus";
        const nlohmann::json inbox =
            call("GET", "/v1/receive_agent_tasks?agent=" + task->agent, nullptr);
        EXPECT_EQ(inbox["ok"], true) << inbox.dump();
        bool listed = false;
        for (const auto& row : inbox["value"]["tasks"]) {
            listed = listed || row["task_id"] == task_id;
        }
        EXPECT_TRUE(listed) << "the task is on " << task->agent << "'s topic: " << inbox.dump();
        const nlohmann::json claimed =
            call("POST", "/v1/claim_agent_task", {{"task_id", task_id}, {"agent", task->agent}});
        EXPECT_EQ(claimed["ok"], true) << claimed.dump();
        const nlohmann::json body = claimed["value"]["body"];
        agents_->turns.push_back(body);
        const nlohmann::json published = call("POST",
                                              "/v1/publish_agent_event",
                                              {{"task_id", task_id},
                                               {"agent", task->agent},
                                               {"type", "result"},
                                               {"result", agents_->answer}});
        EXPECT_EQ(published["ok"], true) << published.dump();
        EXPECT_TRUE(liquid::runtime::block_on(agent_bus_->pump(std::chrono::milliseconds(0))));
        return body.value("lake_token", std::string());
    }

    static std::string task_of(const std::string& response) {
        const std::string location = response_header(response, "location");
        const auto at = location.find("task=");
        return at == std::string::npos ? std::string()
                                       : location.substr(at + 5, location.find('&', at) - at - 5);
    }

    void create_identity_document() {
        const auto home =
            lake::test::fixture_path("response-identity-" + lake::control::new_uuid_v4());
        const char* previous = std::getenv("LIQUID_HOME");
        const bool had_previous_home = previous != nullptr;
        const std::string previous_home = previous == nullptr ? "" : previous;
        ASSERT_EQ(setenv("LIQUID_HOME", home.c_str(), 1), 0);
        auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
        if (!had_previous_home) {
            unsetenv("LIQUID_HOME");
        } else {
            setenv("LIQUID_HOME", previous_home.c_str(), 1);
        }
        ASSERT_TRUE(signer.has_value());
        const auto path = home / "identity" / (signer->identity().fingerprint + ".identity.cbor");
        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input);
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        identity_document_.resize(((bytes.size() + 2) / 3) * 4);
        const int encoded_size =
            EVP_EncodeBlock(reinterpret_cast<unsigned char*>(identity_document_.data()),
                            reinterpret_cast<const unsigned char*>(bytes.data()),
                            static_cast<int>(bytes.size()));
        ASSERT_EQ(encoded_size, identity_document_.size());
        while (identity_document_.ends_with('=')) {
            identity_document_.pop_back();
        }
        std::replace(identity_document_.begin(), identity_document_.end(), '+', '-');
        std::replace(identity_document_.begin(), identity_document_.end(), '/', '_');
    }

    std::unique_ptr<HttpConnection> connect() {
        return std::make_unique<HttpConnection>(server_->port());
    }

    std::shared_ptr<lake::control::Store> store_;
    std::shared_ptr<RecordingWorkspace> workspace_caller_ = std::make_shared<RecordingWorkspace>();
    std::shared_ptr<ScriptedAgents> agents_ = std::make_shared<ScriptedAgents>();
    std::shared_ptr<lake::control::MemoryAgentBus> bus_ =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> agent_bus_;
    std::shared_ptr<lake::control::WorkspaceService> workspaces_;
    std::filesystem::path workspace_dir_;
    std::unique_ptr<lake::api::ApiServer> server_;
    std::string postgres_uri_;
    std::string database_uri_;
    std::string schema_;
    std::string email_;
    std::string identity_document_;
    const std::string password_ = "correct horse battery staple";
    const std::string instance_kek_ =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
};

TEST_P(HttpResponseIsolation, FreshSignUpOwnsFirstRepositoryWithZeroInitialMetering) {
    if (std::get<0>(GetParam())) {
        create_identity_document();
    }
    const std::string email = "first-repository-" + lake::control::new_uuid_v4() + "@example.test";
    auto client = connect();
    const std::string signup =
        client->request("POST",
                        "/identity/sign-up",
                        "email=" + email + "&display_name=Newcomer&password=" + password_ +
                            "&device_name=browser&identity_document=" + identity_document_);
    ASSERT_TRUE(signup.starts_with("HTTP/1.1 302")) << signup;
    EXPECT_EQ(response_header(signup, "location"), "/repositories");
    const std::string cookie = response_header(signup, "set-cookie")
                                   .substr(0, response_header(signup, "set-cookie").find(';'));
    ASSERT_FALSE(cookie.empty());
    const std::string page = client->request("GET", "/repositories", "", cookie);
    EXPECT_NE(page.find("action=\"/repository/create\""), std::string::npos);
    const std::string customers_response = client->request("GET", "/v1/list_customers", "", cookie);
    const auto customers =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4));
    ASSERT_TRUE(customers.at("ok"));
    ASSERT_EQ(customers.at("value").at("items").size(), 1U);
    const std::string customer_id = customers.at("value").at("items").at(0).at("id");
    const std::string usage_response =
        client->request("GET", "/v1/metering_usage?customer_id=" + customer_id, "", cookie);
    const auto usage =
        nlohmann::json::parse(usage_response.substr(usage_response.find("\r\n\r\n") + 4));
    ASSERT_TRUE(usage.at("ok")) << usage.dump();
    EXPECT_EQ(usage.at("value").at("cost_micros"), 0);
    EXPECT_EQ(usage.at("value").at("quota_bytes"), 1ULL << 30);
    ASSERT_EQ(usage.at("value").at("items").size(), 1U);
    EXPECT_EQ(usage.at("value").at("items").at(0).at("stored_bytes"), 0);
    EXPECT_FALSE(usage.at("value").at("over_quota"));
    const std::string created = client->request("POST",
                                                "/repository/create",
                                                "customer_id=" + customer_id +
                                                    "&name=first-repository&description=My+files",
                                                cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 302")) << created;
    const std::string destination = response_header(created, "location");
    ASSERT_TRUE(destination.starts_with("/repository?id="));
    const std::string repository = client->request("GET", destination, "", cookie);
    EXPECT_TRUE(repository.starts_with("HTTP/1.1 200")) << repository;
    EXPECT_NE(repository.find("/first-repository"), std::string::npos);
    const std::string other_cookie = "lake_session=invalid";
    const std::string denied = client->request("POST",
                                               "/repository/create",
                                               "customer_id=" + customer_id + "&name=forbidden",
                                               other_cookie);
    EXPECT_FALSE(denied.starts_with("HTTP/1.1 200"));
    EXPECT_FALSE(response_header(denied, "location").starts_with("/repository?id="));
}

TEST_P(HttpResponseIsolation, NavigationUsesTheResolvedViewerOnEveryPage) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string account_cookie = issued.substr(0, issued.find(';'));
    ASSERT_FALSE(account_cookie.empty());
    for (const std::string& cookie : {std::string(),
                                      std::string("lake_session=invalid"),
                                      account_cookie,
                                      std::string("lake_session=front-door-operator")}) {
        const bool administrator = cookie == "lake_session=front-door-operator";
        const bool signed_in = administrator || cookie == account_cookie;
        std::set<std::string> visited;
        for (const std::string_view path : {"/", "/repositories", "/transparency"}) {
            if (std::get<1>(GetParam())) {
                connection = connect();
            }
            const std::string response = connection->request("GET", path, "", cookie);
            ASSERT_TRUE(response.starts_with("HTTP/1.1 200"));
            const auto begin = response.find("<nav class=\"primary-nav\"");
            ASSERT_NE(begin, std::string::npos);
            std::string nav = response.substr(begin, response.find("</nav>", begin) - begin);
            EXPECT_EQ(nav.contains("Administration"), administrator) << path;
            EXPECT_EQ(nav.contains("href=\"/deployments\""), signed_in) << path;
            // Account pages moved from the sidebar to the top-bar account menu.
            EXPECT_FALSE(nav.contains("href=\"/tokens\"")) << path;
            const auto menu = response.find("<details class=\"account-menu\">");
            EXPECT_EQ(menu != std::string::npos, signed_in && !administrator) << path;
            if (menu != std::string::npos) {
                const std::string links =
                    response.substr(menu, response.find("</details>", menu) - menu);
                EXPECT_TRUE(links.contains("href=\"/tokens\"")) << path;
                nav += links;
            }
            EXPECT_EQ(nav.contains("href=\"/shared-with-me\""), signed_in && !administrator)
                << path;
            EXPECT_EQ(nav.contains("href=\"/search\""), signed_in) << path;
            EXPECT_FALSE(response.contains("Orama data plane"));
            EXPECT_FALSE(response.contains("admin token"));
            // Every offered global destination must serve a page to this same viewer.
            std::size_t position = 0;
            while ((position = nav.find("href=\"", position)) != std::string::npos) {
                position += 6;
                const std::string href = nav.substr(position, nav.find('"', position) - position);
                if (!visited.insert(href).second) {
                    continue;
                }
                const std::string destination = connection->request("GET", href, "", cookie);
                EXPECT_TRUE(destination.starts_with("HTTP/1.1 200")) << href << " " << destination;
            }
        }
    }
    for (const std::string_view path : {"/search", "/settings", "/customers", "/audit"}) {
        const std::string response = connection->request("GET", path);
        EXPECT_EQ(response_header(response, "location"), "/sign-in") << path;
    }
}

TEST_P(HttpResponseIsolation, SignOutEndsTheSessionOnTheServer) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    ASSERT_TRUE(connection->request("GET", "/sessions", {}, cookie).starts_with("HTTP/1.1 200"));
    const std::string signed_out = connection->request("POST", "/logout", {}, cookie);
    ASSERT_TRUE(signed_out.starts_with("HTTP/1.1 302"));
    // The same cookie, replayed after sign-out, no longer authenticates.
    const std::string replayed = connection->request("GET", "/sessions", {}, cookie);
    EXPECT_TRUE(replayed.starts_with("HTTP/1.1 302")) << replayed;
    EXPECT_EQ(response_header(replayed, "location"), "/sign-in");
}

TEST_P(HttpResponseIsolation, TheTokensPageIssuesAndRevokesAToken) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    // No lifetime asked for: the token lasts as long as the session allows (#193).
    const std::string created = connection->request(
        "POST", "/identity/token/create", "name=revoke-me&scopes=list_sessions", cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 200")) << created.substr(0, 300);
    const auto start = created.find("Copy this token now: <code>");
    ASSERT_NE(start, std::string::npos);
    const std::string token =
        created.substr(start + 27, created.find("</code>", start) - start - 27);
    const std::string listed =
        connection->request("GET", "/v1/list_sessions", {}, "lake_session=" + token);
    ASSERT_TRUE(listed.starts_with("HTTP/1.1 200")) << listed.substr(0, 300);
    const auto sessions = nlohmann::json::parse(listed.substr(listed.find("\r\n\r\n") + 4));
    std::string id;
    for (const auto& session : sessions.at("value").at("items")) {
        if (session.value("name", "") == "revoke-me") {
            id = session.at("id");
        }
    }
    ASSERT_FALSE(id.empty());
    // The page's Revoke button posts session_id; it must reach revoke_session as its id.
    const std::string revoked = connection->request(
        "POST", "/identity/session/revoke", "session_id=" + id + "&return_to=/tokens", cookie);
    EXPECT_TRUE(revoked.starts_with("HTTP/1.1 302")) << revoked.substr(0, 300);
    const std::string after =
        connection->request("GET", "/v1/list_sessions", {}, "lake_session=" + token);
    EXPECT_FALSE(after.starts_with("HTTP/1.1 200")) << "a revoked token no longer authenticates";
}

// Developer → MCP (docs/api/mcp.md): llms.txt is public and names every tool; a read-only MCP key
// made on the page lists tools over /mcp, may read and may not write; the skill downloads.
TEST_P(HttpResponseIsolation, TheMcpPageMakesAKeyThatWorksOverMcp) {
    auto connection = connect();
    const auto body_of = [](const std::string& response) {
        return response.substr(response.find("\r\n\r\n") + 4);
    };
    const std::string llms = connection->request("GET", "/llms.txt");
    ASSERT_TRUE(llms.starts_with("HTTP/1.1 200")) << llms.substr(0, 300);
    EXPECT_NE(llms.find("http://localhost/mcp"), std::string::npos);
    EXPECT_NE(llms.find("`create_pipeline` (write)"), std::string::npos);
    // A tool called with the wrong method says which method to use (lake#256).
    const std::string wrong = connection->request("GET", "/v1/create_pipeline");
    EXPECT_TRUE(wrong.starts_with("HTTP/1.1 405")) << wrong.substr(0, 200);
    EXPECT_NE(wrong.find("call it with POST"), std::string::npos);
    // The Docs section is public and is never the raw JSON of the artifact route (lake#252).
    const std::string docs = connection->request("GET", "/docs");
    ASSERT_TRUE(docs.starts_with("HTTP/1.1 200")) << docs.substr(0, 300);
    EXPECT_NE(docs.find("Getting started"), std::string::npos);
    EXPECT_NE(docs.find("Content-Security-Policy: frame-ancestors 'self'"), std::string::npos)
        << "only lake frames lake's pages";
    EXPECT_TRUE(connection->request("GET", "/docs/train").starts_with("HTTP/1.1 200"));
    const std::string missing = connection->request("GET", "/docs/nope");
    EXPECT_TRUE(missing.starts_with("HTTP/1.1 404")) << missing.substr(0, 200);
    EXPECT_EQ(missing.find("artifact does not exist"), std::string::npos);
    const std::string full = body_of(connection->request("GET", "/llms-full.txt"));
    EXPECT_TRUE(connection->request("GET", "/developer/mcp").starts_with("HTTP/1.1 302"));

    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string page = connection->request("GET", "/developer/mcp", {}, cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("Create an MCP Key"), std::string::npos);
    const std::string created =
        connection->request("POST", "/developer/mcp/key", "access=read", cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 200")) << created.substr(0, 300);
    const std::string marker = "<code id=\"mcp-key\">";
    const auto start = created.find(marker);
    ASSERT_NE(start, std::string::npos);
    const std::string key = created.substr(start + marker.size(),
                                           created.find("</code>", start) - start - marker.size());
    EXPECT_NE(created.find("claude mcp add --transport http lake http://localhost/mcp "
                           "--header &quot;Authorization: Bearer " +
                           key + "&quot;"),
              std::string::npos);

    const std::string key_cookie = "lake_session=" + key;
    const std::string tools = connection->request(
        "POST", "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", key_cookie);
    ASSERT_TRUE(tools.starts_with("HTTP/1.1 200")) << tools.substr(0, 300);
    EXPECT_NE(tools.find("\"server_info\""), std::string::npos);
    // llms-full.txt documents every MCP tool.
    const auto listed = nlohmann::json::parse(body_of(tools));
    for (const auto& tool : listed.at("result").at("tools")) {
        const std::string name = tool.at("name");
        EXPECT_NE(full.find("### `" + name + "`"), std::string::npos) << name;
    }
    const std::string read = connection->request(
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_sessions","arguments":{}}})",
        key_cookie);
    EXPECT_NE(read.find("\"isError\":false"), std::string::npos) << read.substr(0, 600);
    const std::string write = connection->request(
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"create_token","arguments":{"name":"x","scopes":["*"]}}})",
        key_cookie);
    EXPECT_NE(write.find("credential does not grant create_token"), std::string::npos)
        << write.substr(0, 600);

    // The skill is linked from llms.txt and downloads with no credential.
    EXPECT_NE(llms.find("(http://localhost/skills/lake/SKILL.md)"), std::string::npos);
    const std::string skill = connection->request("GET", "/skills/lake/SKILL.md");
    ASSERT_TRUE(skill.starts_with("HTTP/1.1 200")) << skill.substr(0, 200);
    EXPECT_EQ(response_header(skill, "content-disposition"), "attachment; filename=SKILL.md");
    EXPECT_NE(skill.find("name: lake"), std::string::npos);
    EXPECT_NE(skill.find("approve_plan"), std::string::npos);
}

TEST_P(HttpResponseIsolation, SignInCannotAuthenticateAnotherClient) {
    for (const std::string_view path :
         {"/", "/sign-up", "/sign-in", "/v1/server_info", "/v1/openapi.json"}) {
        SCOPED_TRACE(path);
        auto connection = connect();
        const std::string sign_in = connection->request(
            "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
        ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
        ASSERT_EQ(response_header(sign_in, "location"), "/sessions");
        const std::string issued = response_header(sign_in, "set-cookie");
        ASSERT_TRUE(issued.starts_with("lake_session="));
        ASSERT_TRUE(issued.contains("HttpOnly"));
        const std::string client_a_cookie = issued.substr(0, issued.find(';'));
        if (std::get<1>(GetParam())) {
            connection.reset();
            connection = connect();
        }

        // Client B sends no credentials and adopts only cookies it actually receives.
        const std::string public_page = connection->request("GET", path);
        ASSERT_TRUE(public_page.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(response_header(public_page, "location").empty());
        const std::string received = response_header(public_page, "set-cookie");
        EXPECT_TRUE(received.empty()) << "Anonymous response received a cookie [REDACTED]";
        const std::string client_b_cookie = received.substr(0, received.find(';'));
        const std::string denied = connection->request("GET", "/sessions", {}, client_b_cookie);
        EXPECT_TRUE(denied.starts_with("HTTP/1.1 302"));
        EXPECT_EQ(response_header(denied, "location"), "/sign-in");
        EXPECT_TRUE(response_header(denied, "set-cookie").empty());

        // A still owns its valid session; B cannot acquire it from A's next response either.
        const std::string allowed = connection->request("GET", "/sessions", {}, client_a_cookie);
        EXPECT_TRUE(allowed.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(response_header(allowed, "location").empty());
        EXPECT_TRUE(response_header(allowed, "set-cookie").empty());
        const std::string anonymous = connection->request("GET", path, {}, client_b_cookie);
        EXPECT_TRUE(anonymous.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(response_header(anonymous, "location").empty());
        EXPECT_TRUE(response_header(anonymous, "set-cookie").empty());
    }
}

TEST_P(HttpResponseIsolation, AgentsReachAWorkspacesZettaThroughLakeInZettasShape) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const auto body_of = [](const std::string& response) {
        return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4));
    };
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const nlohmann::json customers =
        body_of(connection->request("GET", "/v1/list_customers", "", cookie));
    const std::string customer_id = customers["value"]["items"][0]["id"];
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    ASSERT_TRUE(requested && *requested);
    const auto service = memory->get_service((*requested)->service_id);
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["list_datasets"] = {
        {"datasets", nlohmann::json::array({"accounts"})}};
    workspace_caller_->answers["create_project"] = {{"name", "churn"}};
    workspace_caller_->answers["openapi.json"] = {
        {"openapi", "3.1.0"},
        {"info", {{"title", "zetta"}}},
        {"paths",
         {{"/v1/list_datasets", {{"get", {{"operationId", "list_datasets"}}}}},
          {"/v1/create_project", {{"post", {{"operationId", "create_project"}}}}},
          {"/v1/sql", {{"get", {{"operationId", "sql"}}}}},
          {"/v1/add_nodes", {{"post", {{"operationId", "add_nodes"}}}}}}}};
    const std::string base = "/workspaces/" + customer_id + "/zetta/v1/";

    const std::string read =
        connection->request("GET", base + "list_datasets?database=main&limit=5", "", cookie);
    ASSERT_TRUE(read.starts_with("HTTP/1.1 200")) << read.substr(0, 300);
    EXPECT_EQ(body_of(read)["ok"], true);
    EXPECT_EQ(body_of(read)["value"]["datasets"][0], "accounts");
    const auto& [read_name, was_read, read_input] = workspace_caller_->calls.back();
    EXPECT_EQ(read_name, "list_datasets");
    EXPECT_TRUE(was_read);
    EXPECT_EQ(read_input["limit"], "5") << "the workspace coerces query values itself";

    const std::string act =
        connection->request("POST", base + "create_project", R"({"name":"churn"})", cookie);
    ASSERT_TRUE(act.starts_with("HTTP/1.1 200")) << act.substr(0, 300);
    EXPECT_EQ(std::get<0>(workspace_caller_->calls.back()), "create_project");
    EXPECT_EQ(std::get<2>(workspace_caller_->calls.back())["name"], "churn");

    const std::size_t reached = workspace_caller_->calls.size();
    const std::string wrong_method =
        connection->request("POST", base + "list_datasets", "", cookie);
    EXPECT_TRUE(wrong_method.starts_with("HTTP/1.1 405")) << wrong_method.substr(0, 300);
    EXPECT_EQ(response_header(wrong_method, "allow"), "GET");
    const std::string refused = connection->request("POST", base + "add_nodes", "{}", cookie);
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 404")) << refused.substr(0, 300);
    EXPECT_NE(refused.find("not available through lake"), std::string::npos);
    const std::string anonymous = connection->request("GET", base + "list_datasets");
    EXPECT_FALSE(anonymous.starts_with("HTTP/1.1 200")) << anonymous.substr(0, 300);
    const std::string other = connection->request(
        "GET",
        "/workspaces/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0/zetta/v1/list_datasets",
        "",
        cookie);
    EXPECT_FALSE(other.starts_with("HTTP/1.1 200")) << other.substr(0, 300);
    EXPECT_EQ(workspace_caller_->calls.size(), reached) << "nothing refused reached the workspace";

    const std::string openapi = connection->request("GET", base + "openapi.json", "", cookie);
    ASSERT_TRUE(openapi.starts_with("HTTP/1.1 200")) << openapi.substr(0, 300);
    const nlohmann::json document = body_of(openapi);
    EXPECT_EQ(document["openapi"], "3.1.0") << "served bare, as Zetta serves it";
    EXPECT_TRUE(document["paths"].contains("/v1/list_datasets"));
    EXPECT_TRUE(document["paths"].contains("/v1/create_project"));
    EXPECT_FALSE(document["paths"].contains("/v1/sql"))
        << "sql is reached only through workspace_sql";
    EXPECT_FALSE(document["paths"].contains("/v1/add_nodes"));
    EXPECT_EQ(document["servers"][0]["url"], "/workspaces/" + customer_id + "/zetta");
}

TEST_P(HttpResponseIsolation, CreateAnAgentBuildsANativeOrAPythonAgentFromStudiosBoxes) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const std::string customer_id =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    ASSERT_TRUE(requested && *requested);
    const auto service = memory->get_service((*requested)->service_id);
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["create_agent"] = {{"id", "a-1"}, {"name", "Renewal watcher"}};
    workspace_caller_->answers["get_agent"] = {
        {"id", "a-1"}, {"name", "Renewal watcher"}, {"runtime", "python"}};

    const std::string boxes = "customer_id=" + customer_id +
                              "&name=Renewal+watcher&purpose=Flag+renewals&work_area=project"
                              "&work_area_detail=churn&manifesto=Company+data+only&goal=A+month+"
                              "ahead&skills=search&skills=summarize&trigger=every+Monday";
    const std::string frontier =
        connection->request("POST", "/train/agents/create", boxes + "&model=frontier", cookie);
    ASSERT_TRUE(frontier.starts_with("HTTP/1.1 302")) << frontier.substr(0, 300);
    EXPECT_NE(response_header(frontier, "location").find("/train/agent?"), std::string::npos);
    const auto created_call = [&] {
        const auto found = std::ranges::find_if(
            workspace_caller_->calls | std::views::reverse,
            [](const auto& call) { return std::get<0>(call) == "create_agent"; });
        return *found;
    };
    const auto [python_name, python_read, python] = created_call();
    EXPECT_EQ(python_name, "create_agent");
    EXPECT_FALSE(python_read);
    EXPECT_EQ(python["runtime"], "python");
    EXPECT_EQ(python["skills"], nlohmann::json::array({"search", "summarize"}));
    EXPECT_EQ(python["work_area"]["detail"], "churn");
    EXPECT_EQ(python["definition"]["source"], "zetta");
    const std::string instructions = python["definition"]["instructions"];
    EXPECT_NE(instructions.find("# Renewal watcher"), std::string::npos);
    EXPECT_NE(instructions.find("Company data only"), std::string::npos);
    EXPECT_NE(instructions.find("A month ahead"), std::string::npos);

    const std::string native =
        connection->request("POST", "/train/agents/create", boxes + "&model=base", cookie);
    ASSERT_TRUE(native.starts_with("HTTP/1.1 302")) << native.substr(0, 300);
    const nlohmann::json base = std::get<2>(created_call());
    EXPECT_EQ(base["runtime"], "native");
    EXPECT_EQ(base["model"], "/var/lib/zetta/models/base-agent");
    EXPECT_EQ(base["model_name"], "Orama base model");
    EXPECT_FALSE(base.contains("definition"));

    const std::string page = connection->request("GET", "/train/agents", "", cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("Agent Library"), std::string::npos);
    EXPECT_NE(page.find("Auto-Suggest"), std::string::npos);
}

TEST_P(HttpResponseIsolation, AskOramaAnswersProposesAndRunsOnlyWhatThePersonConfirms) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const std::string customer_id =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4))
            .at("value")
            .at("items")
            .at(0)
            .at("id");

    const std::string home = connection->request("GET", "/repositories", "", cookie);
    EXPECT_NE(home.find("href=\"/ask?from=%2Frepositories\""), std::string::npos)
        << "Ask Orama is on every page, carrying the page";

    agents_->answer = {
        {"ok", true},
        {"reply", "You have no repositories yet. I can create one."},
        {"proposals",
         nlohmann::json::array({{{"capability", "create_repository"},
                                 {"input", {{"customer_id", customer_id}, {"name", "notes"}}},
                                 {"impact", "Creates the repository notes"}}})},
        {"reads", nlohmann::json::array()},
        {"delegations", nlohmann::json::array()}};
    const std::string conversation = lake::control::new_uuid_v4();
    const std::string sent = connection->request(
        "POST",
        "/ask/send",
        "customer_id=" + customer_id + "&conversation=" + conversation +
            "&from=%2Frepositories&agent=agent-ask-orama&message=what+do+I+have%3F",
        cookie);
    ASSERT_TRUE(sent.starts_with("HTTP/1.1 302")) << sent.substr(0, 300);
    EXPECT_TRUE(agents_->turns.empty()) << "the request never waits on the agent (lake#269)";
    const std::string task = task_of(sent);
    ASSERT_FALSE(task.empty()) << response_header(sent, "location");
    const std::string thinking = connection->request(
        "GET", "/ask?customer_id=" + customer_id + "&conversation=" + conversation + "&task=" + task,
        "", cookie);
    EXPECT_NE(thinking.find("Thinking…"), std::string::npos) << "the page follows the task";
    const std::string credential = run_agent_task(*connection, task);
    ASSERT_EQ(agents_->turns.size(), 1U);
    EXPECT_EQ(agents_->turns.back()["message"], "what do I have?");
    EXPECT_EQ(agents_->turns.back()["page"]["path"], "/repositories");
    EXPECT_EQ(agents_->turns.back()["lake_token"], credential)
        << "the agent is handed the task's own credential";

    const std::string page = connection->request(
        "GET", "/ask?customer_id=" + customer_id + "&conversation=" + conversation + "&task=" + task,
        "", cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("what do I have?"), std::string::npos);
    EXPECT_NE(page.find("You have no repositories yet"), std::string::npos);
    EXPECT_NE(page.find("Creates the repository notes"), std::string::npos);
    EXPECT_NE(page.find("action=\"/ask/confirm\""), std::string::npos);
    EXPECT_FALSE(store_->list_repositories(customer_id, 10).value().size() > 0)
        << "nothing is created before the person confirms";

    const auto messages = store_->ask_conversation(
        conversation, store_->context_for(cookie.substr(cookie.find('=') + 1))->user_id, 10);
    ASSERT_TRUE(messages && messages->size() == 2U);
    const std::string message_id = messages->at(1).id;
    const std::string forged =
        connection->request("POST",
                            "/ask/confirm",
                            "customer_id=" + customer_id + "&conversation=" + conversation +
                                "&message_id=" + message_id + "&index=7",
                            cookie);
    EXPECT_NE(response_header(forged, "location").find("error="), std::string::npos)
        << "only a proposal lake stored can be confirmed";
    const std::string confirmed =
        connection->request("POST",
                            "/ask/confirm",
                            "customer_id=" + customer_id + "&conversation=" + conversation +
                                "&message_id=" + message_id + "&index=0",
                            cookie);
    ASSERT_TRUE(confirmed.starts_with("HTTP/1.1 302")) << confirmed.substr(0, 300);
    EXPECT_EQ(response_header(confirmed, "location").find("error="), std::string::npos)
        << response_header(confirmed, "location");
    const auto repositories = store_->list_repositories(customer_id, 10);
    ASSERT_TRUE(repositories);
    ASSERT_EQ(repositories->size(), 1U);
    EXPECT_EQ(repositories->front().name, "notes");
    const std::string after = connection->request(
        "GET", "/ask?customer_id=" + customer_id + "&conversation=" + conversation, "", cookie);
    EXPECT_NE(after.find("Done: create_repository."), std::string::npos);
    EXPECT_NE(after.find("Confirmed"), std::string::npos);
}

TEST_P(HttpResponseIsolation, GrantingAPipelineWritesTheWholeListBackToZetta) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const std::string customer_id =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    const auto service = memory->get_service((*requested)->service_id);
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["get_agent"] = {
        {"id", "a-1"},
        {"name", "Watcher"},
        {"pipelines",
         nlohmann::json::array({{{"project", "churn"}, {"graph", "weekly"}, {"version", "v1"}}})}};
    workspace_caller_->answers["update_agent"] = {{"id", "a-1"}};
    const std::string granted =
        connection->request("POST",
                            "/train/agent/grant",
                            "customer_id=" + customer_id + "&id=a-1&project=churn&graph=daily",
                            cookie);
    ASSERT_TRUE(granted.starts_with("HTTP/1.1 302")) << granted.substr(0, 300);
    const auto& [name, read, input] = workspace_caller_->calls.back();
    EXPECT_EQ(name, "update_agent");
    EXPECT_EQ(input["pipelines"],
              nlohmann::json::array({{{"project", "churn"}, {"graph", "weekly"}},
                                     {{"project", "churn"}, {"graph", "daily"}}}));
    const std::string revoked =
        connection->request("POST",
                            "/train/agent/revoke",
                            "customer_id=" + customer_id + "&id=a-1&project=churn&graph=weekly",
                            cookie);
    ASSERT_TRUE(revoked.starts_with("HTTP/1.1 302"));
    EXPECT_EQ(std::get<2>(workspace_caller_->calls.back())["pipelines"], nlohmann::json::array());
}

// lake#290/#291: Design → Methodologies and Design → Solutions list the engine's registries, and a
// solution starts as a project with its composed pipeline.
TEST_P(HttpResponseIsolation, DesignListsMethodologiesAndSolutionsAndStartsASolution) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const std::string customer_id =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4))
            .at("value").at("items").at(0).at("id");
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    const auto service = memory->get_service((*requested)->service_id);
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";

    // An engine without the solution registry: the page says so, and Methodologies still lists.
    workspace_caller_->answers["list_methodologies"] = {
        {"methodologies",
         nlohmann::json::array(
             {{{"id", "M-3"}, {"name", "Segmentation / clustering"}, {"family", "Identity & structure"},
               {"inputs", nlohmann::json::array({{{"role", "entity"}}})},
               {"outputs", nlohmann::json::array({"segment assignments"})},
               {"metrics", nlohmann::json::array({"silhouette"})}, {"runnable", true}},
              {{"id", "M-14"}, {"name", "Speech & audio analytics"}, {"family", "Language"},
               {"runnable", false}, {"issue", "zetta#1053"}}})}};
    const std::string methods = connection->request(
        "GET", "/design/methodologies?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(methods.starts_with("HTTP/1.1 200")) << methods.substr(0, 300);
    EXPECT_NE(methods.find("M-3 Segmentation / clustering"), std::string::npos);
    EXPECT_NE(methods.find("Runs today"), std::string::npos);
    EXPECT_NE(methods.find("Not yet"), std::string::npos);
    EXPECT_NE(methods.find("href=\"/design/methodologies\""), std::string::npos) << "in the nav";
    EXPECT_NE(methods.find("href=\"/design/solutions\""), std::string::npos) << "in the nav";
    // The page's own nav item is the current one, not Projects (Steve, 2026-09-28).
    EXPECT_NE(methods.find("href=\"/design/methodologies\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(methods.find("href=\"/design/projects\" aria-current=\"page\""), std::string::npos);
    const std::string one = connection->request(
        "GET", "/design/methodology?customer_id=" + customer_id + "&id=M-3", "", cookie);
    EXPECT_NE(one.find("silhouette"), std::string::npos);
    EXPECT_NE(one.find("segment assignments"), std::string::npos);
    const std::string waiting = connection->request(
        "GET", "/design/solutions?customer_id=" + customer_id, "", cookie);
    EXPECT_NE(waiting.find("does not list solutions yet"), std::string::npos);

    // With the registry: modules and sketches, a module page, and Start.
    workspace_caller_->answers["list_solutions"] = {
        {"solutions",
         nlohmann::json::array(
             {{{"id", "S-1"}, {"name", "Churn & Retention"}, {"promise", "Which customers will leave"},
               {"methodologies", nlohmann::json::array({"M-1", "M-2", "M-3"})}, {"runnable", true}},
              {{"id", "B-1"}, {"name", "Dynamic Pricing & Discount Governance"},
               {"status", "sketch"}, {"runnable", false}}})}};
    workspace_caller_->answers["get_solution"] = {
        {"solution",
         {{"id", "S-1"}, {"name", "Churn & Retention"}, {"promise", "Which customers will leave"},
          {"composition",
           nlohmann::json::array({{{"methodology", "M-1"}, {"minimum_level", 1}},
                                  {{"methodology", "M-5"}, {"minimum_level", 2}}})},
          {"action_patterns", nlohmann::json::array({"tasks in CRM", "driver report"})},
          {"latency_classes", nlohmann::json::array({"L1"})}, {"runnable", true},
          {"required_data_roles", nlohmann::json::array({{{"role", "entity"}}})},
          {"capability_levels",
           nlohmann::json::array({{{"level", 1}, {"name", "CL-1"}, {"description", "Propensity core"}}})}}}};
    const std::string catalogue = connection->request(
        "GET", "/design/solutions?customer_id=" + customer_id, "", cookie);
    EXPECT_NE(catalogue.find("S-1 Churn &amp; Retention"), std::string::npos);
    EXPECT_NE(catalogue.find("In Design"), std::string::npos);
    EXPECT_NE(catalogue.find("href=\"/design/solutions\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(catalogue.find("href=\"/design/projects\" aria-current=\"page\""), std::string::npos);
    const std::string module = connection->request(
        "GET", "/design/solution?customer_id=" + customer_id + "&id=S-1", "", cookie);
    EXPECT_NE(module.find("tasks in CRM"), std::string::npos);
    EXPECT_NE(module.find("level 2+"), std::string::npos) << "attachable heads show their level";
    EXPECT_NE(module.find("action=\"/design/solutions/start\""), std::string::npos);
    workspace_caller_->answers["create_project"] = {{"name", "churn"}};
    workspace_caller_->answers["create_solution"] = {{"project", "churn"}};
    const std::string started = connection->request(
        "POST", "/design/solutions/start",
        "customer_id=" + customer_id + "&solution=S-1&name=churn&level=1", cookie);
    ASSERT_TRUE(started.starts_with("HTTP/1.1 302")) << started.substr(0, 300);
    EXPECT_NE(response_header(started, "location").find("/design/project?"), std::string::npos);
    const auto& [name, read, input] = workspace_caller_->calls.back();
    EXPECT_EQ(name, "create_solution");
    EXPECT_EQ(input["solution"], "S-1");
    EXPECT_EQ(input["level"], 1);
    EXPECT_EQ(input["project"], "churn");
}

TEST_P(HttpResponseIsolation, DiscoverOpportunitiesScansAndCreatesDraftsOnlyOnConfirmation) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const std::string customer_id =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    const auto service = memory->get_service((*requested)->service_id);
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["list_datasets"] = {
        {"datasets", nlohmann::json::array({{{"name", "accounts"}}})}};
    workspace_caller_->answers["create_project"] = {{"name", "x"}};

    const std::string page = connection->request("GET", "/design/opportunities", "", cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("action=\"/design/opportunities/scan\""), std::string::npos);

    agents_->answer = {
        {"ok", true},
        {"result",
         {{"project_profile",
           {{"suggested",
             nlohmann::json::array({{{"intent", "Predict which accounts will churn"},
                                     {"solution", "S-1"},
                                     {"methodology", "gbdt"},
                                     {"why_this_data_supports_it", "accounts.churned is the label"},
                                     {"required_first", {"entity resolution"}}}})},
            {"declined",
             nlohmann::json::array({{{"intent", "Forecast revenue"},
                                     {"reason", "no timestamp column"},
                                     {"answer", "capture it first"}}})}}}}}};
    // The scan runs in the background (lake#269): the request returns at once.
    const std::string scanned = connection->request(
        "POST", "/design/opportunities/scan", "customer_id=" + customer_id, cookie);
    ASSERT_TRUE(scanned.starts_with("HTTP/1.1 302")) << scanned.substr(0, 300);
    EXPECT_TRUE(agents_->turns.empty()) << "the request never waits on Auto-Suggest";
    const std::string task = task_of(scanned);
    ASSERT_FALSE(task.empty());
    const std::string scanning = connection->request(
        "GET", "/design/opportunities?customer_id=" + customer_id + "&task=" + task, "", cookie);
    EXPECT_NE(scanning.find("Scanning your workspace"), std::string::npos);
    run_agent_task(*connection, task);
    EXPECT_FALSE(agents_->turns.back().contains("profile"))
        << "Auto-Suggest builds the profile itself with its read-only credential";
    EXPECT_FALSE(agents_->turns.back()["zetta_token"].get<std::string>().empty());

    const std::string found = connection->request(
        "GET", "/design/opportunities?customer_id=" + customer_id + "&task=" + task, "", cookie);
    EXPECT_NE(found.find("Predict which accounts will churn"), std::string::npos);
    EXPECT_NE(found.find("Catalogue solution S-1"), std::string::npos);
    EXPECT_NE(found.find("no timestamp column"), std::string::npos);
    const std::string later =
        connection->request("GET", "/design/opportunities?customer_id=" + customer_id, "", cookie);
    EXPECT_NE(later.find("Predict which accounts will churn"), std::string::npos)
        << "Opportunities are kept after the scan";
    const auto rows = store_->list_opportunities(customer_id);
    ASSERT_TRUE(rows && rows->size() == 2U);
    const auto chosen = std::ranges::find(*rows, std::string("new"), &lake::control::Opportunity::status);
    ASSERT_NE(chosen, rows->end());

    const std::size_t before = workspace_caller_->calls.size();
    const std::string unconfirmed = connection->request(
        "POST", "/design/opportunities/create",
        "customer_id=" + customer_id + "&pick=" + chosen->id, cookie);
    EXPECT_NE(response_header(unconfirmed, "location").find("error="), std::string::npos);
    EXPECT_EQ(workspace_caller_->calls.size(), before) << "nothing is created without confirming";
    const std::string created = connection->request(
        "POST", "/design/opportunities/create",
        "customer_id=" + customer_id + "&pick=" + chosen->id + "&confirm=yes", cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 302")) << created.substr(0, 300);
    const auto& [name, read, input] = workspace_caller_->calls.back();
    EXPECT_EQ(name, "create_project");
    EXPECT_TRUE(input["name"].get<std::string>().starts_with("predict-which-accounts-will-churn-"));
    EXPECT_NE(input["description"].get<std::string>().find("Source Opportunity"),
              std::string::npos);
    EXPECT_EQ(input["intent"], "Predict which accounts will churn");
    const auto after = store_->list_opportunities(customer_id);
    const auto made = std::ranges::find(*after, chosen->id, &lake::control::Opportunity::id);
    EXPECT_EQ(made->status, "project");
    const std::string project = input["name"];
    EXPECT_EQ(made->project, project);

    // The Project Agent designs the new project's pipeline in the background, and the project's
    // page compiles the design once it is done (Steve, 2026-09-27).
    const auto build = store_->latest_agent_task(customer_id, "build:" + project);
    ASSERT_TRUE(build) << "creating the project started the Project Agent";
    EXPECT_EQ(build->agent, "agent-project");
    EXPECT_NE(build->body["message"].get<std::string>().find("Predict which accounts will churn"),
              std::string::npos);
    workspace_caller_->answers["get_project"] = {{"name", project},
                                                 {"graphs", nlohmann::json::array()}};
    const std::string page_path =
        "/design/project?customer_id=" + customer_id + "&name=" + project;
    const std::string designing = connection->request("GET", page_path, "", cookie);
    EXPECT_NE(designing.find("The Project Agent is designing"), std::string::npos);
    EXPECT_EQ(std::ranges::count_if(workspace_caller_->calls,
                                    [](const auto& call) {
                                        return std::get<0>(call) == "compile_intent";
                                    }),
              0)
        << "nothing is compiled before the design exists";

    agents_->answer = {
        {"ok", true},
        {"result",
         {{"applied", false},
          {"pipeline",
           {{"dataset", "datasets.accounts"},
            {"label", "churned"},
            {"features", {"failed_payments", "tenure_months"}},
            {"model_kind", "gbdt"},
            {"holdout_fraction", 0.2}}}}}};
    run_agent_task(*connection, build->id);
    workspace_caller_->answers["capture_intent"] = {{"input_id", "in-1"}};
    workspace_caller_->answers["compile_intent"] = {{"name", "churn"}};
    workspace_caller_->answers["get_graph"] = {
        {"name", "churn"},
        {"nodes",
         {{{"id", "load"}, {"kind", "connect"}, {"name", "Load"}},
          {{"id", "extra"}, {"kind", "connect"}, {"name", "Extra"}},
          {{"id", "train"}, {"kind", "model"}, {"name", "Train"}}}},
        {"edges", nlohmann::json::array()}};
    const std::string built = connection->request("GET", page_path, "", cookie);
    ASSERT_TRUE(built.starts_with("HTTP/1.1 200")) << built.substr(0, 300);
    const auto captured = std::ranges::find_if(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "capture_intent";
    });
    ASSERT_NE(captured, workspace_caller_->calls.end());
    const std::string text = std::get<2>(*captured)["text"];
    EXPECT_NE(text.find("Predict churned from datasets.accounts using failed_payments, "
                        "tenure_months."),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("Train a gbdt model with a 20% hold-out."), std::string::npos) << text;
    const auto saved = std::ranges::find_if(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "save_graph";
    });
    ASSERT_NE(saved, workspace_caller_->calls.end()) << "pointed at the design's dataset";
    EXPECT_NE(std::get<2>(*saved).dump().find("\"table\":\"accounts\""), std::string::npos)
        << std::get<2>(*saved).dump();
    EXPECT_NE(std::get<2>(*saved).dump().find("\"target\":\"churned\""), std::string::npos)
        << "the model step learns the design's target";

    // Zetta compiles a pipeline from a project's intent when it is created, and a design may name
    // its table under training_sources: the project's own pipeline is pointed at it, once.
    const auto second = store_->latest_agent_task(customer_id, "build:" + project);
    const std::string again = lake::control::new_uuid_v4();
    auto rebuild = store_->create_agent_task(lake::control::AgentTask{
        .customer_id = customer_id, .agent = "agent-project", .kind = "build:" + project,
        .body = {{"message", "Design it"}}, .scopes = second->scopes,
        .requested_by_user_id = second->requested_by_user_id,
        .requested_by_taap_id = second->requested_by_taap_id});
    ASSERT_TRUE(rebuild);
    ASSERT_TRUE(liquid::runtime::block_on(agent_bus_->publish_task(*rebuild)).value());
    agents_->answer = {{"ok", true},
                       {"result",
                        {{"pipeline",
                          {{"training_sources", {"datasets.accounts_v2"}},
                           {"label", "churned"},
                           {"model_kind", "gbdt"}}}}}};
    run_agent_task(*connection, rebuild->id);
    workspace_caller_->answers["get_project"] = {
        {"name", project},
        {"graphs", {{{"name", "churn skeleton"}, {"approval", "proposed"}}}}};
    const std::size_t compiles = std::ranges::count_if(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "compile_intent";
    });
    connection->request("GET", page_path, "", cookie);
    EXPECT_EQ(std::ranges::count_if(workspace_caller_->calls,
                                    [](const auto& call) {
                                        return std::get<0>(call) == "compile_intent";
                                    }),
              compiles)
        << "a project that has a pipeline is not compiled again";
    const auto repointed = std::ranges::find_if(
        workspace_caller_->calls.rbegin(), workspace_caller_->calls.rend(),
        [](const auto& call) { return std::get<0>(call) == "save_graph"; });
    ASSERT_NE(repointed, workspace_caller_->calls.rend());
    EXPECT_NE(std::get<2>(*repointed).dump().find("\"table\":\"accounts_v2\""), std::string::npos)
        << std::get<2>(*repointed).dump();
    const std::size_t saves = std::ranges::count_if(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "save_graph";
    });
    workspace_caller_->answers["get_graph"]["nodes"][0]["attributes"] = {{"source", "datasets"},
                                                                         {"table", "accounts_v2"}};
    connection->request("GET", page_path, "", cookie);
    EXPECT_EQ(std::ranges::count_if(workspace_caller_->calls,
                                    [](const auto& call) { return std::get<0>(call) == "save_graph"; }),
              saves)
        << "applied once: a pipeline that reads the design's table is left alone";

    // A retrieval design has no label or model (the model writes null): it still applies, and a
    // task that cannot be read is said on the page (live, 2026-09-27: "type must be string").
    auto retrieval = store_->create_agent_task(lake::control::AgentTask{
        .customer_id = customer_id, .agent = "agent-project", .kind = "build:" + project,
        .body = {{"message", "Design it"}}, .scopes = second->scopes,
        .requested_by_user_id = second->requested_by_user_id,
        .requested_by_taap_id = second->requested_by_taap_id});
    ASSERT_TRUE(liquid::runtime::block_on(agent_bus_->publish_task(*retrieval)).value());
    agents_->answer = {{"ok", true},
                       {"result",
                        {{"pipeline",
                          {{"sources", {"churn_results.at_risk_1"}},
                           {"label", nullptr},
                           {"model_kind", nullptr},
                           {"model", false}}}}}};
    run_agent_task(*connection, retrieval->id);
    const std::string applied = connection->request("GET", page_path, "", cookie);
    ASSERT_TRUE(applied.starts_with("HTTP/1.1 200"));
    EXPECT_EQ(applied.find("could not be read"), std::string::npos) << "null fields are no error";
    const auto retrieved = std::ranges::find_if(
        workspace_caller_->calls.rbegin(), workspace_caller_->calls.rend(),
        [](const auto& call) { return std::get<0>(call) == "save_graph"; });
    EXPECT_NE(std::get<2>(*retrieved).dump().find("\"table\":\"at_risk_1\""), std::string::npos);
}

TEST_P(HttpResponseIsolation, TheAgentFactoryCreatesOnlyTheExactProposalThePersonApproved) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const std::string customer_id =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    const auto service = memory->get_service((*requested)->service_id);
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["create_agent"] = {{"id", "a-new"}, {"name", "Renewal watcher"}};
    workspace_caller_->answers["get_agent"] = {
        {"id", "a-new"}, {"name", "Renewal watcher"}, {"runtime", "python"}};

    const nlohmann::json proposal = {
        {"type", "python"},
        {"why_type", "needs a helper"},
        {"storage", "zetta"},
        {"why_storage", "small"},
        {"agent",
         {{"name", "Renewal watcher"},
          {"purpose", "Flag renewals at risk"},
          {"work_area", {{"kind", "workspace"}, {"detail", ""}}},
          {"manifesto", "Company data only."},
          {"goal", "A month ahead."},
          {"skills", {"search"}},
          {"trigger", ""}}},
        {"pipelines", nlohmann::json::array()},
        {"files",
         nlohmann::json::array({{{"path", "agent/tools.py"}, {"contents", "TOOLS = []\n"}}})},
        {"secret_mount", ""}};
    // AgentCreator answers words for the person and the structured answer beside them
    // (agent-creator#1).
    agents_->answer = {
        {"ok", true},
        {"reply", "I propose an agent named Renewal watcher: Flag renewals at risk."},
        {"result",
         {{"kind", "proposal"}, {"proposal", proposal}, {"defects", nlohmann::json::array()}}}};
    const std::string conversation = lake::control::new_uuid_v4();
    const std::string sent =
        connection->request("POST",
                            "/train/agents/new/send",
                            "customer_id=" + customer_id + "&conversation=" + conversation +
                                "&message=an+agent+that+flags+renewals",
                            cookie);
    ASSERT_TRUE(sent.starts_with("HTTP/1.1 302")) << sent.substr(0, 300);
    // AgentCreator works in the background (lake#269).
    const std::string task = task_of(sent);
    ASSERT_FALSE(task.empty()) << response_header(sent, "location");
    const std::string working = connection->request(
        "GET",
        "/train/agents/new?customer_id=" + customer_id + "&conversation=" + conversation +
            "&task=" + task,
        "",
        cookie);
    EXPECT_NE(working.find("Working on it…"), std::string::npos);
    const std::string credential = run_agent_task(*connection, task);
    EXPECT_EQ(agents_->turns.back()["lake_token"], credential);
    const std::string page = connection->request("GET",
                                                 "/train/agents/new?customer_id=" + customer_id +
                                                     "&conversation=" + conversation +
                                                     "&task=" + task,
                                                 "",
                                                 cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("Proposed Agent: Renewal watcher"), std::string::npos);
    EXPECT_NE(page.find("agent/tools.py"), std::string::npos);
    const std::string sha_marker = "name=\"proposal_sha256\" value=\"";
    const auto at = page.find(sha_marker);
    ASSERT_NE(at, std::string::npos);
    const std::string sha = page.substr(at + sha_marker.size(), 64);
    const std::string id_marker = "name=\"message_id\" value=\"";
    const auto id_at = page.find(id_marker);
    const std::string message_id =
        page.substr(id_at + id_marker.size(),
                    page.find('"', id_at + id_marker.size()) - id_at - id_marker.size());

    const std::size_t before = workspace_caller_->calls.size();
    const std::string stale = connection->request(
        "POST",
        "/train/agents/new/approve",
        "customer_id=" + customer_id + "&conversation=" + conversation +
            "&message_id=" + message_id + "&proposal_sha256=" + std::string(64, '0'),
        cookie);
    EXPECT_NE(response_header(stale, "location").find("error="), std::string::npos);
    EXPECT_EQ(workspace_caller_->calls.size(), before) << "nothing is created for another proposal";

    const std::string approved =
        connection->request("POST",
                            "/train/agents/new/approve",
                            "customer_id=" + customer_id + "&conversation=" + conversation +
                                "&message_id=" + message_id + "&proposal_sha256=" + sha,
                            cookie);
    ASSERT_TRUE(approved.starts_with("HTTP/1.1 302")) << approved.substr(0, 300);
    EXPECT_NE(response_header(approved, "location").find("/train/agent?"), std::string::npos);
    const auto created = std::ranges::find_if(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "create_agent";
    });
    ASSERT_NE(created, workspace_caller_->calls.end());
    const nlohmann::json& record = std::get<2>(*created);
    EXPECT_EQ(record["runtime"], "python");
    EXPECT_EQ(record["definition"]["source"], "zetta");
    EXPECT_NE(record["definition"]["instructions"].get<std::string>().find("# Renewal watcher"),
              std::string::npos);
    EXPECT_EQ(record["definition"]["files"][0]["path"], "agent/tools.py");
    const auto launches = memory->list_agent_launches(customer_id, "a-new", 10);
    ASSERT_TRUE(launches && launches->size() == 1U) << "a first check run starts";
}

INSTANTIATE_TEST_SUITE_P(HttpServer,
                         HttpResponseIsolation,
                         testing::Combine(testing::Bool(), testing::Bool()),
                         [](const testing::TestParamInfo<HttpResponseIsolation::ParamType>& info) {
                             return std::string(std::get<0>(info.param) ? "Postgres" : "Memory") +
                                    (std::get<1>(info.param) ? "FreshConnection"
                                                             : "ReusedConnection");
                         });

} // namespace

TEST(HttpServer, ResponseResetPreservesCurrentProtocolAndCors) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.instance_kek = "response-reset-test-kek";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    HttpConnection connection(server.port());
    const std::string preflight = connection.request("OPTIONS", "/sign-in", {}, {}, "HTTP/1.0");
    ASSERT_TRUE(preflight.starts_with("HTTP/1.0 204"));
    EXPECT_EQ(response_header(preflight, "connection"), "keep-alive");
    EXPECT_EQ(response_header(preflight, "access-control-allow-origin"), "*");
    EXPECT_FALSE(response_header(preflight, "access-control-allow-methods").empty());

    const std::string missing = connection.request("GET", "/missing-response-reset-route");
    ASSERT_TRUE(missing.starts_with("HTTP/1.1 404"));
    EXPECT_EQ(response_header(missing, "access-control-allow-origin"), "*");
    EXPECT_TRUE(response_header(missing, "access-control-allow-methods").empty());
    EXPECT_TRUE(response_header(missing, "connection").empty());

    const std::string healthy = connection.request("GET", "/healthz", {}, {}, "HTTP/1.0");
    ASSERT_TRUE(healthy.starts_with("HTTP/1.0 200"));
    EXPECT_EQ(response_header(healthy, "connection"), "keep-alive");
    EXPECT_EQ(response_header(healthy, "access-control-allow-origin"), "*");
    EXPECT_TRUE(response_header(healthy, "access-control-allow-methods").empty());
}

TEST(HttpServer, ClerkReplacesTheBrowserSignInPagesWhenConfigured) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.instance_kek = "clerk-http-test-kek";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    // base64("127.0.0.1$"): a Frontend API host that refuses connections, so the JWKS fetch fails
    // fast and every token is refused.
    options.clerk_publishable_key = "pk_test_MTI3LjAuMC4xJA";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());

    const std::string sign_in = connection.request("GET", "/sign-in");
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 200")) << sign_in;
    EXPECT_NE(sign_in.find("data-clerk-publishable-key=\"pk_test_MTI3LjAuMC4xJA\""),
              std::string::npos);
    EXPECT_NE(sign_in.find("mountSignIn"), std::string::npos);
    EXPECT_EQ(sign_in.find("name=\"password\""), std::string::npos);
    const std::string sign_up = connection.request("GET", "/sign-up");
    ASSERT_TRUE(sign_up.starts_with("HTTP/1.1 200"));
    EXPECT_NE(sign_up.find("mountSignUp"), std::string::npos);
    EXPECT_EQ(sign_up.find("identity_document"), std::string::npos);

    const std::string refused =
        connection.request("POST", "/identity/clerk-session", "token=not.a.jwt&device_name=b");
    ASSERT_TRUE(refused.starts_with("HTTP/1.1 302")) << refused;
    EXPECT_TRUE(response_header(refused, "location").starts_with("/sign-in?error="));
    EXPECT_TRUE(response_header(refused, "set-cookie").empty());

    const std::string signed_out = connection.request("POST", "/logout");
    EXPECT_EQ(response_header(signed_out, "location"), "/sign-in?signed_out=1");
}

TEST(HttpServer, ClerkSessionExchangeIsAbsentWithoutClerk) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.instance_kek = "clerk-off-http-test-kek";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    EXPECT_TRUE(connection.request("POST", "/identity/clerk-session", "token=x")
                    .starts_with("HTTP/1.1 404"));
    const std::string sign_in = connection.request("GET", "/sign-in");
    EXPECT_EQ(sign_in.find("clerk"), std::string::npos);
    EXPECT_EQ(response_header(connection.request("POST", "/logout"), "location"), "/");
}

TEST(HttpServer, IdleKeepAliveCloseWritesNothingToStderr) {
    StderrPipe captured_stderr;
    ASSERT_TRUE(captured_stderr.ready());

    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.node_name = "idle-close-test";
    options.admin_token = "idle-close-admin";
    options.instance_kek = "idle-close-instance-kek";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const int keep_alive = connect_loopback(server.port());
    ASSERT_GE(keep_alive, 0);
    ASSERT_TRUE(send_all(keep_alive,
                         "GET /healthz HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: keep-alive\r\n\r\n"));
    const std::string first_response = read_response(keep_alive, false);
    ASSERT_NE(first_response.find(" 200 "), std::string::npos) << first_response;
    ::close(keep_alive);

    const int confirmation = connect_loopback(server.port());
    ASSERT_GE(confirmation, 0);
    ASSERT_TRUE(send_all(confirmation,
                         "GET /healthz HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: close\r\n\r\n"));
    const std::string confirmation_response = read_response(confirmation, true);
    ::close(confirmation);
    ASSERT_NE(confirmation_response.find(" 200 "), std::string::npos) << confirmation_response;

    server.stop();
    EXPECT_EQ(captured_stderr.finish(), "");
}
