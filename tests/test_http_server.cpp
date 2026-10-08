#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <functional>

#include "lake/control/identity_crypto.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <liquid/identity.hpp>
#include <liquid/liquid.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <pqxx/pqxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <boost/url.hpp>

#include "lake/api/oauth.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/web_apps.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "temporary_directory.hpp"
#include "stream_fixture.hpp"
#include "workspace_fixture.hpp"
#include "lake/api/web_ui.hpp"
#include "api/route_guard.hpp"
#include "repository_test_support.hpp"

#include <boost/beast2/http_server.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/http/server/router.hpp>

namespace {

// Captures what the process writes to one descriptor (stderr by default, or stdout).
class OutputPipe {
public:
    explicit OutputPipe(int fd = STDERR_FILENO, std::FILE* stream = stderr)
        : fd_(fd), stream_(stream) {
        if (::pipe(pipe_) != 0) {
            return;
        }
        original_ = ::dup(fd_);
        if (original_ < 0) {
            close_pipe();
            return;
        }
        std::fflush(stream_);
        std::cerr.flush();
        if (::dup2(pipe_[1], fd_) < 0) {
            ::close(original_);
            original_ = -1;
            close_pipe();
            return;
        }
        ::close(pipe_[1]);
        pipe_[1] = -1;
        capturing_ = true;
    }

    ~OutputPipe() {
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
        std::fflush(stream_);
        std::cerr.flush();
        static_cast<void>(::dup2(original_, fd_));
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

    int fd_;
    std::FILE* stream_;
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

std::string response_body(const std::string& response) {
    const auto header_end = response.find("\r\n\r\n");
    return header_end == std::string::npos ? std::string() : response.substr(header_end + 4);
}

std::string response_cookie_pair(const std::string& response) {
    std::string cookie = response_header(response, "set-cookie");
    if (const std::size_t end = cookie.find(';'); end != std::string::npos) {
        cookie.resize(end);
    }
    return cookie;
}

class HttpConnection {
public:
    explicit HttpConnection(unsigned short port) : descriptor_(connect_loopback(port)) {}
    std::string workspace_id;

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
                        std::string_view content_type = "application/x-www-form-urlencoded",
                        const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        std::string selected_path(path);
        std::string selected_body(body);
        const auto page = path.substr(0, path.find('?'));
        if (!workspace_id.empty() && lake::api::workspace_page_path(page)) {
            selected_path = "/w/" + workspace_id + std::string(path);
        }
        if (!workspace_id.empty() && method == "POST" &&
            content_type == "application/x-www-form-urlencoded" &&
            !lake::api::workspace_local_path(selected_path).starts_with("/workspaces/") &&
            selected_body.find("workspace_id=") == std::string::npos &&
            (selected_path.starts_with("/w/") || page == "/repository/build")) {
            selected_body += "&workspace_id=" + workspace_id;
        }
        path = selected_path;
        body = selected_body;
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
        for (const auto& [name, value] : headers) {
            wire += name + ": " + value + "\r\n";
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

TEST(WorkspaceNavigationHttp, SelectorRedirectsRememberedWorkspaceAndValidatesForms) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"taap:workspace-nav", "", "Navigation"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "navigation"});
    ASSERT_TRUE(customer);
    lake::test::StreamWorkspace workspace(store, customer->id);
    lake::api::ApiServer server({.port = 0, .store = store, .workspaces = workspace.service,
        .admin_token = "workspace-navigation-operator"});
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    const auto request = [&](const std::string& method, const std::string& path,
                             const std::string& body = "", const std::string& cookie = "") {
        return connection.request(method, path, body, cookie, "HTTP/1.1", "workspace-navigation-operator");
    };
    const std::string query = "customer_id=" + customer->id;
    const auto old = request("GET", "/train/streams?" + query);
    ASSERT_TRUE(old.starts_with("HTTP/1.1 302")) << old;
    EXPECT_EQ(response_header(old, "location"), "/w/" + workspace.id + "/train/streams?" + query);
    EXPECT_TRUE(response_header(old, "set-cookie").starts_with("lake_workspace=" + workspace.id));
    const auto first = request("GET", response_header(old, "location"));
    ASSERT_TRUE(first.starts_with("HTTP/1.1 200"));
    EXPECT_NE(first.find("href=\"/w/" + workspace.id + "/ship/apps\""), std::string::npos);
    EXPECT_NE(first.find("name=\"workspace_id\" value=\"" + workspace.id + "\""), std::string::npos);
    EXPECT_NE(first.find("New workspace</a>"), std::string::npos);
    const auto second = workspace.add(store, customer->id, "second");
    const auto switched = request("GET", "/workspace/select?" + query + "&workspace_id=" + second +
        "&page=%2Ftrain%2Fstreams&page_query=prefix%3Dorders%26limit%3D20");
    ASSERT_TRUE(switched.starts_with("HTTP/1.1 302")) << switched;
    EXPECT_EQ(response_header(switched, "location"), "/w/" + second + "/train/streams?prefix=orders&limit=20&" + query);
    const auto remembered = request("GET", "/train/streams?" + query, "", "lake_workspace=" + second);
    EXPECT_EQ(response_header(remembered, "location"), "/w/" + second + "/train/streams?" + query);
    const auto selected = request("GET", response_header(remembered, "location"));
    EXPECT_NE(selected.find("href=\"/w/" + second + "/train/streams\" aria-current=\"page\""), std::string::npos);
    // The selector names a workspace, not "name (environment)" (lake#452); names are unique.
    EXPECT_NE(selected.find("value=\"" + second + "\" selected>second</option>"),
              std::string::npos);
    EXPECT_EQ(selected.find("(prod)"), std::string::npos);
    EXPECT_NE(selected.find("class=\"filters workspace-picker\""), std::string::npos);
    // `/w/<workspace>/` is the workspace's home, not an unknown route (lake#452).
    for (const std::string root : {"/w/" + second + "/", "/w/" + second}) {
        const auto home = request("GET", root + "?" + query);
        ASSERT_TRUE(home.starts_with("HTTP/1.1 302")) << root << "\n" << home;
        EXPECT_EQ(response_header(home, "location"),
                  "/w/" + second + "/design/projects?" + query);
    }
    EXPECT_TRUE(request("GET", "/w/not-a-workspace/").starts_with("HTTP/1.1 404"));
    const auto unknown = request("GET", "/w/" + lake::control::new_uuid_v4() + "/train/streams?" + query);
    EXPECT_TRUE(unknown.starts_with("HTTP/1.1 400"));
    EXPECT_NE(unknown.find("workspace_id"), std::string::npos);
    for (const std::string field : {std::string(), "&workspace_id=" + second}) {
        const auto refused = request("POST", "/w/" + workspace.id + "/ship/apps/create", query + field);
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 400")) << refused;
        EXPECT_NE(refused.find("invalid_input"), std::string::npos);
        EXPECT_NE(refused.find("workspace_id"), std::string::npos);
    }
    const auto created = request("POST", "/workspaces/create", query + "&name=third&environment=staging");
    EXPECT_TRUE(created.starts_with("HTTP/1.1 302")) << created;
    EXPECT_EQ(store->list_services(customer->id)->size(), 3);
    const auto rows = store->list_workspaces(customer->id);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), 3);
    const auto third = rows->back();
    EXPECT_NE(third.id, third.zetta_service_id);
    EXPECT_EQ(third.environment, "staging");
    EXPECT_EQ(third.phase, "Pending");
    const auto management = request("GET", "/workspace/select?" + query + "&workspace_id=" +
        third.id + "&page=%2Fworkspaces");
    EXPECT_EQ(response_header(management, "location"), "/workspaces?" + query);
    const auto page = request("GET", "/workspaces?" + query);
    EXPECT_TRUE(page.starts_with("HTTP/1.1 200")) << page;
    EXPECT_NE(page.find("href=\"/workspaces\" aria-current=\"page\""), std::string::npos);
    for (const std::string field : {"Environment", "Phase", "Namespace", "Created", "staging", "Pending"}) {
        EXPECT_NE(page.find(field), std::string::npos) << field;
    }
    // Delete opens a confirmation that names what goes with the workspace, with counts (lake#514).
    EXPECT_NE(page.find("delete=" + third.id + "#delete-workspace"), std::string::npos);
    ASSERT_TRUE(store->create_repository({customer->id, "third-notes", "", "private", third.id}));
    const auto confirmation = request("GET", "/workspaces?" + query + "&delete=" + third.id);
    EXPECT_TRUE(confirmation.starts_with("HTTP/1.1 200")) << confirmation;
    EXPECT_NE(confirmation.find("name=\"workspace_id\" value=\"" + third.id + "\""),
              std::string::npos);
    EXPECT_NE(confirmation.find("1 repository (1 live, 0 deleted and still retained)"),
              std::string::npos)
        << confirmation;
    EXPECT_NE(confirmation.find("Repository <code>third-notes</code>"), std::string::npos);
    EXPECT_NE(confirmation.find("cannot be undone"), std::string::npos);
    EXPECT_EQ(store->get_workspace(third.id)->desired, "running");
    const auto refused =
        request("POST", "/workspaces/delete", query + "&workspace_id=" + third.id + "&confirm=wrong");
    EXPECT_NE(response_header(refused, "location").find("delete=" + third.id), std::string::npos)
        << "a wrong name returns to the confirmation";
    EXPECT_EQ(store->get_workspace(third.id)->desired, "running");
    request("POST", "/workspaces/delete", query + "&workspace_id=" + third.id + "&confirm=third");
    EXPECT_EQ(store->get_workspace(third.id)->desired, "deleted");
}

TEST(HttpServer, WorkspacePrefixServesRestOpenApiMcpAndAppSourceReads) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"taap:workspace-api", "", "Workspace API"});
    ASSERT_TRUE(user);
    const auto customer =
        store->create_customer({"individual", user->id, "", "workspace-api"});
    ASSERT_TRUE(customer);
    const auto workspace = store->create_workspace({customer->id, "apps", "dev", user->taap_id});
    const auto other_workspace =
        store->create_workspace({customer->id, "other", "dev", user->taap_id});
    ASSERT_TRUE(workspace);
    ASSERT_TRUE(other_workspace);
    const auto repository =
        store->create_repository({customer->id, "app-source", "", "private",
                                  lake::test::repository_workspace(*store, customer->id)});
    ASSERT_TRUE(repository);

    const std::filesystem::path root =
        lake::test::fixture_path("workspace-api-" + lake::control::new_uuid_v4());
    auto objects = liquid::runtime::block_on(liquid::store::open(root / customer->id / "store"));
    ASSERT_TRUE(objects) << objects.error().message;
    liquid::repository_options repository_options;
    repository_options.store = &*objects;
    auto source = liquid::runtime::block_on(liquid::repository::create(
        root / customer->id / "repositories" / repository->id, repository_options));
    ASSERT_TRUE(source) << source.error().message;
    const std::string source_text = "workspace app source\n";
    const auto* source_bytes = reinterpret_cast<const std::byte*>(source_text.data());
    const std::vector<std::byte> source_contents{
        source_bytes, source_bytes + source_text.size()};
    ASSERT_TRUE(liquid::runtime::block_on(source->write(
        "README.md", source_contents)));
    const auto change = liquid::runtime::block_on(source->cut("Add workspace app source"));
    ASSERT_TRUE(change) << change.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("main", *change)));

    lake::control::NewWebApp app_input;
    app_input.customer_id = customer->id;
    app_input.workspace_id = workspace->id;
    app_input.name = "source-reader";
    app_input.mode = "runtime";
    app_input.source_repository = repository->id;
    app_input.source_revision = "main";
    app_input.source_path = ".";
    app_input.port = 8080;
    app_input.host = "source-reader.apps.example.test";
    const auto app = store->create_web_app(app_input);
    ASSERT_TRUE(app);
    ASSERT_TRUE(store->set_web_app_token_hashes(
        app->id,
        {{"source", lake::control::identity_crypto::sha256_hex("workspace-source-token")}}));

    lake::api::ApiServer server({.port = 0,
                                 .store = store,
                                 .admin_token = "workspace-api-operator",
                                 .instance_kek = "workspace-api-instance-kek",
                                 .store_root = root.string()});
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    const auto request = [&](std::string_view method,
                             const std::string& path,
                             std::string body = {},
                             std::string_view bearer = "workspace-api-operator") {
        return connection.request(
            method, path, body, "", "HTTP/1.1", bearer, "application/json");
    };
    const auto body_json = [](const std::string& response) {
        return nlohmann::json::parse(response_body(response));
    };
    const std::string prefix = "/w/" + workspace->id;

    const auto unscoped_info = request("GET", "/v1/server_info");
    const auto scoped_info = request("GET", prefix + "/v1/server_info");
    ASSERT_TRUE(scoped_info.starts_with("HTTP/1.1 200")) << scoped_info;
    EXPECT_EQ(body_json(scoped_info)["value"], body_json(unscoped_info)["value"]);

    const std::string list_query = "?customer_id=" + customer->id;
    const auto unscoped_apps = request(
        "GET", "/v1/list_apps" + list_query + "&workspace_id=" + workspace->id);
    const auto scoped_apps = request("GET", prefix + "/v1/list_apps" + list_query);
    ASSERT_TRUE(scoped_apps.starts_with("HTTP/1.1 200")) << scoped_apps;
    EXPECT_EQ(body_json(scoped_apps)["value"], body_json(unscoped_apps)["value"]);
    const auto mismatch = request("GET",
                                  prefix + "/v1/list_apps" + list_query +
                                      "&workspace_id=" + other_workspace->id);
    EXPECT_TRUE(mismatch.starts_with("HTTP/1.1 400")) << mismatch;
    EXPECT_EQ(body_json(mismatch)["error"]["code"], "invalid_input");

    const auto unscoped_openapi = request("GET", "/v1/openapi.json");
    const auto scoped_openapi = request("GET", prefix + "/v1/openapi.json");
    ASSERT_TRUE(scoped_openapi.starts_with("HTTP/1.1 200")) << scoped_openapi;
    EXPECT_EQ(response_body(scoped_openapi), response_body(unscoped_openapi));

    const std::string tree_query =
        "?id=" + repository->id + "&ref=main";
    const auto tree = request(
        "GET", prefix + "/v1/repository_tree" + tree_query, "", "workspace-source-token");
    ASSERT_TRUE(tree.starts_with("HTTP/1.1 200")) << tree;
    ASSERT_TRUE(body_json(tree)["ok"].get<bool>()) << body_json(tree);
    EXPECT_EQ(body_json(tree)["value"]["items"][0]["path"], "README.md");
    const auto wrong_workspace = request("GET",
                                         "/w/" + other_workspace->id +
                                             "/v1/repository_tree" + tree_query,
                                         "",
                                         "workspace-source-token");
    EXPECT_TRUE(wrong_workspace.starts_with("HTTP/1.1 403")) << wrong_workspace;

    const nlohmann::json mcp_request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params",
         {{"name", "repository_tree"},
          {"arguments", {{"id", repository->id}, {"ref", "main"}}}}},
    };
    const auto mcp = request(
        "POST", prefix + "/mcp", mcp_request.dump(), "workspace-source-token");
    ASSERT_TRUE(mcp.starts_with("HTTP/1.1 200")) << mcp;
    EXPECT_EQ(body_json(mcp)["result"]["structuredContent"]["ok"], true);
    EXPECT_EQ(body_json(mcp)["result"]["structuredContent"]["value"]["items"][0]["path"],
              "README.md");

    const auto unknown = request(
        "GET", "/w/" + lake::control::new_uuid_v4() + "/v1/server_info");
    EXPECT_TRUE(unknown.starts_with("HTTP/1.1 404")) << unknown;
    EXPECT_EQ(body_json(unknown)["error"]["code"], "not_found");
}

// A workspace cluster that answers from a table, recording what reached it.
class RecordingWorkspace final : public lake::control::WorkspaceCaller {
public:
    std::vector<std::tuple<std::string, bool, nlohmann::json>> calls;
    std::map<std::string, nlohmann::json> answers;
    lake::test::StreamCaller topics;

    liquid::runtime::task<lake::control::ZettaControlResult<nlohmann::json>>
    call(std::string, std::string, std::string capability, bool read, nlohmann::json input)
        override {
        calls.emplace_back(capability, read, input);
        if (capability == "create_topic" || capability == "publish_event" ||
            capability == "receive_events" || capability == "update_topic" || capability == "drop_topic" ||
            lake::test::RecordTables::handles(capability, input)) {
            co_return lake::control::settled(co_await topics.call("http://workspace.test", "token", capability, read, input));
        }
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
            // The runner's database is private to this test (tests/test_main.cpp).
            database_uri_ = uri;
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
        options.apps_wildcard = "apps.example.test";
        options.apps_runtime_image = "oramasearch/app-runtime:20260930";
        options.internal_url = "http://lake-prod.lake.svc:80";
        options.apps_per_customer = 10;
        options.app_replicas_per_customer = 20;
        workspace_dir_ = lake::test::fixture_path("http-services-" + lake::control::new_uuid_v4());
        std::filesystem::create_directories(workspace_dir_);
        lake::control::WorkspaceConfig workspace_config{workspace_dir_.string(), 0};
        workspace_config.agent_endpoints = {{"agent-ask-orama", "http://ask:8080"},
                                            {"agent-project", "http://project:8080"},
                                            {"agent-application", "http://application:8080"},
                                            {"agent-experiments", "http://experiments:8080"},
                                            {"agent-autosuggest", "http://suggest:8080"},
                                            {"agent-creator", "http://creator:8080"},
                                            {"agent-eval-generator", "http://eval:8080"}};
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
    }

    void verify_agent_workspace(HttpConnection& connection,
                                const lake::control::AgentTask& task,
                                const std::string& credential) {
        const std::string selected_workspace = connection.workspace_id;
        connection.workspace_id.clear();
        const std::string zetta_openapi = connection.request(
            "GET", "/workspaces/" + task.customer_id + "/zetta/v1/openapi.json",
            "", "", "HTTP/1.1", credential);
        const std::string lake_openapi_response = connection.request(
            "GET", "/v1/openapi.json", "", "", "HTTP/1.1", credential);
        const std::string workspace_read = connection.request(
            "GET", "/workspaces/" + task.customer_id + "/zetta/v1/list_methodologies",
            "", "", "HTTP/1.1", credential);
        const std::string malformed = connection.request(
            "POST", "/v1/workspace_query", "[]", "", "HTTP/1.1", credential,
            "application/json");
        std::string app_list;
        if (task.agent == "agent-application") {
            app_list = connection.request(
                "GET", "/v1/list_apps?customer_id=" + task.customer_id,
                "", "", "HTTP/1.1", credential);
        }
        connection.workspace_id = selected_workspace;
        EXPECT_TRUE(zetta_openapi.starts_with("HTTP/1.1 200")) << zetta_openapi;
        EXPECT_TRUE(workspace_read.starts_with("HTTP/1.1 200")) << workspace_read;
        EXPECT_TRUE(malformed.starts_with("HTTP/1.1 400")) << malformed;
        if (task.agent == "agent-application") {
            EXPECT_TRUE(app_list.starts_with("HTTP/1.1 200")) << app_list;
        }
        ASSERT_TRUE(lake_openapi_response.starts_with("HTTP/1.1 200"))
            << lake_openapi_response;
        const nlohmann::json lake_openapi =
            nlohmann::json::parse(response_body(lake_openapi_response));
        const auto& parameters = lake_openapi["paths"]["/v1/list_apps"]["get"]["parameters"];
        const auto workspace_parameter =
            std::ranges::find_if(parameters, [](const nlohmann::json& parameter) {
                return parameter.value("name", std::string()) == "workspace_id";
            });
        ASSERT_NE(workspace_parameter, parameters.end());
        EXPECT_FALSE(workspace_parameter->value("required", true));
        const auto& required = lake_openapi["paths"]["/v1/workspace_query"]["post"]
                                           ["requestBody"]["content"]["application/json"]
                                           ["schema"]["required"];
        EXPECT_EQ(std::ranges::find(required, "workspace_id"), required.end());
    }

    // A library agent's part of a background turn (docs/api/agent-bus.md), done here through
    // lake's own capabilities with the agent service token: the task is received from the agent's
    // topic, claimed (its body arrives with the task's credential in place of "{{credential}}"),
    // handed to the scripted agent, and its answer published on agent-events. Lake's bus loop then
    // applies it. Answers the credential the agent was handed.
    std::string run_agent_task(HttpConnection& connection,
                              const std::string& task_id,
                              bool verify_workspace_inference = false) {
        const auto task = assembled_task(task_id);
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
            << "the service token user-created agents also hold never reaches the bus: " << shared;
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
        const std::string credential = body.value("lake_token", std::string());
        agents_->turns.push_back(body);
        if (verify_workspace_inference) {
            verify_agent_workspace(connection, *task, credential);
        }
        const nlohmann::json progress =
            call("POST",
                 "/v1/publish_agent_event",
                 {{"task_id", task_id},
                  {"agent", task->agent},
                  {"type", "progress"},
                  {"message", "Working on the task"}});
        EXPECT_EQ(progress["ok"], true) << progress.dump();
        const auto running = assembled_task(task_id);
        EXPECT_TRUE(running);
        if (running) {
            EXPECT_EQ(running->phase, "Running");
            EXPECT_EQ(running->message, "Working on the task");
        }
        // The control store keeps no word of it: the message is in the workspace (lake#375).
        EXPECT_EQ(store_->get_agent_task(task_id)->message, "");
        const nlohmann::json published =
            call("POST",
                 "/v1/publish_agent_event",
                 {{"task_id", task_id},
                  {"agent", task->agent},
                  {"type", "result"},
                  {"result", agents_->answer}});
        EXPECT_EQ(published["ok"], true) << published.dump();
        const auto finished = assembled_task(task_id);
        EXPECT_TRUE(finished);
        if (finished) {
            EXPECT_EQ(finished->phase, "Succeeded");
        }
        EXPECT_TRUE(liquid::runtime::block_on(agent_bus_->pump(std::chrono::milliseconds(0))));
        return credential;
    }

    // The task as lake assembles it: the control store's dispatch row with the request, message
    // and answer read from its record in the workspace (lake#374, lake#375).
    std::optional<lake::control::AgentTask> assembled_task(const std::string& id) {
        auto found = store_->get_agent_task(id);
        if (!found) {
            return std::nullopt;
        }
        if (const auto content = lake::test::agent_task_content(workspaces_, *found, instance_kek_)) {
            found->body = content->request.is_object() ? content->request
                                                       : nlohmann::json::object();
            found->result = content->result;
            if (found->message.empty()) {
                found->message = content->message;
            }
        }
        return *found;
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
        auto connection = std::make_unique<HttpConnection>(server_->port());
        connection->workspace_id = selected_workspace_id_;
        return connection;
    }

    void select_workspace(HttpConnection& connection, const std::string& customer_id) {
        const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
        ASSERT_TRUE(requested && *requested);
        connection.workspace_id = (*requested)->workspace_id;
        selected_workspace_id_ = connection.workspace_id;
        ready_service((*requested)->service_id);
    }

    // What the operator reports once a workspace's Zetta is up.
    void ready_service(const std::string& service_id) {
        const auto service = store_->get_service(service_id);
        ASSERT_TRUE(service);
        if (auto memory = std::dynamic_pointer_cast<lake::control::InMemoryStore>(store_)) {
            ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://workspace.test"));
        } else {
            pqxx::connection database(database_uri_);
            pqxx::work tx(database);
            tx.exec_params("INSERT INTO service_status (service_id, phase, endpoint) "
                           "VALUES ($1::uuid, 'Ready', 'http://workspace.test') "
                           "ON CONFLICT (service_id) DO UPDATE SET phase = 'Ready', endpoint = 'http://workspace.test'",
                           service->id);
            tx.commit();
        }
        std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    }

    std::shared_ptr<lake::control::Store> store_;
    std::string selected_workspace_id_;
    std::shared_ptr<RecordingWorkspace> workspace_caller_ = std::make_shared<RecordingWorkspace>();
    std::shared_ptr<ScriptedAgents> agents_ = std::make_shared<ScriptedAgents>();
    std::shared_ptr<lake::control::MemoryAgentBus> bus_ =
        std::make_shared<lake::control::MemoryAgentBus>();
    std::shared_ptr<lake::control::AgentTaskBus> agent_bus_;
    std::shared_ptr<lake::control::WorkspaceService> workspaces_;
    std::filesystem::path workspace_dir_;
    std::unique_ptr<lake::api::ApiServer> server_;
    std::string database_uri_;
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
    // A repository belongs to a workspace (lake#315): before the space has one, the page sends
    // the person to create it.
    const std::string page = client->request("GET", "/repositories", "", cookie);
    EXPECT_EQ(page.find("action=\"/repository/create\""), std::string::npos);
    EXPECT_NE(page.find("<a href=\"/workspaces\">Create a workspace</a> first"),
              std::string::npos);
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
    // No transfer and no builds yet: no hourly rows, and storage is measured live (lake#348).
    EXPECT_TRUE(usage.at("value").at("items").empty());
    EXPECT_EQ(usage.at("value").at("summary").at("storage").at("repositories_bytes"), 0);
    EXPECT_FALSE(usage.at("value").at("over_quota"));
    const auto workspace = store_->create_workspace({customer_id, "notes", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    // The description is written to the workspace's Zetta (lake#315), so it must be Ready.
    ready_service(workspace->zetta_service_id);
    const std::string form = client->request("GET", "/repositories", "", cookie);
    EXPECT_NE(form.find("action=\"/repository/create\""), std::string::npos);
    EXPECT_NE(form.find("<select name=\"workspace_id\" required>"), std::string::npos);
    EXPECT_NE(form.find("value=\"" + workspace->id + "\""), std::string::npos);
    // The workspace names its space, so the form sends only the workspace.
    const std::string created = client->request("POST",
                                                "/repository/create",
                                                "workspace_id=" + workspace->id +
                                                    "&name=first-repository&description=My+files",
                                                cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 302")) << created;
    const std::string destination = response_header(created, "location");
    ASSERT_TRUE(destination.starts_with("/repository?id="));
    const std::string repository = client->request("GET", destination, "", cookie);
    EXPECT_TRUE(repository.starts_with("HTTP/1.1 200")) << repository;
    EXPECT_NE(repository.find("/first-repository"), std::string::npos);
    EXPECT_NE(repository.find("My files"), std::string::npos) << "the description, from the workspace";
    // The repository page names its workspace and links into it.
    EXPECT_NE(repository.find("href=\"/w/" + workspace->id + "/design/projects\">notes</a>"),
              std::string::npos);
    EXPECT_NE(repository.find("name=\"workspace_id\" value=\"" + workspace->id + "\""),
              std::string::npos);
    const std::string missing = client->request(
        "POST", "/repository/create", "customer_id=" + customer_id + "&name=no-workspace", cookie);
    EXPECT_FALSE(response_header(missing, "location").starts_with("/repository?id="));
    const std::string other_cookie = "lake_session=invalid";
    const std::string denied = client->request("POST",
                                               "/repository/create",
                                               "workspace_id=" + workspace->id + "&name=forbidden",
                                               other_cookie);
    EXPECT_FALSE(denied.starts_with("HTTP/1.1 200"));
    EXPECT_FALSE(response_header(denied, "location").starts_with("/repository?id="));
}

namespace {

nlohmann::json response_json(const std::string& response) {
    return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4));
}

} // namespace

TEST_P(HttpResponseIsolation, PersonCreatesAnOrganizationInvitesAndAColleagueJoins) {
    auto client = connect();
    const std::string sign_in =
        client->request("POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302")) << sign_in;
    const std::string owner = response_header(sign_in, "set-cookie")
                                  .substr(0, response_header(sign_in, "set-cookie").find(';'));
    const std::string empty = client->request("GET", "/organizations", "", owner);
    ASSERT_TRUE(empty.starts_with("HTTP/1.1 200")) << empty;
    EXPECT_NE(empty.find("action=\"/organizations/create\""), std::string::npos);
    EXPECT_NE(empty.find("action=\"/organizations/convert\""), std::string::npos);
    EXPECT_NE(empty.find("You are not in an organization yet"), std::string::npos);
    EXPECT_EQ(empty.find("POST /v1/"), std::string::npos);

    const std::string handle = "acme-" + lake::control::new_uuid_v4().substr(0, 8);
    const std::string created = client->request(
        "POST", "/organizations/create", "display_name=Acme+Research&name=" + handle, owner);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 302")) << created;
    const std::string settings = response_header(created, "location");
    ASSERT_TRUE(settings.starts_with("/organization?id=")) << settings;
    ASSERT_TRUE(settings.ends_with("&tab=invitations")) << settings;
    const std::string organization_id =
        settings.substr(17, settings.size() - 17 - std::string("&tab=invitations").size());
    const std::string invite_page = client->request("GET", settings, "", owner);
    ASSERT_TRUE(invite_page.starts_with("HTTP/1.1 200")) << invite_page;
    EXPECT_NE(invite_page.find("Send invitation"), std::string::npos);
    EXPECT_NE(invite_page.find("name=\"expires_in_days\""), std::string::npos);
    const std::string listed = client->request("GET", "/organizations", "", owner);
    EXPECT_NE(listed.find("Acme Research"), std::string::npos);
    EXPECT_NE(listed.find(">owner<"), std::string::npos);

    if (std::get<0>(GetParam())) {
        create_identity_document();
    }
    const std::string colleague_email =
        "colleague-" + lake::control::new_uuid_v4() + "@example.test";
    const std::string signup =
        client->request("POST",
                        "/identity/sign-up",
                        "email=" + colleague_email + "&display_name=Colleague&password=" +
                            password_ + "&device_name=browser&identity_document=" +
                            identity_document_);
    ASSERT_TRUE(signup.starts_with("HTTP/1.1 302")) << signup;
    const std::string colleague = response_header(signup, "set-cookie")
                                      .substr(0, response_header(signup, "set-cookie").find(';'));

    const std::string invited = client->request("POST",
                                                "/organization/invitations",
                                                "organization_id=" + organization_id +
                                                    "&invitee_type=email&invitee=" +
                                                    colleague_email +
                                                    "&role=admin&expires_in_days=7",
                                                owner);
    ASSERT_TRUE(invited.starts_with("HTTP/1.1 302")) << invited;
    const std::string pending_page = client->request("GET", settings, "", owner);
    EXPECT_NE(pending_page.find(colleague_email), std::string::npos);

    const std::string colleague_page = client->request("GET", "/organizations", "", colleague);
    EXPECT_NE(colleague_page.find("Invitations for You"), std::string::npos);
    EXPECT_NE(colleague_page.find("Acme Research"), std::string::npos);
    EXPECT_NE(colleague_page.find("action=\"/invitations/accept\""), std::string::npos);
    const auto invitations =
        response_json(client->request("GET", "/v1/list_invitations", "", colleague));
    ASSERT_TRUE(invitations.at("ok")) << invitations.dump();
    ASSERT_EQ(invitations.at("value").at("items").size(), 1U);
    const std::string invitation_id = invitations.at("value").at("items").at(0).at("id");
    EXPECT_EQ(invitations.at("value").at("items").at(0).at("organization_name"), "Acme Research");

    const std::string accepted = client->request(
        "POST", "/invitations/accept", "invitation_id=" + invitation_id, colleague);
    ASSERT_TRUE(accepted.starts_with("HTTP/1.1 302")) << accepted;
    EXPECT_EQ(response_header(accepted, "location"), "/organization?id=" + organization_id);
    const std::string organization =
        client->request("GET", "/organization?id=" + organization_id, "", colleague);
    EXPECT_TRUE(organization.starts_with("HTTP/1.1 200")) << organization;
    const auto memberships =
        response_json(client->request("GET", "/v1/list_memberships", "", colleague));
    ASSERT_TRUE(memberships.at("ok")) << memberships.dump();
    ASSERT_EQ(memberships.at("value").at("items").size(), 1U);
    EXPECT_EQ(memberships.at("value").at("items").at(0).at("role"), "admin");
    const auto spaces =
        response_json(client->request("GET", "/v1/list_customers", "", colleague));
    ASSERT_TRUE(spaces.at("ok"));
    bool organization_space = false;
    for (const auto& space : spaces.at("value").at("items")) {
        organization_space = organization_space || space.at("name") == handle;
    }
    EXPECT_TRUE(organization_space) << spaces.dump();
}

TEST_P(HttpResponseIsolation, PersonalSpaceBecomesAnOrganizationAfterConfirmation) {
    auto client = connect();
    const std::string sign_in =
        client->request("POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302")) << sign_in;
    const std::string cookie = response_header(sign_in, "set-cookie")
                                   .substr(0, response_header(sign_in, "set-cookie").find(';'));
    const auto before = response_json(client->request("GET", "/v1/list_customers", "", cookie));
    ASSERT_TRUE(before.at("ok"));
    ASSERT_EQ(before.at("value").at("items").size(), 1U);
    const std::string customer_id = before.at("value").at("items").at(0).at("id");
    const std::string handle = before.at("value").at("items").at(0).at("name");

    const std::string refused = client->request(
        "POST",
        "/organizations/convert",
        "customer_id=" + customer_id + "&display_name=Solo+Labs&confirm=not-the-handle",
        cookie);
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 400")) << refused;
    const auto unchanged = response_json(
        client->request("GET", "/v1/get_customer?customer_id=" + customer_id, "", cookie));
    ASSERT_TRUE(unchanged.at("ok")) << unchanged.dump();
    EXPECT_EQ(unchanged.at("value").at("customer_type"), "individual");

    const std::string converted = client->request(
        "POST",
        "/organizations/convert",
        "customer_id=" + customer_id + "&display_name=Solo+Labs&confirm=" + handle,
        cookie);
    ASSERT_TRUE(converted.starts_with("HTTP/1.1 302")) << converted;
    const auto after = response_json(
        client->request("GET", "/v1/get_customer?customer_id=" + customer_id, "", cookie));
    ASSERT_TRUE(after.at("ok")) << after.dump();
    EXPECT_EQ(after.at("value").at("customer_type"), "organization");
    EXPECT_EQ(after.at("value").at("name"), handle);
    const auto spaces = response_json(client->request("GET", "/v1/list_customers", "", cookie));
    ASSERT_TRUE(spaces.at("ok"));
    EXPECT_EQ(spaces.at("value").at("items").size(), 2U) << spaces.dump();
    const auto memberships =
        response_json(client->request("GET", "/v1/list_memberships", "", cookie));
    ASSERT_TRUE(memberships.at("ok")) << memberships.dump();
    ASSERT_EQ(memberships.at("value").at("items").size(), 1U);
    EXPECT_EQ(memberships.at("value").at("items").at(0).at("role"), "owner");
    const std::string page = client->request("GET", "/organizations", "", cookie);
    EXPECT_NE(page.find("Solo Labs"), std::string::npos);
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
            EXPECT_EQ(nav.contains("href=\"/ship/apps\""), signed_in) << path;
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

// lake#534: a real long poll over REST gives its HTTP worker back (counted once in
// lake_http_long_requests_total); the same capability without a wait keeps it.
TEST_P(HttpResponseIsolation, ALongPollOverRestGivesItsWorkerBack) {
    HttpConnection connection(server_->port());
    const auto long_requests = [&]() -> std::string {
        const std::string metrics = connection.request("GET", "/metrics", "", "", "HTTP/1.1",
                                                       "front-door-operator");
        const auto at = metrics.find("\nlake_http_long_requests_total ");
        return at == std::string::npos
                   ? std::string("0")
                   : metrics.substr(at + 31, metrics.find('\n', at + 1) - at - 31);
    };
    EXPECT_EQ(long_requests(), "0");
    const std::string immediate = connection.request(
        "GET", "/v1/receive_agent_tasks?agent=agent-project&wait_ms=0", "", "", "HTTP/1.1",
        "bus-token-agent-project");
    ASSERT_TRUE(immediate.starts_with("HTTP/1.1 200")) << immediate;
    EXPECT_EQ(long_requests(), "0");
    const std::string parked = connection.request(
        "GET", "/v1/receive_agent_tasks?agent=agent-project&wait_ms=200", "", "", "HTTP/1.1",
        "bus-token-agent-project");
    ASSERT_TRUE(parked.starts_with("HTTP/1.1 200")) << parked;
    EXPECT_EQ(long_requests(), "1");
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
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
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
    EXPECT_EQ(document["servers"][0]["url"],
              "/w/" + connection->workspace_id + "/workspaces/" + customer_id + "/zetta");
}

TEST_P(HttpResponseIsolation, LibraryAgentsFinishEveryTurnKindWithoutAWorkspaceEcho) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const nlohmann::json customers = nlohmann::json::parse(
        response_body(connection->request("GET", "/v1/list_customers", "", cookie)));
    const std::string customer_id = customers["value"]["items"][0]["id"];
    select_workspace(*connection, customer_id);

    const std::vector<std::pair<std::string, std::string>> turns = {
        {"agent-application", "app"},
        {"agent-project", "solution:retention"},
        {"agent-experiments", "experiment"},
    };
    for (const auto& [agent, kind] : turns) {
        const nlohmann::json input = {{"customer_id", customer_id},
                                      {"workspace_id", connection->workspace_id},
                                      {"agent", agent},
                                      {"kind", kind},
                                      {"message", "Run the regression turn"}};
        const nlohmann::json started = nlohmann::json::parse(response_body(connection->request(
            "POST",
            "/v1/agent_turn",
            input.dump(),
            cookie,
            "HTTP/1.1",
            "",
            "application/json")));
        ASSERT_EQ(started["ok"], true) << started.dump();
        const std::string task_id = started["value"]["task_id"];
        const auto task = assembled_task(task_id);
        ASSERT_TRUE(task);
        EXPECT_EQ(task->kind, kind);
        const auto dispatch = bus_->events("agent-tasks." + agent);
        const lake::control::BusEvent* event = nullptr;
        for (const auto& row : dispatch) {
            if (row.correlation_id == task_id) {
                event = &row;
                break;
            }
        }
        ASSERT_NE(event, nullptr);
        EXPECT_EQ(event->payload["workspace_id"], connection->workspace_id);

        agents_->answer = {{"ok", true},
                           {"reply", "The turn finished."},
                           {"result", {{"kind", kind}}}};
        run_agent_task(*connection, task_id, true);
        const auto finished = assembled_task(task_id);
        ASSERT_TRUE(finished);
        EXPECT_EQ(finished->phase, "Succeeded");
        EXPECT_EQ(finished->result["reply"], "The turn finished.");
    }
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
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
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

    select_workspace(*connection, customer_id);
    const std::string home = connection->request("GET", "/repositories", "", cookie);
    EXPECT_NE(home.find("href=\"/w/" + connection->workspace_id + "/ask?from=%2Frepositories\""), std::string::npos)
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
    EXPECT_NE(thinking.find("Waiting for Ask Orama to start"), std::string::npos)
        << "the page follows the task";
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
    EXPECT_NE(page.find("action=\"/w/" + connection->workspace_id + "/ask/confirm\""), std::string::npos);
    EXPECT_FALSE(store_->list_repositories(customer_id, 10).value().size() > 0)
        << "nothing is created before the person confirms";

    // The conversation is in the workspace's Zetta (lake#370), read as the person.
    const std::string listed = connection->request(
        "GET",
        "/v1/get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" +
            connection->workspace_id + "&conversation_id=" + conversation,
        "", cookie);
    const auto messages =
        nlohmann::json::parse(listed.substr(listed.find("\r\n\r\n") + 4))["value"]["messages"];
    ASSERT_EQ(messages.size(), 2U) << listed.substr(0, 400);
    const std::string message_id = messages.at(1).at("id");
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

// lake#451: Talk to offers only agents a person uses, and a long turn says which agent is working
// and what it last reported, including the agent Ask Orama brought in.
TEST_P(HttpResponseIsolation, AskOramaOffersPeoplesAgentsAndSaysWhoIsWorkingOnALongTurn) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 302"));
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customer_id =
        nlohmann::json::parse(response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);

    const std::string fresh =
        connection->request("GET", "/ask?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(fresh.starts_with("HTTP/1.1 200")) << fresh.substr(0, 300);
    EXPECT_NE(fresh.find("<option value=\"agent-experiments\""), std::string::npos);
    EXPECT_NE(fresh.find("<option value=\"agent-autosuggest\""), std::string::npos);
    EXPECT_EQ(fresh.find("agent-eval-generator"), std::string::npos)
        << "a running internal tool is not offered to talk to";
    EXPECT_EQ(fresh.find("Eval Generator"), std::string::npos);

    const auto bus = [&](const std::string& agent, const std::string& path, const nlohmann::json& body) {
        return nlohmann::json::parse(response_body(connection->request(
            "POST", path, body.dump(), "", "HTTP/1.1", "bus-token-" + agent, "application/json")));
    };
    const auto page = [&](const std::string& conversation, const std::string& task) {
        return connection->request(
            "GET",
            "/ask?customer_id=" + customer_id + "&conversation=" + conversation + "&task=" + task,
            "", cookie);
    };
    const std::string conversation = lake::control::new_uuid_v4();
    const std::string sent = connection->request(
        "POST",
        "/ask/send",
        "customer_id=" + customer_id + "&conversation=" + conversation +
            "&agent=agent-ask-orama&message=what+should+I+build%3F",
        cookie);
    ASSERT_TRUE(sent.starts_with("HTTP/1.1 302")) << sent.substr(0, 300);
    const std::string task = task_of(sent);
    ASSERT_FALSE(task.empty());
    EXPECT_NE(page(conversation, task).find("Waiting for Ask Orama to start"), std::string::npos);

    const nlohmann::json claimed =
        bus("agent-ask-orama", "/v1/claim_agent_task", {{"task_id", task}, {"agent", "agent-ask-orama"}});
    ASSERT_EQ(claimed["ok"], true) << claimed.dump();
    const std::string credential = claimed["value"]["body"].value("lake_token", std::string());
    ASSERT_EQ(bus("agent-ask-orama", "/v1/publish_agent_event",
                  {{"task_id", task}, {"agent", "agent-ask-orama"}, {"type", "progress"},
                   {"message", "Working on it"}})["ok"],
              true);
    const std::string working = page(conversation, task);
    EXPECT_NE(working.find("Ask Orama is working…"), std::string::npos) << working;
    EXPECT_NE(working.find(" · for "), std::string::npos) << "how long it has worked";

    // Ask Orama brings in Auto-Suggest with its turn key; the asking turn reports it.
    const std::string delegated = connection->request(
        "POST", "/v1/agent_turn",
        nlohmann::json{{"customer_id", customer_id},
                       {"workspace_id", connection->workspace_id},
                       {"agent", "agent-autosuggest"},
                       {"message", "What is worth building?"}}
            .dump(),
        "", "HTTP/1.1", credential, "application/json");
    ASSERT_TRUE(delegated.starts_with("HTTP/1.1 200")) << delegated;
    const std::string child =
        nlohmann::json::parse(response_body(delegated))["value"].value("task_id", std::string());
    ASSERT_FALSE(child.empty()) << delegated;
    EXPECT_EQ(assembled_task(child)->body.value("parent_task_id", std::string()), task);
    EXPECT_EQ(store_->get_agent_task(child)->parent_task_id, task) << "routing stays central";
    EXPECT_EQ(store_->get_agent_task(task)->message, "") << "the report is in the workspace";
    EXPECT_EQ(assembled_task(task)->message,
              "Ask Orama asked Auto-Suggest: What is worth building?");
    EXPECT_NE(page(conversation, task).find("Ask Orama asked Auto-Suggest: What is worth building?"),
              std::string::npos);

    ASSERT_EQ(bus("agent-autosuggest", "/v1/claim_agent_task",
                  {{"task_id", child}, {"agent", "agent-autosuggest"}})["ok"],
              true);
    ASSERT_EQ(bus("agent-autosuggest", "/v1/publish_agent_event",
                  {{"task_id", child}, {"agent", "agent-autosuggest"}, {"type", "progress"},
                   {"message", "Reading 4 datasets"}})["ok"],
              true);
    EXPECT_NE(page(conversation, task).find("Auto-Suggest: Reading 4 datasets"), std::string::npos)
        << "the subagent's progress shows on the person's turn";
    bool published = false;
    for (const auto& [name, read, input] : workspace_caller_->calls) {
        if (name == "publish_event" && input.value("correlation_id", std::string()) == task) {
            const nlohmann::json event = nlohmann::json::parse(input.value("payload", std::string("{}")));
            published = published || (event.value("delegated_task_id", std::string()) == child &&
                                       event.value("message", std::string()) ==
                                           "Auto-Suggest: Reading 4 datasets");
        }
    }
    EXPECT_TRUE(published) << "the asking turn's progress is on the workspace's agent-events";

    ASSERT_EQ(bus("agent-autosuggest", "/v1/publish_agent_event",
                  {{"task_id", child}, {"agent", "agent-autosuggest"}, {"type", "result"},
                   {"result", {{"ok", true}, {"reply", "Churn"}}}})["ok"],
              true);
    EXPECT_NE(page(conversation, task).find("Auto-Suggest finished; writing the answer."),
              std::string::npos);
    ASSERT_EQ(bus("agent-ask-orama", "/v1/publish_agent_event",
                  {{"task_id", task}, {"agent", "agent-ask-orama"}, {"type", "result"},
                   {"result", {{"ok", true}, {"reply", "Build churn."}}}})["ok"],
              true);
    const std::string answered = page(conversation, task);
    EXPECT_NE(answered.find("Build churn."), std::string::npos);
    EXPECT_EQ(answered.find("is working…"), std::string::npos);
}

// lake#446: Ask Orama, Train > Agents and Search Indexes render without waiting for Zetta's slow
// reads; the deferred panels fill them in afterwards.
TEST_P(HttpResponseIsolation, SlowZettaPanelsLoadAfterTheirPage) {
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
    select_workspace(*connection, customer_id);
    workspace_caller_->answers["list_agents"] = {
        {"agents",
         nlohmann::json::array({{{"id", "a-1"}, {"name", "Renewal watcher"}, {"kind", "custom"},
                                 {"chat_capable", true}},
                                {{"id", "a-2"}, {"name", "Batch scorer"}, {"kind", "custom"},
                                 {"chat_capable", false}}})}};

    const auto list_agents_calls = [&] {
        return std::ranges::count_if(workspace_caller_->calls, [](const auto& call) {
            return std::get<0>(call) == "list_agents";
        });
    };
    const auto before = list_agents_calls();
    const std::string empty = connection->request(
        "GET", "/ask?customer_id=" + customer_id + "&agent=a-1", "", cookie);
    ASSERT_TRUE(empty.starts_with("HTTP/1.1 200")) << empty.substr(0, 300);
    EXPECT_EQ(list_agents_calls(), before) << "the page never waits for Zetta's list_agents";
    EXPECT_NE(empty.find("data-workspace-agents"), std::string::npos);
    EXPECT_NE(empty.find("fetch('/w/" + connection->workspace_id + "/ask/agents?customer_id="),
              std::string::npos)
        << "the picker loads the space's agents from the deferred panel";
    EXPECT_NE(empty.find("<option value=\"a-1\" selected>"), std::string::npos)
        << "the conversation's agent stays selected while the list loads";

    const std::string panel =
        connection->request("GET", "/ask/agents?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(panel.starts_with("HTTP/1.1 200")) << panel.substr(0, 300);
    EXPECT_TRUE(response_header(panel, "content-type").starts_with("application/json"));
    EXPECT_NE(response_header(panel, "server-timing").find("zetta_list_agents;dur="),
              std::string::npos)
        << "the panel names the Zetta read it waited on";
    const auto agents = nlohmann::json::parse(panel.substr(panel.find("\r\n\r\n") + 4));
    EXPECT_EQ(agents["agents"],
              nlohmann::json::array({{{"id", "a-1"}, {"name", "Renewal watcher"}}}))
        << "only the space's own agents that hold a conversation";

    // Train > Agents and Search Indexes defer their slow panels the same way.
    const auto before_agents_page = list_agents_calls();
    const std::string agents_page = connection->request(
        "GET", "/train/agents?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(agents_page.starts_with("HTTP/1.1 200")) << agents_page.substr(0, 300);
    EXPECT_EQ(list_agents_calls(), before_agents_page);
    EXPECT_NE(agents_page.find("Loading your agents"), std::string::npos);
    EXPECT_NE(agents_page.find("fetch('/w/" + connection->workspace_id + "/train/agents/list?"),
              std::string::npos);
    const std::string agents_panel = connection->request(
        "GET", "/train/agents/list?customer_id=" + customer_id, "", cookie);
    const auto rendered =
        nlohmann::json::parse(agents_panel.substr(agents_panel.find("\r\n\r\n") + 4));
    const std::string agents_html = rendered.value("html", std::string());
    EXPECT_NE(agents_html.find("Renewal watcher"), std::string::npos) << rendered.dump();
    EXPECT_NE(agents_html.find("href=\"/w/" + connection->workspace_id + "/train/agent?"),
              std::string::npos)
        << "the panel's links keep the workspace";
    const std::string inline_agents = connection->request(
        "GET", "/train/agents?customer_id=" + customer_id + "&inline=1", "", cookie);
    EXPECT_NE(inline_agents.find("Renewal watcher"), std::string::npos)
        << "without JavaScript the page can wait for the list";
    const std::string indexes_page = connection->request(
        "GET", "/ship/indexes?customer_id=" + customer_id, "", cookie);
    EXPECT_NE(indexes_page.find("Loading your datasets"), std::string::npos);
    const std::string indexes_panel = connection->request(
        "GET", "/ship/indexes/datasets?customer_id=" + customer_id, "", cookie);
    EXPECT_NE(indexes_panel.find("\"html\""), std::string::npos) << indexes_panel;
    EXPECT_NE(response_header(indexes_panel, "server-timing").find("zetta_sql;dur="),
              std::string::npos)
        << "the dataset choices come from the projected system.datasets read";

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
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
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

// lake#290/#291/#292: Design lists the engine registries; a solution starts with the person's
// business inputs, then the Project Agent's editable foundation is confirmed before creation.
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
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
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
    EXPECT_NE(methods.find("href=\"/w/" + connection->workspace_id + "/design/methodologies\""), std::string::npos) << "in the nav";
    EXPECT_NE(methods.find("href=\"/w/" + connection->workspace_id + "/design/solutions\""), std::string::npos) << "in the nav";
    // The page's own nav item is the current one, not Projects (Steve, 2026-09-28).
    EXPECT_NE(methods.find("href=\"/w/" + connection->workspace_id + "/design/methodologies\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(methods.find("href=\"/w/" + connection->workspace_id + "/design/projects\" aria-current=\"page\""), std::string::npos);
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
    workspace_caller_->answers["list_datasets"] = {
        {"datasets", nlohmann::json::array({{{"database", "datasets"}, {"name", "customers"}}})}};
    // The scripted engine has gained its solution registry, as after a deployment.
    workspaces_->invalidate_registry(customer_id, service->workspace_id);
    const std::string catalogue = connection->request(
        "GET", "/design/solutions?customer_id=" + customer_id, "", cookie);
    EXPECT_NE(catalogue.find("S-1 Churn &amp; Retention"), std::string::npos);
    EXPECT_NE(catalogue.find("In Design"), std::string::npos);
    EXPECT_NE(catalogue.find("href=\"/w/" + connection->workspace_id + "/design/solutions\" aria-current=\"page\""), std::string::npos);
    EXPECT_EQ(catalogue.find("href=\"/w/" + connection->workspace_id + "/design/projects\" aria-current=\"page\""), std::string::npos);
    const std::string module = connection->request(
        "GET", "/design/solution?customer_id=" + customer_id + "&id=S-1", "", cookie);
    EXPECT_NE(module.find("tasks in CRM"), std::string::npos);
    EXPECT_NE(module.find("level 2+"), std::string::npos) << "attachable heads show their level";
    EXPECT_NE(module.find("action=\"/w/" + connection->workspace_id + "/design/solutions/start\""), std::string::npos);
    EXPECT_NE(module.find("datasets.customers"), std::string::npos);
    EXPECT_NE(module.find("Value per outcome"), std::string::npos);
    workspace_caller_->answers["create_project"] = {{"name", "churn"}};
    workspace_caller_->answers["get_project"] = {
        {"name", "churn"}, {"description", "Solution S-1"}, {"graphs", nlohmann::json::array()}};
    const std::string started = connection->request(
        "POST",
        "/design/solutions/start",
        "customer_id=" + customer_id +
            "&solution=S-1&name=churn&level=1&dataset=datasets.customers&delivery_kind=webhook"
            "&delivery_endpoint=https%3A%2F%2Fcrm.example.test%2Ftasks&owner=retention-team"
            "&value_per_outcome=125&investment=4000&currency=USD",
        cookie);
    ASSERT_TRUE(started.starts_with("HTTP/1.1 302")) << started.substr(0, 300);
    EXPECT_NE(response_header(started, "location").find("/design/project?"), std::string::npos);
    const auto latest =
        store_->latest_agent_task(customer_id, "solution:churn", connection->workspace_id);
    ASSERT_TRUE(latest) << "Start launches the Project Agent";
    const auto task = assembled_task(latest->id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->body["context"]["delivery"]["owner"], "retention-team");
    EXPECT_EQ(task->body["context"]["roi_model"]["investment"], 4000.0);
    EXPECT_TRUE(std::ranges::none_of(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "create_solution";
    })) << "Start does not create the solution";
    const std::string proposing = connection->request(
        "GET", "/design/project?customer_id=" + customer_id + "&name=churn", "", cookie);
    EXPECT_NE(proposing.find("Project Agent is reading the dataset"), std::string::npos);

    agents_->answer = {
        {"ok", true},
        {"result",
         {{"foundation",
           {{"sources",
             {{"source", "datasets"},
              {"table", "customers"},
              {"M-2", {{"source", "crm"}, {"record_id", "id"}, {"name", "account"}}}}},
            {"snapshot",
             {{"database", "datasets"},
              {"source_table", "customers"},
              {"target_table", "churn_snapshot"},
              {"entity_column", "entity_id"},
              {"time_column", "observed_at"},
              {"as_of", "2026-09-28T00:00:00Z"}}},
            {"dataset",
             {{"entity_column", "entity_id"},
              {"label_column", "churned"},
              {"feature_columns", nlohmann::json::array({"usage"})}}},
            {"predicates",
             {{"in_collections", "in_collections"},
              {"open_complaint", "open_complaint"},
              {"vulnerable", "vulnerable"}}}}}}}};
    run_agent_task(*connection, task->id);
    const std::string proposal = connection->request(
        "GET", "/design/project?customer_id=" + customer_id + "&name=churn", "", cookie);
    EXPECT_NE(proposal.find("Foundation Proposal"), std::string::npos);
    EXPECT_NE(proposal.find("retention-team"), std::string::npos);
    EXPECT_NE(proposal.find("Confirm and create solution"), std::string::npos);

    workspace_caller_->answers["create_solution"] = {{"project", "churn"},
                                                     {"solution", "S-1"},
                                                     {"level", 1},
                                                     {"graph", "S-1 CL-1"},
                                                     {"policy_id", "policy-1"},
                                                     {"experiment_id", "experiment-1"},
                                                     {"monitor_id", "monitor-1"},
                                                     {"readout_id", "readout-1"}};
    nlohmann::json edited = agents_->answer["result"]["foundation"];
    edited["dataset"]["feature_columns"].push_back("tenure");
    const std::string confirmed = connection->request(
        "POST",
        "/design/solution/confirm",
        "customer_id=" + customer_id + "&project=churn&task_id=" + task->id +
            "&confirm=yes&foundation=" + lake::api::oauth_url_encode(edited.dump()),
        cookie);
    ASSERT_TRUE(confirmed.starts_with("HTTP/1.1 302")) << confirmed.substr(0, 300);
    const auto created = std::ranges::find_if(workspace_caller_->calls, [](const auto& call) {
        return std::get<0>(call) == "create_solution";
    });
    ASSERT_NE(created, workspace_caller_->calls.end());
    EXPECT_EQ(std::get<2>(*created)["foundation"]["dataset"]["feature_columns"],
              nlohmann::json::array({"usage", "tenure"}));
    EXPECT_EQ(std::get<2>(*created)["foundation"]["roi_model"]["value_per_outcome"], 125.0);

    workspace_caller_->answers["get_project"]["graphs"] =
        nlohmann::json::array({{{"id", "graph-1"}, {"name", "S-1 CL-1"}}});
    workspace_caller_->answers["get_graph"] = {
        {"graph_id", "graph-1"},
        {"name", "S-1 CL-1"},
        {"nodes",
         nlohmann::json::array(
             {{{"id", "solution_manifest"},
               {"name", "Solution Manifest"},
               {"kind", "transform"},
               {"attributes",
                {{"sources", {{"missing_policy_predicates", nlohmann::json::array()}}},
                 {"foundation",
                  {{"scope", "project-1/S-1 CL-1"},
                   {"policy_id", "policy-1"},
                   {"experiment_id", "experiment-1"},
                   {"monitor_id", "monitor-1"}}}}}},
              {{"id", "stage-1"},
               {"name", "Propensity"},
               {"kind", "model"},
               {"attributes", {{"solution_stage", 1}}}}})},
        {"edges", nlohmann::json::array()}};
    workspace_caller_->answers["get_policy"] = {
        {"id", "policy-1"},
        {"versions", nlohmann::json::array({{{"version", 1}, {"config", {{"threshold", 0.8}}}}})}};
    workspace_caller_->answers["get_experiment"] = {
        {"allocations", nullptr},
        {"sizes", {{"assignments", 1676}, {"holdout_assignments", 165}}},
        {"balance",
         {{"balanced", true},
          {"allocations",
           nlohmann::json::array({{{"name", "project-1/S-1 CL-1/holdout"},
                                   {"tier", "holdout"},
                                   {"configured_fraction", 0.1},
                                   {"assignments", 165},
                                   {"expected", 167.6},
                                   {"balanced", true}},
                                  {{"name", "project-1/S-1 CL-1/csm_task"},
                                   {"tier", "treatment"},
                                   {"configured_fraction", 0.45},
                                   {"assignments", 754},
                                   {"expected", 754.2},
                                   {"balanced", true}},
                                  {{"name", "project-1/S-1 CL-1/retention_journey"},
                                   {"tier", "treatment"},
                                   {"configured_fraction", 0.45},
                                   {"assignments", 757},
                                   {"expected", 754.2},
                                   {"balanced", true}}})}}}};
    workspace_caller_->answers["list_ledger"] = {
        {"entries",
         nlohmann::json::array({{{"id", "action-1"},
                                 {"kind", "action"},
                                 {"subject", "account-17"},
                                 {"score", 0.91},
                                 {"decision", "allowed"},
                                 {"action", "send_offer"},
                                 {"model_version", "run:model-v7"},
                                 {"policy_version", "policy-1:1"},
                                 {"cell", "project-1/S-1 CL-1/csm_task"},
                                 {"currency", "USD"},
                                 {"recorded_at", "2026-09-28T12:00:00Z"}},
                                {{"id", "action-pending"},
                                 {"kind", "action"},
                                 {"subject", "account-18"},
                                 {"score", 0.89},
                                 {"decision", "decision-1"},
                                 {"action", "no_contact"},
                                 {"model_version", "run:model-v7"},
                                 {"policy_version", "1"},
                                 {"cell", "project-1/S-1 CL-1/retention_journey"},
                                 {"currency", "USD"},
                                 {"recorded_at", "2026-09-28T11:59:00Z"}},
                                {{"id", "other-action"},
                                 {"kind", "action"},
                                 {"subject", "other-project-account"},
                                 {"decision", "other-decision"},
                                 {"action", "send_offer"},
                                 {"cell", "project-2/S-1 CL-1/csm_task"},
                                 {"currency", "USD"},
                                 {"recorded_at", "2026-09-28T12:00:00Z"}}})}};
    workspace_caller_->answers["get_monitor"] = {{"status", "within_bounds"}};
    workspace_caller_->answers["get_roi_readout"] = {
        {"solution", "S-1"},
        {"incrementality",
         {{"claimable", false},
          {"reason", "Incrementality cannot be claimed: no holdout outcomes are available."}}},
        {"roi_statement",
         {{"claimable", false},
          {"reason", "Incrementality cannot be claimed: no holdout outcomes are available."}}}};
    workspace_caller_->answers["list_runs"] = {
        {"runs",
         nlohmann::json::array(
             {{{"id", "run-1"},
               {"project_name", "churn"},
               {"graph", "S-1 CL-1"},
               {"status", "succeeded"},
               {"outputs",
                nlohmann::json::array({{{"name", "solution decisions"},
                                        {"ref", "solution_results.decisions_run_1"}}})},
               {"created", "2026-09-28T12:00:00Z"}}})}};
    workspace_caller_->answers["get_run"] = {
        {"id", "run-1"},
        {"project_name", "churn"},
        {"graph", "S-1 CL-1"},
        {"status", "succeeded"},
        {"created", "2026-09-28T12:00:00Z"},
        {"logs",
         nlohmann::json::array({{{"level", "info"},
                                 {"message", "solution stage succeeded: M-2 Entity Resolution"}}})},
        {"outputs",
         nlohmann::json::array(
             {{{"name", "solution decisions"}, {"ref", "solution_results.decisions_run_1"}}})}};
    workspace_caller_->answers["sql"] = {
        {"rows",
         nlohmann::json::array({{{"id", "decision-1"},
                                 {"entity_id", "account-17"},
                                 {"tier", "high"},
                                 {"action", "send_offer"},
                                 {"cell", "project-1/S-1 CL-1/retention_journey"},
                                 {"score", 0.89},
                                 {"autonomy", "A1"},
                                 {"status", "pending_approval"}}})}};
    const std::string project = connection->request(
        "GET", "/design/project?customer_id=" + customer_id + "&name=churn", "", cookie);
    EXPECT_NE(project.find("Solution Stages"), std::string::npos);
    EXPECT_NE(project.find("Decision Policy"), std::string::npos);
    EXPECT_NE(project.find("Experiment and Holdout"), std::string::npos);
    EXPECT_NE(project.find("Actions and A1 Queue"), std::string::npos);
    EXPECT_NE(project.find("Approve A1"), std::string::npos);
    EXPECT_NE(project.find("Latest Actions"), std::string::npos);
    EXPECT_NE(project.find("<strong>1,676 assignments.</strong> 165 holdout assignments."),
              std::string::npos);
    EXPECT_EQ(project.find("other-project-account"), std::string::npos);
    EXPECT_NE(project.find("Monitor"), std::string::npos);
    EXPECT_NE(project.find("ROI Readout"), std::string::npos);
    EXPECT_NE(project.find("Incrementality cannot be claimed: no holdout outcomes are available."),
              std::string::npos);
    EXPECT_NE(project.find("action=\"/w/" + connection->workspace_id + "/design/solution/run\""),
              std::string::npos);

    workspace_caller_->answers["run_solution"] = {
        {"run_id", "run-2"}, {"status", "queued"}, {"engine", "native"}, {"preview", false}};
    const std::string ran =
        connection->request("POST",
                            "/design/solution/run",
                            "customer_id=" + customer_id + "&project=churn&graph=S-1%20CL-1",
                            cookie);
    ASSERT_TRUE(ran.starts_with("HTTP/1.1 302"));
    EXPECT_EQ(std::get<0>(workspace_caller_->calls.back()), "run_solution");
    EXPECT_FALSE(std::get<1>(workspace_caller_->calls.back()));

    workspace_caller_->answers["update_policy"] = {{"policy_id", "policy-1"}, {"version", 2}};
    const std::string policy_config =
        lake::api::oauth_url_encode(R"({"autonomy":"A1","threshold":0.75})");
    const std::string updated = connection->request(
        "POST",
        "/design/solution/policy",
        "customer_id=" + customer_id + "&project=churn&policy_id=policy-1&config=" + policy_config +
            "&reason=reviewed&confirm=yes",
        cookie);
    ASSERT_TRUE(updated.starts_with("HTTP/1.1 302"));
    EXPECT_EQ(std::get<0>(workspace_caller_->calls.back()), "update_policy");
    EXPECT_FALSE(std::get<1>(workspace_caller_->calls.back()));

    workspace_caller_->answers["approve_decision"] = {{"decision_id", "decision-1"},
                                                      {"status", "approved"}};
    const std::string approved = connection->request(
        "POST",
        "/design/solution/approve",
        "customer_id=" + customer_id + "&project=churn&decision_id=decision-1&confirm=yes",
        cookie);
    ASSERT_TRUE(approved.starts_with("HTTP/1.1 302"));
    EXPECT_EQ(std::get<0>(workspace_caller_->calls.back()), "approve_decision");
    EXPECT_FALSE(std::get<1>(workspace_caller_->calls.back()));

    workspace_caller_->answers["record_outcome"] = {{"outcome_id", "outcome-1"}};
    const std::string outcome = connection->request(
        "POST",
        "/design/solution/outcome",
        "customer_id=" + customer_id +
            "&project=churn&action_id=action-1&outcome=retained&value=125&currency=USD"
            "&occurred_at=2026-09-28T12%3A30%3A00Z",
        cookie);
    ASSERT_TRUE(outcome.starts_with("HTTP/1.1 302"));
    EXPECT_EQ(std::get<0>(workspace_caller_->calls.back()), "record_outcome");
    EXPECT_FALSE(std::get<1>(workspace_caller_->calls.back()));

    workspace_caller_
        ->answers["get_graph"]["nodes"][0]["attributes"]["sources"]["missing_policy_predicates"] =
        nlohmann::json::array({"vulnerable"});
    const std::string blocked = connection->request(
        "GET", "/design/project?customer_id=" + customer_id + "&name=churn", "", cookie);
    EXPECT_NE(blocked.find("Run is blocked"), std::string::npos);
    EXPECT_EQ(blocked.find("action=\"/w/" + connection->workspace_id + "/design/solution/run\""), std::string::npos);
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
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["list_datasets"] = {
        {"datasets", nlohmann::json::array({{{"name", "accounts"}}})}};
    workspace_caller_->answers["create_project"] = {{"name", "x"}};

    const std::string page = connection->request("GET", "/design/opportunities", "", cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("action=\"/w/" + connection->workspace_id + "/design/opportunities/scan\""), std::string::npos);

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
    // The Opportunities are in the workspace's Zetta (lake#376), read as the person.
    const auto opportunities = [&] {
        const std::string listed = connection->request(
            "GET",
            "/v1/list_opportunities?customer_id=" + customer_id + "&workspace_id=" +
                connection->workspace_id,
            "", cookie);
        return nlohmann::json::parse(listed.substr(listed.find("\r\n\r\n") + 4))["value"]
            .value("opportunities", nlohmann::json::array());
    };
    const auto rows = opportunities();
    ASSERT_EQ(rows.size(), 2U);
    const auto chosen = std::ranges::find_if(
        rows, [](const nlohmann::json& row) { return row.at("status") == "new"; });
    ASSERT_NE(chosen, rows.end());
    const std::string chosen_id = chosen->at("id");

    const std::size_t before = workspace_caller_->calls.size();
    const std::string unconfirmed = connection->request(
        "POST", "/design/opportunities/create",
        "customer_id=" + customer_id + "&pick=" + chosen_id, cookie);
    EXPECT_NE(response_header(unconfirmed, "location").find("error="), std::string::npos);
    EXPECT_EQ(workspace_caller_->calls.size(), before) << "nothing is created without confirming";
    const std::string created = connection->request(
        "POST", "/design/opportunities/create",
        "customer_id=" + customer_id + "&pick=" + chosen_id + "&confirm=yes", cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 302")) << created.substr(0, 300);
    const auto project_call = std::ranges::find_if(
        workspace_caller_->calls.rbegin(), workspace_caller_->calls.rend(),
        [](const auto& call) { return std::get<0>(call) == "create_project"; });
    ASSERT_NE(project_call, workspace_caller_->calls.rend());
    const auto& [name, read, input] = *project_call;
    EXPECT_EQ(name, "create_project");
    EXPECT_TRUE(input["name"].get<std::string>().starts_with("predict-which-accounts-will-churn-"));
    EXPECT_NE(input["description"].get<std::string>().find("Source Opportunity"),
              std::string::npos);
    EXPECT_EQ(input["intent"], "Predict which accounts will churn");
    const auto after = opportunities();
    const auto made = std::ranges::find_if(
        after, [&](const nlohmann::json& row) { return row.at("id") == chosen_id; });
    ASSERT_NE(made, after.end());
    EXPECT_EQ(made->at("status"), "project");
    const std::string project = input["name"];
    EXPECT_EQ(made->at("project"), project);
    // Dismissing goes through the same workspace record, conditionally on its revision.
    const auto declined = std::ranges::find_if(
        after, [](const nlohmann::json& row) { return row.at("status") == "not_yet"; });
    ASSERT_NE(declined, after.end());
    const std::string dismissed = connection->request(
        "POST", "/design/opportunities/dismiss",
        "customer_id=" + customer_id + "&opportunity_id=" + declined->at("id").get<std::string>(),
        cookie);
    ASSERT_TRUE(dismissed.starts_with("HTTP/1.1 302")) << dismissed.substr(0, 300);
    EXPECT_EQ(response_header(dismissed, "location").find("error="), std::string::npos);
    const auto finally = opportunities();
    EXPECT_EQ(std::ranges::count_if(finally, [](const nlohmann::json& row) {
                  return row.at("status") == "dismissed";
              }),
              1);

    // The Project Agent designs the new project's pipeline in the background, and the project's
    // page compiles the design once it is done (Steve, 2026-09-27).
    const auto latest_build =
        store_->latest_agent_task(customer_id, "build:" + project, connection->workspace_id);
    ASSERT_TRUE(latest_build) << "creating the project started the Project Agent";
    const auto build = assembled_task(latest_build->id);
    ASSERT_TRUE(build);
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
    const auto second = store_->latest_agent_task(customer_id, "build:" + project, connection->workspace_id);
    const std::string again = lake::control::new_uuid_v4();
    auto rebuild = store_->create_agent_task(lake::control::AgentTask{
        .customer_id = customer_id, .agent = "agent-project", .kind = "build:" + project,
        .workspace_id = second->workspace_id,
        .body = {{"message", "Design it"}}, .scopes = second->scopes,
        .requested_by_user_id = second->requested_by_user_id,
        .requested_by_taap_id = second->requested_by_taap_id});
    ASSERT_TRUE(rebuild);
    lake::test::seed_agent_task_request(workspaces_, *rebuild, {{"message", "Design it"}},
                                        instance_kek_);
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
        .workspace_id = second->workspace_id,
        .body = {{"message", "Design it"}}, .scopes = second->scopes,
        .requested_by_user_id = second->requested_by_user_id,
        .requested_by_taap_id = second->requested_by_taap_id});
    ASSERT_TRUE(retrieval);
    lake::test::seed_agent_task_request(workspaces_, *retrieval, {{"message", "Design it"}},
                                        instance_kek_);
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

// lake#440: Ship > Packages dropped the connection when the space had a build with no digest yet.
TEST_P(HttpResponseIsolation, ShipPackagesListsABuildThatHasNoDigestYet) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const nlohmann::json customers = nlohmann::json::parse(response_body(customers_response));
    const std::string customer_id = customers["value"]["items"][0]["id"];
    select_workspace(*connection, customer_id);
    const auto repository = store_->create_repository({customer_id, "scoring", "", "private",
                                                       lake::test::repository_workspace(
                                                           *store_, customer_id)});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto queued = store_->create_build({.customer_id = customer_id,
                                              .workspace_id = connection->workspace_id,
                                              .repository = repository->id,
                                              .revision = "main",
                                              .target = "lake.example.test/acme/scoring:v3"});
    ASSERT_TRUE(queued) << queued.error().message;
    ASSERT_TRUE(queued->digest.empty());

    const std::string page =
        connection->request("GET", "/ship/packages?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("data-build-id=\"" + queued->id + "\""), std::string::npos);
    EXPECT_NE(page.find("<code>lake.example.test/acme/scoring:v3</code>"), std::string::npos);
}

TEST_P(HttpResponseIsolation, ShipAppsCreatesOperatesAndFollowsAnApp) {
    auto connection = connect();
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const std::string customers_response =
        connection->request("GET", "/v1/list_customers", "", cookie);
    const nlohmann::json customers =
        nlohmann::json::parse(customers_response.substr(customers_response.find("\r\n\r\n") + 4));
    const std::string customer_id = customers["value"]["items"][0]["id"];
    select_workspace(*connection, customer_id);

    const std::string empty =
        connection->request("GET", "/ship/apps?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(empty.starts_with("HTTP/1.1 200")) << empty.substr(0, 300);
    EXPECT_NE(empty.find("No apps yet"), std::string::npos);
    EXPECT_EQ(empty.find("ApplicationAgent is not registered"), std::string::npos);
    // The viewer's own space is named as theirs, not by its generated handle (lake#452).
    EXPECT_NE(empty.find(">Personal space</option>"), std::string::npos);
    EXPECT_EQ(empty.find(">" + customers["value"]["items"][0]["name"].get<std::string>() +
                         "</option>"),
              std::string::npos);
    EXPECT_NE(empty.find("name=\"description\""), std::string::npos);
    EXPECT_EQ(empty.find("name=\"description\" placeholder=\"What should the app do?\" disabled"),
              std::string::npos);

    const std::string agent_started = connection->request(
        "POST",
        "/ship/apps/agent",
        "customer_id=" + customer_id + "&description=Build+a+customer+health+app",
        cookie);
    ASSERT_TRUE(agent_started.starts_with("HTTP/1.1 302")) << agent_started.substr(0, 300);
    const std::string agent_task = task_of(agent_started);
    ASSERT_FALSE(agent_task.empty()) << response_header(agent_started, "location");
    const auto first_task = assembled_task(agent_task);
    ASSERT_TRUE(first_task);
    EXPECT_EQ(first_task->agent, "agent-application");
    EXPECT_EQ(first_task->kind, "app");
    EXPECT_EQ(first_task->body["context"], nlohmann::json::object());
    EXPECT_EQ(std::ranges::find(first_task->scopes, "create_app"), first_task->scopes.end());
    // A confirmed retry looks up the repository its earlier attempt created.
    EXPECT_NE(std::ranges::find(first_task->scopes, "list_repositories"),
              first_task->scopes.end());
    EXPECT_NE(std::ranges::find(first_task->scopes, "get_repository"), first_task->scopes.end());
    const std::string agent_location = response_header(agent_started, "location");
    ASSERT_FALSE(agent_location.empty()) << agent_started;
    const std::string working = connection->request("GET", agent_location, "", cookie);
    EXPECT_NE(working.find("ApplicationAgent Is Working"), std::string::npos) << working;

    agents_->answer = {{"ok", true},
                       {"reply", "Which dataset should the app use?"},
                       {"result", {{"kind", "question"}, {"confirm", false}}}};
    connection.reset();
    connection = connect();
    run_agent_task(*connection, agent_task);
    connection.reset();
    connection = connect();
    const std::string question = connection->request("GET", agent_location, "", cookie);
    EXPECT_NE(question.find("Which dataset should the app use?"), std::string::npos);
    EXPECT_NE(question.find("<h3>Reply</h3>"), std::string::npos);
    EXPECT_NE(question.find("name=\"reply\""), std::string::npos);
    const std::string replied = connection->request(
        "POST",
        "/ship/apps/agent",
        "customer_id=" + customer_id + "&task_id=" + agent_task +
            "&reply=Use+datasets%2Faccounts",
        cookie);
    ASSERT_TRUE(replied.starts_with("HTTP/1.1 302")) << replied.substr(0, 300);
    const std::string proposal_task = task_of(replied);
    const auto continued_task = assembled_task(proposal_task);
    ASSERT_TRUE(continued_task);
    EXPECT_EQ(continued_task->conversation_id, first_task->conversation_id);
    EXPECT_EQ(continued_task->body["message"], "Use datasets/accounts");
    EXPECT_EQ(continued_task->body["prior_turn"]["task_id"], agent_task);

    const nlohmann::json proposal = {
        {"kind", "proposal"},
        {"confirm", true},
        {"files",
         nlohmann::json::array(
             {{{"path", "app.py"}, {"contents", "def health():\n    return 'healthy'\n"}}})},
        {"app",
         {{"name", "customer-health"},
          {"mode", "runtime"},
          {"repository", {{"name", "app-customer-health"}}},
          {"path", "."},
          {"framework", "fastapi"},
          {"port", 8080},
          {"auth", "none"}}}};
    agents_->answer = {{"ok", true},
                       {"reply", "Review the customer health app."},
                       {"result", proposal}};
    connection.reset();
    connection = connect();
    run_agent_task(*connection, proposal_task);
    connection.reset();
    connection = connect();
    const std::string review = connection->request(
        "GET", response_header(replied, "location"), "", cookie);
    EXPECT_NE(review.find("Proposed App: customer-health"), std::string::npos);
    EXPECT_NE(review.find("app.py"), std::string::npos);
    // A public app says so and asks for its own acknowledgement (lake#452).
    EXPECT_NE(review.find("<dd>Public — no sign-in</dd>"), std::string::npos);
    EXPECT_NE(review.find("This app will be public"), std::string::npos);
    EXPECT_NE(review.find("name=\"confirm_public\" value=\"1\" required"), std::string::npos);
    const std::string sha_marker = "name=\"proposal_sha256\" value=\"";
    const auto sha_at = review.find(sha_marker);
    ASSERT_NE(sha_at, std::string::npos);
    const std::string proposal_sha = review.substr(sha_at + sha_marker.size(), 64);
    const std::string unacknowledged = connection->request(
        "POST",
        "/ship/apps/agent/confirm",
        "customer_id=" + customer_id + "&task_id=" + proposal_task +
            "&proposal_sha256=" + proposal_sha,
        cookie);
    ASSERT_TRUE(unacknowledged.starts_with("HTTP/1.1 302")) << unacknowledged.substr(0, 300);
    EXPECT_NE(response_header(unacknowledged, "location").find("will%20be%20public"),
              std::string::npos)
        << response_header(unacknowledged, "location");
    EXPECT_TRUE(task_of(unacknowledged).empty()) << "no turn starts without the acknowledgement";

    const std::string stale = connection->request(
        "POST",
        "/ship/apps/agent/confirm",
        "customer_id=" + customer_id + "&task_id=" + proposal_task +
            "&proposal_sha256=" + std::string(64, '0'),
        cookie);
    EXPECT_NE(response_header(stale, "location").find("error="), std::string::npos);

    connection.reset();
    connection = connect();
    const std::string confirmed = connection->request(
        "POST",
        "/ship/apps/agent/confirm",
        "customer_id=" + customer_id + "&task_id=" + proposal_task +
            "&proposal_sha256=" + proposal_sha + "&confirm_public=1",
        cookie);
    ASSERT_TRUE(confirmed.starts_with("HTTP/1.1 302")) << confirmed.substr(0, 300);
    const std::string confirmed_task_id = task_of(confirmed);
    const auto confirmed_task = assembled_task(confirmed_task_id);
    ASSERT_TRUE(confirmed_task);
    EXPECT_EQ(confirmed_task->body["context"]["confirmed_proposal"], proposal);
    EXPECT_EQ(confirmed_task->body["context"]["confirm_public"], true);
    EXPECT_NE(std::ranges::find(confirmed_task->scopes, "create_app"),
              confirmed_task->scopes.end());
    EXPECT_NE(std::ranges::find(confirmed_task->scopes, "deploy_app"),
              confirmed_task->scopes.end());
    agents_->answer = {
        {"ok", true},
        {"reply", "The app is deployed."},
        {"result",
         {{"kind", "deployed"},
          {"app", {{"url", "https://customer-health.apps.example.test"}}}}}};
    connection.reset();
    connection = connect();
    run_agent_task(*connection, confirmed_task_id);
    connection.reset();
    connection = connect();
    const std::string deployed =
        connection->request("GET", response_header(confirmed, "location"), "", cookie);
    EXPECT_NE(deployed.find("ApplicationAgent Result"), std::string::npos);
    EXPECT_NE(deployed.find("https://customer-health.apps.example.test"), std::string::npos);

    const std::string repository = connection->request(
        "POST",
        "/repository/create",
        "customer_id=" + customer_id + "&workspace_id=" + connection->workspace_id +
            "&name=apps-ui-source&description=App+source",
        cookie);
    const std::string repository_location = response_header(repository, "location");
    ASSERT_TRUE(repository_location.starts_with("/repository?id=")) << repository;
    const std::string repository_id =
        repository_location.substr(repository_location.find("id=") + 3);
    const std::string created =
        connection->request("POST",
                            "/ship/apps/create",
                            "customer_id=" + customer_id +
                                "&mode=runtime&name=customer-portal&repository=" + repository_id +
                                "&revision=main&path=.&framework=node&port=3000",
                            cookie);
    ASSERT_TRUE(created.starts_with("HTTP/1.1 302")) << created.substr(0, 300);
    const std::string location = response_header(created, "location");
    const std::size_t app_at = location.find("app_id=");
    ASSERT_NE(app_at, std::string::npos) << location;
    const std::string app_id = location.substr(app_at + 7);

    // Builds take their registry authority from LAKE_INTERNAL_URL via the server options; a context
    // that never received it answered every build_app with store_unavailable (live 2026-09-30).
    const std::string built =
        connection->request("POST",
                            "/v1/build_app",
                            nlohmann::json{{"customer_id", customer_id},
                                           {"workspace_id", connection->workspace_id},
                                           {"app", app_id}}
                                .dump(),
                            cookie,
                            "HTTP/1.1",
                            "",
                            "application/json");
    EXPECT_NE(built.find("\"ok\":true"), std::string::npos) << built.substr(0, 400);
    EXPECT_NE(built.find("\"target\":\"lake-prod.lake.svc/"), std::string::npos) << built.substr(0, 400);
    const nlohmann::json built_body = nlohmann::json::parse(response_body(built));
    const auto queued_build = store_->get_build(built_body.at("value").at("build_id"));
    ASSERT_TRUE(queued_build) << queued_build.error().message;
    EXPECT_TRUE(queued_build->dockerfile.starts_with(
        "inline:ARG APP_RUNTIME_IMAGE\nFROM ${APP_RUNTIME_IMAGE}\n"));
    EXPECT_EQ(queued_build->build_args.at("APP_RUNTIME_IMAGE"), "oramasearch/app-runtime:20260930");

    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory != nullptr) {
        ASSERT_TRUE(memory->set_web_app_status(
            app_id, "Running", "https://customer-portal.apps.example.test", true, "Ready", 1));
    }
    const std::string published =
        connection->request("POST",
                            "/v1/publish_app_event",
                            nlohmann::json{{"customer_id", customer_id},
                                           {"workspace_id", connection->workspace_id},
                                           {"app_id", app_id},
                                           {"type", "deploy"},
                                           {"message", "Revision is ready"},
                                           {"event", {{"phase", "Running"}}}}
                                .dump(),
                            cookie,
                            "HTTP/1.1",
                            "",
                            "application/json");
    EXPECT_NE(published.find("\"ok\":true"), std::string::npos) << published;

    const std::string page = connection->request("GET", location, "", cookie);
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_NE(page.find("<h1>customer-portal</h1>"), std::string::npos);
    EXPECT_NE(page.find("Revision is ready"), std::string::npos);
    EXPECT_NE(page.find("id=\"app-live-events\""), std::string::npos);
    EXPECT_NE(page.find("customer-portal:build-"), std::string::npos);
    EXPECT_NE(page.find("action=\"/w/" + connection->workspace_id + "/ship/app/build\""), std::string::npos);
    EXPECT_NE(page.find("action=\"/w/" + connection->workspace_id + "/ship/app/restart\""), std::string::npos);
    EXPECT_NE(page.find("action=\"/w/" + connection->workspace_id + "/ship/app/service/create\""), std::string::npos);
    if (memory != nullptr) {
        EXPECT_NE(page.find("https://customer-portal.apps.example.test"), std::string::npos);
    }

    const auto before = store_->get_web_app(app_id);
    ASSERT_TRUE(before);
    const std::string restarted = connection->request(
        "POST", "/ship/app/restart", "customer_id=" + customer_id + "&app_id=" + app_id, cookie);
    EXPECT_EQ(response_header(restarted, "location"), location);
    const auto after = store_->get_web_app(app_id);
    ASSERT_TRUE(after);
    EXPECT_GT(after->generation, before->generation);
}

TEST_P(HttpResponseIsolation, ExperimentsAgentRestMcpAndPageUseTheSameConfirmedTurn) {
    auto connection = connect();
    const auto body_of = [](const std::string& response) {
        return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4));
    };
    const std::string sign_in = connection->request(
        "POST", "/identity/sign-in", "email=" + email_ + "&password=" + password_);
    const std::string issued = response_header(sign_in, "set-cookie");
    const std::string cookie = issued.substr(0, issued.find(';'));
    const nlohmann::json customers =
        body_of(connection->request("GET", "/v1/list_customers", "", cookie));
    const std::string customer_id = customers["value"]["items"][0]["id"];
    select_workspace(*connection, customer_id);

    const nlohmann::json openapi =
        body_of(connection->request("GET", "/v1/openapi.json", "", cookie));
    const nlohmann::json listed = body_of(connection->request(
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
        cookie,
        "HTTP/1.1",
        "",
        "application/json"));
    const auto tool = std::ranges::find_if(
        listed["result"]["tools"],
        [](const nlohmann::json& candidate) {
            return candidate["name"] == "agent_turn";
        });
    ASSERT_NE(tool, listed["result"]["tools"].end());
    const nlohmann::json operation = openapi["paths"]["/v1/agent_turn"]["post"];
    EXPECT_EQ((*tool)["inputSchema"],
              operation["requestBody"]["content"]["application/json"]["schema"]);
    EXPECT_EQ((*tool)["outputSchema"],
              operation["responses"]["200"]["content"]["application/json"]["schema"]);

    const nlohmann::json turn = {{"customer_id", customer_id},
                                 {"workspace_id", connection->workspace_id},
                                 {"agent", "agent-experiments"},
                                 {"kind", "experiment"},
                                 {"message", "Tune retention"},
                                 {"conversation_id", "d92a57b7-04bb-4c35-8277-4833e791aa79"}};
    const nlohmann::json rest = body_of(connection->request(
        "POST", "/v1/agent_turn", turn.dump(), cookie, "HTTP/1.1", "", "application/json"));
    ASSERT_TRUE(rest["ok"].get<bool>()) << rest.dump();
    EXPECT_EQ(assembled_task(rest["value"]["task_id"])->agent, "agent-experiments");
    const nlohmann::json mcp = body_of(connection->request(
        "POST",
        "/mcp",
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 2},
                       {"method", "tools/call"},
                       {"params", {{"name", "agent_turn"}, {"arguments", turn}}}}
            .dump(),
        cookie,
        "HTTP/1.1",
        "",
        "application/json"));
    ASSERT_TRUE(mcp["result"]["structuredContent"]["ok"].get<bool>()) << mcp.dump();
    const auto rest_task = assembled_task(rest["value"]["task_id"]);
    const auto mcp_task = assembled_task(
        mcp["result"]["structuredContent"]["value"]["task_id"]);
    ASSERT_TRUE(rest_task);
    ASSERT_TRUE(mcp_task);
    EXPECT_EQ(rest_task->conversation_id, turn["conversation_id"].get<std::string>());
    EXPECT_EQ(mcp_task->conversation_id, turn["conversation_id"].get<std::string>());
    EXPECT_EQ(mcp_task->body["prior_turn"]["task_id"], rest_task->id);

    const std::string empty = connection->request(
        "GET", "/train/experiments?customer_id=" + customer_id, "", cookie);
    ASSERT_TRUE(empty.starts_with("HTTP/1.1 200")) << empty.substr(0, 300);
    EXPECT_NE(empty.find("<h2>Run an Experiment</h2>"), std::string::npos);
    EXPECT_NE(empty.find("data-capability=\"agent_turn\""), std::string::npos);
    EXPECT_EQ(empty.find("name=\"goal\" rows=\"4\" placeholder=\"Tune a churn model for "
                         "validation AUC with a bounded trial budget.\" required disabled"),
              std::string::npos);

    const std::string started = connection->request(
        "POST",
        "/train/experiments/agent",
        "customer_id=" + customer_id + "&goal=Tune+retention+for+validation+AUC",
        cookie);
    ASSERT_TRUE(started.starts_with("HTTP/1.1 302")) << started.substr(0, 300);
    const std::string task_id = task_of(started);
    const auto first = assembled_task(task_id);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->agent, "agent-experiments");
    EXPECT_EQ(first->kind, "experiment");
    EXPECT_EQ(std::ranges::find(first->scopes, "create_study"), first->scopes.end());
    const std::string working = connection->request(
        "GET", response_header(started, "location"), "", cookie);
    EXPECT_NE(working.find("ExperimentsAgent Is Working"), std::string::npos);

    agents_->answer = {{"ok", true},
                       {"reply", "Which project should I use for the dataset?"},
                       {"result", {{"kind", "question"}, {"confirm", false}}}};
    run_agent_task(*connection, task_id);
    const std::string question = connection->request(
        "GET", response_header(started, "location"), "", cookie);
    EXPECT_NE(question.find("Which project should I use for the dataset?"), std::string::npos);
    EXPECT_NE(question.find("<h3>Reply</h3>"), std::string::npos);
    EXPECT_NE(question.find("name=\"reply\""), std::string::npos);
    const std::string replied = connection->request(
        "POST",
        "/train/experiments/agent",
        "customer_id=" + customer_id + "&task_id=" + task_id + "&reply=Use+retention",
        cookie);
    ASSERT_TRUE(replied.starts_with("HTTP/1.1 302")) << replied.substr(0, 300);
    const std::string proposal_task_id = task_of(replied);
    const auto continued = assembled_task(proposal_task_id);
    ASSERT_TRUE(continued);
    EXPECT_EQ(continued->conversation_id, first->conversation_id);
    EXPECT_EQ(continued->body["message"], "Use retention");
    EXPECT_EQ(continued->body["prior_turn"]["task_id"], task_id);

    nlohmann::json study = {{"name", "Retention Tuning"},
                            {"project", "retention"},
                            {"pipeline", "training"},
                            {"dataset", "accounts"},
                            {"methodology", "M-1"},
                            {"objective", {{"metric", "validation.auc"},
                                           {"direction", "maximize"},
                                           {"validation_split", "validation"}}},
                            {"budgets", {{"max_trials", 4},
                                         {"max_parallel_trials", 1},
                                         {"max_wall_time_ms", 60000},
                                         {"cost_cap", 5}}}};
    agents_->answer = {{"ok", true},
                       {"reply", "Review the bounded retention study."},
                       {"result", {{"kind", "proposal"},
                                   {"confirm", true},
                                   {"study", study},
                                   {"explanations", {{"objective", "Use validation AUC."}}}}}};
    run_agent_task(*connection, proposal_task_id);
    const std::string review = connection->request(
        "GET", response_header(replied, "location"), "", cookie);
    EXPECT_NE(review.find("Proposed Study"), std::string::npos);
    EXPECT_NE(review.find("Study proposal (JSON)"), std::string::npos);
    const std::string sha_marker = "name=\"proposal_sha256\" value=\"";
    const auto sha_at = review.find(sha_marker);
    ASSERT_NE(sha_at, std::string::npos);
    const std::string proposal_sha = review.substr(sha_at + sha_marker.size(), 64);

    study["name"] = "Reviewed Retention Tuning";
    const std::string confirmed = connection->request(
        "POST",
        "/train/experiments/agent/confirm",
        "customer_id=" + customer_id + "&task_id=" + proposal_task_id +
            "&proposal_sha256=" + proposal_sha +
            "&study=" + lake::api::oauth_url_encode(study.dump()),
        cookie);
    ASSERT_TRUE(confirmed.starts_with("HTTP/1.1 302")) << confirmed.substr(0, 300);
    const auto confirmed_task = assembled_task(task_of(confirmed));
    ASSERT_TRUE(confirmed_task);
    EXPECT_EQ(confirmed_task->body["context"]["confirmed_proposal"]["study"]["name"],
              "Reviewed Retention Tuning");
    EXPECT_NE(std::ranges::find(confirmed_task->scopes, "create_study"),
              confirmed_task->scopes.end());
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
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
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

    // Engine calls other than reading the conversation's own records (lake#370).
    const auto engine_calls = [&] {
        return std::ranges::count_if(workspace_caller_->calls, [](const auto& call) {
            return !lake::test::RecordTables::handles(std::get<0>(call), std::get<2>(call));
        });
    };
    const auto before = engine_calls();
    const std::string stale = connection->request(
        "POST",
        "/train/agents/new/approve",
        "customer_id=" + customer_id + "&conversation=" + conversation +
            "&message_id=" + message_id + "&proposal_sha256=" + std::string(64, '0'),
        cookie);
    EXPECT_NE(response_header(stale, "location").find("error="), std::string::npos);
    EXPECT_EQ(engine_calls(), before) << "nothing is created for another proposal";

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

TEST_P(HttpResponseIsolation, ProtectedAppAuthorizationHandsOffAHostOnlySession) {
    const auto issued =
        store_->sign_in_password({"lake", email_, password_, "", "app-auth-fixture"});
    ASSERT_TRUE(issued) << issued.error().message;
    const auto principal = store_->context_for(issued->secret);
    ASSERT_TRUE(principal);

    const std::string suffix = lake::control::new_uuid_v4();
    std::string customer_name = "app-auth-" + suffix;
    std::erase(customer_name, '-');
    const auto organization =
        store_->create_organization({"taap:1:ed25519:app-auth-" + suffix, "App auth fixture", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store_->create_customer({"organization", "", organization->id, customer_name});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto membership = store_->create_membership({organization->id,
                                                       "",
                                                       "user",
                                                       principal->user_id,
                                                       principal->taap_id,
                                                       "member",
                                                       "app-auth-attestation",
                                                       "app-auth-grantor",
                                                       "app-auth-request"});
    ASSERT_TRUE(membership) << membership.error().message;
    const auto workspace =
        store_->create_workspace({customer->id, "protected", "prod", principal->taap_id});
    ASSERT_TRUE(workspace) << workspace.error().message;

    lake::control::NewWebApp app_input;
    app_input.customer_id = customer->id;
    app_input.workspace_id = workspace->id;
    app_input.name = "protected";
    app_input.mode = "image";
    app_input.image = "registry.example.test/protected:latest";
    app_input.port = 8080;
    app_input.host = "protected-" + customer_name + ".apps.example.test";
    app_input.auth = "lake";
    const auto app = store_->create_web_app(app_input);
    ASSERT_TRUE(app) << app.error().message;
    app_input.name = "other";
    app_input.host = "other-" + customer_name + ".apps.example.test";
    const auto other_app = store_->create_web_app(app_input);
    ASSERT_TRUE(other_app) << other_app.error().message;

    const std::string auth_path = "/w/" + workspace->id + "/v1/apps/auth";
    // Exactly what ingress-nginx's auth subrequest sends: Host is Lake's Service, there is no
    // X-Forwarded-Host, and X-Original-URL is absolute ($scheme://$http_host$request_uri).
    const auto app_headers = [&](const std::string& host, const std::string& original_url) {
        const std::string absolute =
            original_url.starts_with("http") ? original_url : "http://" + host + original_url;
        return std::vector<std::pair<std::string, std::string>>{
            {"X-Original-Method", "GET"},
            {"X-Original-URL", absolute},
        };
    };
    HttpConnection connection(server_->port());
    const std::string missing = connection.request(
        "GET", auth_path, {}, {}, "HTTP/1.1", {}, {}, app_headers(app->host, "/"));
    ASSERT_TRUE(missing.starts_with("HTTP/1.1 401")) << missing;

    const std::string redirect_url = "https://" + app->host + "/dashboard?view=recent";
    const std::string authorize = "/apps/authorize?app=" + lake::api::oauth_url_encode(app->id) +
                                  "&rd=" + lake::api::oauth_url_encode(redirect_url);
    const std::string signed_out = connection.request("GET", authorize);
    ASSERT_TRUE(signed_out.starts_with("HTTP/1.1 302")) << signed_out;
    EXPECT_TRUE(response_header(signed_out, "location").starts_with("/sign-in?return_to="));
    const std::string return_cookie = response_cookie_pair(signed_out);
    EXPECT_TRUE(return_cookie.starts_with("lake_sign_in_return="));

    const std::string signed_in =
        connection.request("POST",
                           "/identity/sign-in",
                           "email=" + lake::api::oauth_url_encode(email_) + "&password=" +
                               lake::api::oauth_url_encode(password_) + "&device_name=app-auth",
                           return_cookie);
    ASSERT_TRUE(signed_in.starts_with("HTTP/1.1 302")) << signed_in;
    EXPECT_EQ(response_header(signed_in, "location"), authorize);
    const std::string lake_cookie = response_cookie_pair(signed_in);
    ASSERT_TRUE(lake_cookie.starts_with("lake_session="));

    const std::string wrong_redirect =
        connection.request("GET",
                           "/apps/authorize?app=" + lake::api::oauth_url_encode(app->id) +
                               "&rd=https%3A%2F%2Fevil.example.test%2F",
                           {},
                           lake_cookie);
    ASSERT_TRUE(wrong_redirect.starts_with("HTTP/1.1 400")) << wrong_redirect;

    const std::string authorized =
        connection.request("GET", authorize, {}, return_cookie + "; " + lake_cookie);
    ASSERT_TRUE(authorized.starts_with("HTTP/1.1 302")) << authorized;
    const std::string handoff = response_header(authorized, "location");
    const auto handoff_url = boost::urls::parse_uri(handoff);
    ASSERT_TRUE(handoff_url) << handoff;
    std::string code;
    for (const auto& parameter : handoff_url->params()) {
        if (parameter.key == "__lake_code") {
            code = std::string(parameter.value);
        }
    }
    ASSERT_FALSE(code.empty());
    EXPECT_EQ(handoff_url->host(), app->host);

    const std::string exchanged =
        connection.request("GET",
                           auth_path,
                           {},
                           {},
                           "HTTP/1.1",
                           {},
                           {},
                           app_headers(app->host, "/dashboard?view=recent&__lake_code=" + code));
    ASSERT_TRUE(exchanged.starts_with("HTTP/1.1 200")) << exchanged;
    EXPECT_EQ(response_header(exchanged, "x-orama-user"), principal->user_id);
    EXPECT_EQ(response_header(exchanged, "x-orama-space"), workspace->id);
    const std::string app_cookie_header = response_header(exchanged, "set-cookie");
    EXPECT_TRUE(app_cookie_header.starts_with("lake_app_session="));
    EXPECT_NE(app_cookie_header.find("; Path=/; Secure; HttpOnly; SameSite=Lax"),
              std::string::npos);
    EXPECT_EQ(app_cookie_header.find("Domain="), std::string::npos);
    const std::string app_cookie = response_cookie_pair(exchanged);

    const std::string reused =
        connection.request("GET",
                           auth_path,
                           {},
                           {},
                           "HTTP/1.1",
                           {},
                           {},
                           app_headers(app->host, "/dashboard?__lake_code=" + code));
    EXPECT_TRUE(reused.starts_with("HTTP/1.1 401")) << reused;

    const std::string wrong_app = connection.request(
        "GET", auth_path, {}, app_cookie, "HTTP/1.1", {}, {}, app_headers(other_app->host, "/"));
    EXPECT_TRUE(wrong_app.starts_with("HTTP/1.1 401")) << wrong_app;

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto expired_token = lake::control::issue_web_app_session(
        instance_kek_, app->id, principal->user_id, workspace->id, now - 12 * 60 * 60);
    ASSERT_TRUE(expired_token) << expired_token.error();
    const std::string expired = connection.request("GET",
                                                   auth_path,
                                                   {},
                                                   "lake_app_session=" + *expired_token,
                                                   "HTTP/1.1",
                                                   {},
                                                   {},
                                                   app_headers(app->host, "/"));
    EXPECT_TRUE(expired.starts_with("HTTP/1.1 401")) << expired;

    ASSERT_TRUE(store_->remove_member(membership->id, "app-auth-grantor"));
    const std::string removed = connection.request(
        "GET", auth_path, {}, app_cookie, "HTTP/1.1", {}, {}, app_headers(app->host, "/"));
    EXPECT_TRUE(removed.starts_with("HTTP/1.1 401")) << removed;
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

// lake#440: a handler that throws is answered 500 and logged with its route and request id; the
// connection stays open for the next request.
class ThrowingRoutes {
public:
    ThrowingRoutes() {
        namespace http = boost::http;
        http::router<http::route_params> router;
        const auto observe = [this](http::route_params&, const lake::api::RouteFailure& failure) {
            std::lock_guard lock(mutex_);
            failures_.push_back(failure);
        };
        // Throws while the handler is called, before any task exists.
        router.add(http::method::get,
                   "/throws-now",
                   lake::api::guard_route(
                       "/throws-now",
                       [](http::route_params&) -> http::route_task {
                           throw std::runtime_error("renderer failed before its task");
                       },
                       observe));
        // Throws inside the handler's task, as a renderer exception does.
        router.add(http::method::get,
                   "/throws-later",
                   lake::api::guard_route(
                       "/throws-later",
                       [](http::route_params& route) -> http::route_task {
                           route.res.set("X-Partial", "set before the throw");
                           const nlohmann::json row{{"digest", nullptr}};
                           // Throws: the key is present and null, not a string.
                           route.res.set("X-Digest", row.value("digest", std::string()));
                           co_return http::route_done;
                       },
                       observe));
        router.add(http::method::get,
                   "/fine",
                   lake::api::guard_route(
                       "/fine",
                       [](http::route_params& route) -> http::route_task {
                           route.status(http::status::ok);
                           route.res.set(http::field::content_type, "text/plain");
                           route.res.set_payload_size(2);
                           auto [error] = co_await route.send("ok");
                           if (error) {
                               co_return http::route_error(error);
                           }
                           co_return http::route_done;
                       },
                       observe));
        server_ = std::make_unique<boost::beast2::http_server>(
            io_context_,
            1,
            std::move(router),
            http::make_parser_config(http::parser_config{true}),
            http::make_serializer_config(http::serializer_config()));
        if (server_->bind(boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), 0))) {
            return;
        }
        port_ = server_->local_endpoint().port();
        server_->start();
        thread_ = std::thread([this] { io_context_.run(); });
    }

    ~ThrowingRoutes() {
        if (server_) {
            server_->stop();
        }
        io_context_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] unsigned short port() const {
        return port_;
    }

    [[nodiscard]] std::vector<lake::api::RouteFailure> failures() const {
        std::lock_guard lock(mutex_);
        return failures_;
    }

private:
    boost::corosio::io_context io_context_;
    std::unique_ptr<boost::beast2::http_server> server_;
    std::thread thread_;
    unsigned short port_ = 0;
    mutable std::mutex mutex_;
    std::vector<lake::api::RouteFailure> failures_;
};

TEST(HttpServer, AHandlerThatThrowsIsAnswered500AndLogged) {
    ThrowingRoutes routes;
    ASSERT_NE(routes.port(), 0);
    HttpConnection connection(routes.port());

    const std::string page = connection.request(
        "GET", "/throws-now", {}, {}, "HTTP/1.1", {}, "text/plain",
        {{"Accept", "text/html,application/xhtml+xml"}});
    ASSERT_TRUE(page.starts_with("HTTP/1.1 500")) << page.substr(0, 300);
    EXPECT_EQ(response_header(page, "content-type"), "text/html; charset=utf-8");
    const std::string generated_id = response_header(page, "x-request-id");
    EXPECT_FALSE(generated_id.empty());
    EXPECT_NE(page.find("We Could Not Load This Page"), std::string::npos);
    EXPECT_NE(page.find("Request id: " + generated_id), std::string::npos);
    EXPECT_EQ(page.find("renderer failed"), std::string::npos);

    // Same connection: the failure did not drop it.
    const std::string api = connection.request(
        "GET", "/throws-later", {}, {}, "HTTP/1.1", {}, "text/plain",
        {{"Accept", "application/json"}, {"X-Request-Id", "request-440"}});
    ASSERT_TRUE(api.starts_with("HTTP/1.1 500")) << api.substr(0, 300);
    EXPECT_EQ(response_header(api, "content-type"), "application/json; charset=utf-8");
    EXPECT_EQ(response_header(api, "x-request-id"), "request-440");
    EXPECT_TRUE(response_header(api, "x-partial").empty());
    const nlohmann::json envelope = nlohmann::json::parse(response_body(api));
    EXPECT_EQ(envelope["ok"], false);
    EXPECT_EQ(envelope["error"]["code"], "internal_error");
    EXPECT_NE(envelope["error"]["message"].get<std::string>().find("request-440"),
              std::string::npos);

    const std::string fine = connection.request("GET", "/fine");
    ASSERT_TRUE(fine.starts_with("HTTP/1.1 200")) << fine.substr(0, 300);
    EXPECT_EQ(response_body(fine), "ok");

    const auto failures = routes.failures();
    ASSERT_EQ(failures.size(), 2U);
    EXPECT_EQ(failures[0].route, "/throws-now");
    EXPECT_EQ(failures[0].method, "GET");
    EXPECT_EQ(failures[0].request_id, generated_id);
    EXPECT_EQ(failures[0].message, "renderer failed before its task");
    EXPECT_EQ(failures[1].route, "/throws-later");
    EXPECT_EQ(failures[1].path, "/throws-later");
    EXPECT_EQ(failures[1].request_id, "request-440");
    EXPECT_NE(failures[1].message.find("type must be string"), std::string::npos);

    const nlohmann::json line = nlohmann::json::parse(lake::api::route_failure_log_line(failures[1]));
    EXPECT_EQ(line["level"], "error");
    EXPECT_EQ(line["event"], "handler_exception");
    EXPECT_EQ(line["route"], "/throws-later");
    EXPECT_EQ(line["request_id"], "request-440");
    EXPECT_EQ(line["error"], failures[1].message);
}

// lake#441: a client learns that lake's tools changed instead of keeping a stale list.
TEST(HttpServer, McpSessionsLearnThatTheToolListChanged) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.instance_kek = "mcp-list-changed-test-kek";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    const auto body_of = [](const std::string& response) {
        return response.substr(response.find("\r\n\r\n") + 4);
    };
    const std::vector<std::pair<std::string, std::string>> accepts_events = {
        {"Accept", "application/json, text/event-stream"}};

    const std::string initialized = connection.request(
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})",
        {}, "HTTP/1.1", {}, "application/json", accepts_events);
    ASSERT_TRUE(initialized.starts_with("HTTP/1.1 200")) << initialized.substr(0, 300);
    EXPECT_EQ(nlohmann::json::parse(body_of(initialized))["result"]["capabilities"]["tools"]
                                                         ["listChanged"],
              true);
    const std::string session = response_header(initialized, "mcp-session-id");
    ASSERT_FALSE(session.empty());
    // The session names this build's tool list: 64 hex digest characters, a dot, 32 random ones.
    ASSERT_EQ(session.size(), 64U + 1U + 32U) << session;
    EXPECT_EQ(session[64], '.');

    const std::string call =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"server_info","arguments":{}}})";
    auto with_session = accepts_events;
    with_session.emplace_back("Mcp-Session-Id", session);
    const std::string current =
        connection.request("POST", "/mcp", call, {}, "HTTP/1.1", {}, "application/json", with_session);
    ASSERT_TRUE(current.starts_with("HTTP/1.1 200")) << current.substr(0, 300);
    EXPECT_EQ(response_header(current, "content-type"), "application/json; charset=utf-8");
    EXPECT_EQ(body_of(current).find("list_changed"), std::string::npos);
    EXPECT_TRUE(response_header(current, "mcp-session-id").empty());

    // A session that began on another tool list — lake was upgraded since — is told, ahead of the
    // response it asked for.
    auto stale = accepts_events;
    stale.emplace_back("Mcp-Session-Id", std::string(64, '0') + "." + std::string(32, '1'));
    const std::string changed =
        connection.request("POST", "/mcp", call, {}, "HTTP/1.1", {}, "application/json", stale);
    ASSERT_TRUE(changed.starts_with("HTTP/1.1 200")) << changed.substr(0, 300);
    EXPECT_EQ(response_header(changed, "content-type"), "text/event-stream");
    const std::string events = body_of(changed);
    const std::string notice =
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}"
        "\n\n";
    ASSERT_TRUE(events.starts_with(notice)) << events.substr(0, 300);
    const std::string second = events.substr(notice.size());
    ASSERT_TRUE(second.starts_with("event: message\ndata: ")) << second.substr(0, 300);
    ASSERT_TRUE(second.ends_with("\n\n"));
    const auto reply = nlohmann::json::parse(
        second.substr(std::string_view("event: message\ndata: ").size(),
                      second.size() - std::string_view("event: message\ndata: ").size() - 2));
    EXPECT_EQ(reply["id"], 2);
    EXPECT_EQ(reply["result"]["isError"], false);

    // tools/list is the fresh list itself, so refetching on the notice is not told again.
    const std::string relisted = connection.request(
        "POST", "/mcp", R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})", {}, "HTTP/1.1", {},
        "application/json", stale);
    ASSERT_TRUE(relisted.starts_with("HTTP/1.1 200")) << relisted.substr(0, 300);
    EXPECT_EQ(response_header(relisted, "content-type"), "application/json; charset=utf-8");
    EXPECT_FALSE(nlohmann::json::parse(body_of(relisted))["result"]["tools"].empty());

    // A client that accepts only JSON still gets its plain response.
    const std::string json_only = connection.request(
        "POST", "/mcp", call, {}, "HTTP/1.1", {}, "application/json",
        {{"Accept", "application/json"},
         {"Mcp-Session-Id", std::string(64, '0') + "." + std::string(32, '1')}});
    ASSERT_TRUE(json_only.starts_with("HTTP/1.1 200")) << json_only.substr(0, 300);
    EXPECT_EQ(nlohmann::json::parse(body_of(json_only))["id"], 2);

    // No standalone event stream and no session to delete: 405, never a 404 that reads as an
    // expired session.
    for (const std::string_view method : {"GET", "DELETE"}) {
        const std::string refused =
            connection.request(method, "/mcp", {}, {}, "HTTP/1.1", {}, "application/json",
                               with_session);
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 405")) << method << refused.substr(0, 300);
        EXPECT_EQ(response_header(refused, "allow"), "POST");
    }
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
    OutputPipe captured_stderr;
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

// Decision 54 S9 (lake#384): a request log line names the route pattern and the identifiers it
// captured, never the raw path, and a request routed to a workspace is counted in that workspace's
// own `lake_requests` series by the owner loop.
TEST(HttpServer, RequestLogsNameRoutesAndEachWorkspaceGetsItsOwnRequestSeries) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces(store);
    const auto create_space = [&store](const std::string& handle) {
        const auto user = store->create_user(
            {"taap:1:ed25519:" + handle, handle + "@example.test", handle});
        EXPECT_TRUE(user) << user.error().message;
        const auto customer = store->create_customer({"individual", user->id, "", handle});
        EXPECT_TRUE(customer) << customer.error().message;
        return customer->id;
    };
    const std::string mine = create_space("request-series-mine");
    const std::string theirs = create_space("request-series-theirs");
    const std::string home = workspaces.create(mine, "home");
    const std::string lab = workspaces.create(mine, "lab");
    const std::string other = workspaces.create(theirs, "home");
    const auto repository =
        store->create_repository({mine, "secret-project", "", "private", home});
    ASSERT_TRUE(repository) << repository.error().message;

    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.node_name = "request-series";
    options.admin_token = "request-series-admin";
    options.instance_kek = "726571756573742d7365726965732d696e7374616e63652d6b656b2d74657374";
    options.store = store;
    options.workspaces = workspaces.service;
    options.request_series_flush_interval = std::chrono::milliseconds(0);
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    std::string output;
    std::vector<std::string> statuses;
    {
        OutputPipe captured(STDOUT_FILENO, stdout);
        ASSERT_TRUE(captured.ready());
        HttpConnection connection(server.port());
        const auto get = [&](const std::string& path, std::string_view bearer = {}) {
            const std::string response = connection.request(
                "GET", path, {}, {}, "HTTP/1.1", bearer, "application/json",
                {{"X-Request-Id", "request-384"}});
            statuses.push_back(response.substr(0, response.find("\r\n")));
        };
        const std::string admin = options.admin_token;
        // A repository read: its repository's workspace.
        get("/v1/get_repository?id=" + repository->id, admin);
        get("/v1/get_repository?id=" + repository->id, admin);
        // A capability naming its workspace: that workspace, in either space.
        get("/v1/workspace_status?customer_id=" + mine + "&workspace_id=" + lab, admin);
        get("/v1/workspace_status?customer_id=" + theirs + "&workspace_id=" + other, admin);
        // No workspace: counted nowhere.
        get("/v1/server_info");
        // Registry, drive and repository sync routes whose paths carry names.
        get("/secret-package");
        get("/v2/" + mine + "/secret-image/manifests/latest", admin);
        get("/drive?path=secret-folder/secret-file.txt", admin);
        const std::string pull = connection.request(
            "POST", "/v1/repositories/" + repository->id + "/sync/pull", "not a sync request",
            {}, "HTTP/1.1", admin, "application/octet-stream", {{"X-Request-Id", "request-384"}});
        statuses.push_back(pull.substr(0, pull.find("\r\n")));
        server.drain();
        output = captured.finish();
    }
    server.stop();

    std::vector<nlohmann::json> lines;
    std::istringstream stream(output);
    for (std::string text; std::getline(stream, text);) {
        const auto line = nlohmann::json::parse(text, nullptr, false);
        if (!line.is_discarded() && line.is_object() && line.contains("route")) {
            lines.push_back(line);
        }
    }
    ASSERT_EQ(lines.size(), 9U) << output;
    for (const auto& line : lines) {
        EXPECT_FALSE(line.contains("path")) << line.dump();
        EXPECT_EQ(line["node"], "request-series");
        EXPECT_EQ(line["request_id"], "request-384") << line.dump();
    }
    EXPECT_EQ(output.find("secret-package"), std::string::npos) << output;
    EXPECT_EQ(output.find("secret-image"), std::string::npos) << output;
    EXPECT_EQ(output.find("secret-file"), std::string::npos) << output;
    EXPECT_EQ(output.find("secret-project"), std::string::npos) << output;
    EXPECT_EQ(lines[0]["route"], "/v1/get_repository");
    EXPECT_EQ(lines[0]["capability"], "get_repository");
    EXPECT_EQ(lines[0]["workspace_id"], home);
    EXPECT_EQ(lines[2]["workspace_id"], lab);
    EXPECT_EQ(lines[3]["workspace_id"], other);
    EXPECT_FALSE(lines[4].contains("workspace_id"));
    EXPECT_EQ(lines[5]["route"], "/:package");
    EXPECT_EQ(lines[6]["route"], "/v2/:space/:repository/manifests/:reference");
    EXPECT_EQ(lines[7]["route"], "/drive");
    EXPECT_EQ(lines[8]["route"], "/v1/repositories/:repository_id/sync/pull");
    EXPECT_EQ(lines[8]["repository_id"], repository->id);

    // Two workspaces of one space and one of another: three separate series.
    auto& home_series = workspaces.records(home);
    EXPECT_EQ(home_series.metric_total("lake_requests",
                                       {{"route", "/v1/get_repository"},
                                        {"capability", "get_repository"},
                                        {"status_class", "2xx"}}),
              2)
        << statuses[0];
    EXPECT_EQ(workspaces.records(lab).metric_total("lake_requests"), 1) << statuses[2];
    EXPECT_EQ(workspaces.records(lab).metric_total(
                  "lake_requests", {{"capability", "workspace_status"}}),
              1);
    EXPECT_EQ(workspaces.records(other).metric_total("lake_requests"), 1) << statuses[3];
    EXPECT_EQ(home_series.metric_total("lake_requests", {{"route", "/v1/server_info"}}), 0);
    EXPECT_EQ(home_series.metric_total("lake_requests", {{"route", "/:package"}}), 0);
    // Nothing reached the workspace on the request path: only the owner loop's flush wrote.
    EXPECT_EQ(std::ranges::count(home_series.calls, std::string("record_metric")), 1);
}

namespace {

using json = nlohmann::json;

class DelayedPageWorkspace final : public lake::control::WorkspaceCaller {
public:
    liquid::runtime::task<lake::control::ZettaControlResult<json>>
    call(std::string, std::string, std::string capability, bool, json input) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            ++capability_calls_[capability];
        }
        auto [delay_error] =
            co_await liquid::runtime::delay(std::chrono::milliseconds(30));
        if (delay_error) {
            co_return lake::control::ZettaControlResult<json>(std::unexpected(
                lake::control::ZettaControlError{"store_error", delay_error.message()}));
        }
        co_return answer(capability, input);
    }

    std::size_t call_count() const {
        return calls_.load(std::memory_order_relaxed);
    }

    std::size_t call_count(const std::string& capability) const {
        std::lock_guard lock(mutex_);
        const auto found = capability_calls_.find(capability);
        return found == capability_calls_.end() ? 0 : found->second;
    }

    void dataset_count(std::size_t count) {
        datasets_.store(count, std::memory_order_relaxed);
    }

    std::map<std::string, std::size_t> capability_counts() const {
        std::lock_guard lock(mutex_);
        return capability_calls_;
    }

private:
    std::atomic<std::size_t> calls_{0};
    std::atomic<std::size_t> datasets_{50};
    mutable std::mutex mutex_;
    std::map<std::string, std::size_t> capability_calls_;

    static json study(std::size_t index) {
        return {{"id", "study-" + std::to_string(index)},
                {"status", "complete"},
                {"created", "2026-09-28T18:20:00.000Z"},
                {"updated", "2026-09-28T18:20:03.000Z"},
                {"spec",
                 {{"name", "Study " + std::to_string(index)},
                  {"project", "project-0"},
                  {"pipeline", "pipeline-0"},
                  {"dataset", "dataset-0"},
                  {"methodology", "random"},
                  {"objective",
                   {{"metric", "validation.auc"},
                    {"direction", "maximize"},
                    {"validation_split", "validation"}}},
                  {"budgets",
                   {{"max_trials", 20},
                    {"max_parallel_trials", 2},
                    {"max_wall_time_ms", 60000},
                    {"cost_cap", 10}}}}},
                {"spent_cost", 1.0},
                {"stop_reason", ""},
                {"promoted_model", ""},
                {"trials", json::array()}};
    }

    static json run(std::size_t index) {
        return {{"id", "run-" + std::to_string(index)},
                {"project", "project-id-" + std::to_string(index % 50)},
                {"graph", "graph-id-" + std::to_string(index % 50)},
                {"project_name", "project-" + std::to_string(index % 50)},
                {"pipeline_name", "pipeline-" + std::to_string(index % 50)},
                {"status", "succeeded"},
                {"created", "2026-09-28T18:20:00.000Z"},
                {"updated", "2026-09-28T18:20:03.000Z"},
                {"outputs", json::array()},
                {"log_count", 0},
                {"output_count", 0}};
    }

    static json model(std::size_t index) {
        return {{"id", "model-" + std::to_string(index)},
                {"name", "model-" + std::to_string(index)},
                {"project", "project-id-" + std::to_string(index)},
                {"pipeline", "graph-id-" + std::to_string(index)},
                {"project_name", "project-" + std::to_string(index)},
                {"pipeline_name", "pipeline-" + std::to_string(index)},
                {"run_id", "run-" + std::to_string(index)},
                {"created", "2026-09-28T18:20:03.000Z"},
                {"exposed", index % 2 == 0},
                {"metrics", {{"held_out_auc", 0.91}}},
                {"feature_schema", {{"features", json::array()}}}};
    }

    static json projects() {
        json rows = json::array();
        for (std::size_t index = 0; index < 50; ++index) {
            rows.push_back(
                {{"id", "project-id-" + std::to_string(index)},
                 {"name", "project-" + std::to_string(index)},
                 {"graphs",
                  json::array({{{"id", "graph-id-" + std::to_string(index)},
                                {"name", "pipeline-" + std::to_string(index)},
                                {"approval", "approved"}}})}});
        }
        return {{"projects", std::move(rows)}};
    }

    static json runs() {
        json rows = json::array();
        for (std::size_t index = 0; index < 200; ++index) {
            rows.push_back(run(index));
        }
        return {{"runs", std::move(rows)}};
    }

    static json models() {
        json rows = json::array();
        for (std::size_t index = 0; index < 50; ++index) {
            rows.push_back(model(index));
        }
        return {{"models", std::move(rows)}};
    }

    static json studies() {
        json rows = json::array();
        for (std::size_t index = 0; index < 20; ++index) {
            rows.push_back(study(index));
        }
        return {{"studies", std::move(rows)}};
    }

    static json topics() {
        json rows = json::array();
        for (std::size_t index = 0; index < 50; ++index) {
            rows.push_back({{"topic", "topic:topic-" + std::to_string(index)},
                            {"subscribers", 1},
                            {"held_events", 0},
                            {"next_sequence", 1}});
        }
        return {{"topics", std::move(rows)}};
    }

    json answer(const std::string& capability, const json& input) const {
        if (capability == "list_datasets") {
            json rows = json::array();
            for (std::size_t index = 0; index < datasets_.load(std::memory_order_relaxed); ++index) {
                rows.push_back({{"name", "dataset-" + std::to_string(index)},
                    {"database", "datasets"}, {"rows", 100000}, {"size_bytes", 8000000},
                    {"format", "parquet"}});
            }
            return {{"datasets", std::move(rows)}};
        }
        if (capability == "list_methodologies") {
            json rows = json::array();
            for (std::size_t index = 0; index < 21; ++index) {
                rows.push_back({{"id", "M-" + std::to_string(index)},
                    {"name", "Methodology " + std::to_string(index)}, {"family", "Prediction"},
                    {"runnable", true}, {"partial", false}, {"issue", ""},
                    {"inputs", json::array({"training data"})},
                    {"outputs", json::array({"predictions"})},
                    {"metrics", json::array({"accuracy"})},
                    {"model_kinds", json::array({"tabular"})}});
            }
            return {{"methodologies", std::move(rows)}};
        }
        if (capability == "list_solutions") {
            json rows = json::array();
            for (std::size_t index = 0; index < 20; ++index) {
                rows.push_back({{"id", "solution-" + std::to_string(index)},
                    {"name", "Solution " + std::to_string(index)},
                    {"methodologies", json::array({"M-1"})},
                    {"description", std::string(3000, 'x')}});
            }
            return {{"solutions", std::move(rows)}};
        }
        if (capability == "list_dataset_generators") {
            return {{"generators", json::array({{{"kind", "base"}, {"title", "Base"}},
                {{"kind", "suggest"}, {"title", "Suggest"}},
                {{"kind", "authored"}, {"title", "Authored"}}})}};
        }
        if (capability == "list_projects") {
            return projects();
        }
        if (capability == "list_runs") {
            return runs();
        }
        if (capability == "list_models") {
            return models();
        }
        if (capability == "list_studies") {
            return studies();
        }
        if (capability == "list_topics") {
            return topics();
        }
        if (capability == "get_project") {
            return {{"id", "project-id-0"},
                    {"name", input.value("name", "project-0")},
                    {"graphs",
                     json::array({{{"id", "graph-id-0"},
                                   {"name", "pipeline-0"},
                                   {"approval", "approved"}}})}};
        }
        if (capability == "get_graph") {
            return {{"graph_id", "graph-id-0"},
                    {"name", input.value("graph", "pipeline-0")},
                    {"approval", "approved"},
                    {"nodes", json::array()},
                    {"edges", json::array()}};
        }
        if (capability == "get_run") {
            json value = run(0);
            value["status"] = "running";
            return value;
        }
        if (capability == "get_study") {
            return study(0);
        }
        if (capability == "get_study_analysis") {
            return {{"best_trial", nullptr},
                    {"optimization_history", json::array()},
                    {"parameter_importances", json::array()},
                    {"parallel_coordinates", json::array()},
                    {"leaderboard", json::array()}};
        }
        if (capability == "get_run_metrics") {
            return {{"run", input.value("run", "run-0")},
                    {"params", json::object()},
                    {"series", json::array()},
                    {"artifacts", json::array()}};
        }
        if (capability == "get_topic") {
            return {{"topic", "topic:topic-0"},
                    {"settings", {{"exposed", true}}},
                    {"subscriber_count", 1},
                    {"held_events", 0},
                    {"earliest_sequence", 1},
                    {"next_sequence", 1},
                    {"subscriptions", json::array()}};
        }
        if (capability == "browse_topic") {
            return {{"topic", "topic:topic-0"},
                    {"events", json::array()},
                    {"earliest_sequence", 1},
                    {"next_sequence", 1}};
        }
        if (capability == "get_dataset") {
            return {{"name", input.value("dataset", "dataset-0")},
                    {"columns", json::array()},
                    {"partitions", json::array()}};
        }
        if (capability == "get_index") {
            return {{"id", input.value("index_id", "index-0")},
                    {"name", "index-0"},
                    {"exposed", false},
                    {"fields", json::array()}};
        }
        if (capability == "get_agent") {
            return {{"id", input.value("id", "agent-0")}, {"name", "agent-0"}};
        }
        if (capability == "agent_training_status") {
            return {{"status", "ready"}};
        }
        if (capability == "search") {
            return {{"hits", json::array()}};
        }
        if (capability == "sql") {
            // Zetta's system.datasets names the table column `dataset` (lake#447).
            if (input.value("statement", "") == "SELECT database, dataset FROM system.datasets") {
                json rows = json::array();
                for (std::size_t index = 0; index < datasets_.load(std::memory_order_relaxed); ++index) {
                    rows.push_back({{"dataset", "dataset-" + std::to_string(index)},
                                    {"database", "datasets"}});
                }
                return {{"rows", std::move(rows)}};
            }
            return {{"rows", json::array()}};
        }
        if (capability.starts_with("list_")) {
            return json::object();
        }
        return json::object();
    }
};

std::string concrete_route(std::string pattern, const std::string& workspace_id) {
    std::size_t begin = 0;
    while (begin < pattern.size()) {
        const std::size_t slash = pattern.find('/', begin + 1);
        const std::size_t end = slash == std::string::npos ? pattern.size() : slash;
        if (begin + 1 < pattern.size() &&
            (pattern[begin + 1] == ':' || pattern[begin + 1] == '*')) {
            const std::string parameter = pattern.substr(begin + 1, end - begin - 1);
            pattern.replace(begin + 1, end - begin - 1,
                            parameter == ":workspace_id" ? workspace_id : "fixture");
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = pattern.find('/', begin + 1);
    }
    return pattern;
}

// The Zetta capabilities one response called: the fake's per-capability counts after it, less
// those before it, one entry per call, sorted.
std::vector<std::string> called_capabilities(const std::map<std::string, std::size_t>& before,
                                             const std::map<std::string, std::size_t>& after) {
    std::vector<std::string> called;
    for (const auto& [capability, count] : after) {
        const auto earlier = before.find(capability);
        const std::size_t previous = earlier == before.end() ? 0 : earlier->second;
        for (std::size_t index = previous; index < count; ++index) {
            called.push_back(capability);
        }
    }
    std::ranges::sort(called);
    return called;
}

// The capabilities a response's Server-Timing names (zetta_<capability>;dur=...), sorted.
std::vector<std::string> server_timing_capabilities(const std::string& response) {
    const std::string timing = response_header(response, "server-timing");
    std::vector<std::string> named;
    std::size_t at = 0;
    while ((at = timing.find("zetta_", at)) != std::string::npos) {
        const std::size_t end = timing.find(';', at);
        named.push_back(timing.substr(at + 6, end - at - 6));
        at = end;
    }
    std::ranges::sort(named);
    return named;
}

double total_server_milliseconds(const std::string& response) {
    const std::string timing = response_header(response, "server-timing");
    constexpr std::string_view prefix = "total;dur=";
    const std::size_t start = timing.find(prefix);
    if (start == std::string::npos) {
        return -1.0;
    }
    const std::size_t value_start = start + prefix.size();
    const std::size_t end = timing.find(',', value_start);
    return std::stod(timing.substr(value_start, end - value_start));
}

std::size_t workspace_call_count(const std::string& response) {
    const std::string timing = response_header(response, "server-timing");
    constexpr std::string_view prefix = "workspace;dur=";
    const std::size_t workspace = timing.find(prefix);
    const std::size_t description = timing.find(";desc=\"", workspace);
    if (workspace == std::string::npos || description == std::string::npos) {
        return std::numeric_limits<std::size_t>::max();
    }
    const std::size_t value_start = description + std::string_view(";desc=\"").size();
    const std::size_t end = timing.find(' ', value_start);
    return static_cast<std::size_t>(std::stoull(timing.substr(value_start, end - value_start)));
}

std::size_t workspace_round_count(const std::string& response) {
    const std::string timing = response_header(response, "server-timing");
    constexpr std::string_view prefix = "workspace_rounds;desc=\"";
    const auto start = timing.find(prefix);
    if (start == std::string::npos) {
        return std::numeric_limits<std::size_t>::max();
    }
    return std::stoull(timing.substr(start + prefix.size()));
}

void populate_metering_store(lake::control::Store& control_store,
                            const std::filesystem::path& root,
                            const std::string& customer_id) {
    auto objects = liquid::runtime::block_on(liquid::store::open(root / customer_id / "store"));
    ASSERT_TRUE(objects) << objects.error().message;
    for (std::size_t index = 0; index < 4; ++index) {
        const auto row = control_store.create_repository(
            {customer_id, "metering-" + std::to_string(index), "", "private",
             lake::test::repository_workspace(control_store, customer_id)});
        ASSERT_TRUE(row);
        liquid::repository_options options;
        options.store = &*objects;
        auto repository = liquid::runtime::block_on(liquid::repository::create(
            root / customer_id / "repositories" / row->id, options));
        ASSERT_TRUE(repository) << repository.error().message;
        for (std::size_t file = 0; file < 32; ++file) {
            const std::vector<std::byte> contents(4096, std::byte(1 + index * 32 + file));
            ASSERT_TRUE(liquid::runtime::block_on(repository->write(
                "file-" + std::to_string(file) + ".bin", contents)));
        }
        ASSERT_TRUE(liquid::runtime::block_on(repository->cut("metering fixture")));
    }
}

void verify_cold_metering(lake::api::ApiServer& server,
                          const std::string& customer_id,
                          const std::string& cookie) {
    HttpConnection connection(server.port());
    const std::string path = "/metering?customer_id=" + customer_id;
    const std::string first = connection.request("GET", path, {}, cookie);
    ASSERT_TRUE(first.starts_with("HTTP/1.1 200")) << first;
    const double cold_ms = total_server_milliseconds(first);
    std::println("PAGE_COLD /metering total_ms={:.3f} repositories=4 files=128", cold_ms);
    ASSERT_GE(cold_ms, 0.0);
    const auto response = connection.request("GET", "/v1/metering_usage?customer_id=" + customer_id,
        {}, cookie);
    const auto body = json::parse(response.substr(response.find("\r\n\r\n") + 4));
    ASSERT_TRUE(body.at("ok")) << body;
    EXPECT_GT(
        body.at("value").at("summary").at("storage").at("repositories_bytes").get<std::uint64_t>(),
        0U);
}

} // namespace

TEST(PagePerformance, EveryRegisteredGetPageKeepsItsWorkspaceCallBudget) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is required for authenticated page performance";
    }
    const std::string instance_kek = std::getenv("LAKE_INSTANCE_KEK");
    std::shared_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(uri, instance_kek);
    const auto identity = lake::audit::mint_custodial_identity(instance_kek);
    ASSERT_TRUE(identity);
    const auto session = store->sign_in_clerk(
        {"page-performance", "page-performance@example.test", "Page performance", "fixture",
         identity->taap_id, identity->public_key, identity->wrapped_seed});
    ASSERT_TRUE(session);
    const auto principal = store->context_for(session->secret);
    ASSERT_TRUE(principal);
    const auto customers = store->list_customers(2, principal->user_id);
    ASSERT_TRUE(customers);
    ASSERT_EQ(customers->size(), 1U);
    const std::string customer_id = customers->front().id;

    const std::filesystem::path workspace_directory =
        lake::test::fixture_path("page-performance-" + lake::control::new_uuid_v4());
    std::filesystem::create_directories(workspace_directory);
    auto caller = std::make_shared<DelayedPageWorkspace>();
    auto workspaces = std::make_shared<lake::control::WorkspaceService>(
        lake::control::WorkspaceConfig{workspace_directory.string(), 1}, store, caller);
    const auto requested = liquid::runtime::block_on(workspaces->request(customer_id));
    ASSERT_TRUE(requested);
    ASSERT_TRUE(*requested);
    {
        pqxx::connection database(uri);
        pqxx::work tx(database);
        tx.exec("INSERT INTO service_status (service_id, phase, endpoint) "
                "VALUES ($1::uuid, 'Ready', 'http://workspace.example.test')",
                pqxx::params{(*requested)->service_id});
        tx.commit();
    }
    std::ofstream(workspace_directory / ((*requested)->slug + ".json"))
        << json{{"kind", "zetta"}, {"token", "page-performance-token"}}.dump();
    const std::string workspace_id = (*requested)->workspace_id;
    ASSERT_FALSE(workspace_id.empty());
    std::string app_id;
    for (std::size_t index = 0; index < 20; ++index) {
        lake::control::NewWebApp app;
        app.customer_id = customer_id;
        app.workspace_id = workspace_id;
        app.name = "app-" + std::to_string(index);
        app.mode = "image";
        app.image = "registry.example.test/app:latest";
        app.port = 8080;
        app.host = app.name + ".apps.example.test";
        app.max_apps = 20;
        app.max_replicas = 20;
        const auto created = store->create_web_app(app);
        ASSERT_TRUE(created);
        app_id = created->id;
    }

    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 4;
    options.store = store;
    options.workspaces = workspaces;
    options.admin_token = "page-performance-operator";
    options.instance_kek = instance_kek;
    options.apps_per_customer = 20;
    options.app_replicas_per_customer = 20;
    options.store_root = (workspace_directory / "repositories").string();
    ASSERT_NO_FATAL_FAILURE(populate_metering_store(*store, options.store_root, customer_id));
    lake::api::ApiServer server(std::move(options));
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string query =
        "?customer_id=" + customer_id +
        "&id=fixture&name=project-0&project=project-0&graph=pipeline-0"
        "&study=study-0&run=run-0&runs=run-0,run-1&app_id=" + app_id +
        "&topic=topic-0&zetta=workspace"
        // An open conversation: Ask Orama and the Agent Factory read it from the workspace's
        // records (lake#370).
        "&conversation=5f0c2a1e-3b4d-4c6e-8f70-9a1b2c3d4e5f";
    const std::string cookie = "lake_session=" + session->secret;
    ASSERT_NO_FATAL_FAILURE(verify_cold_metering(server, customer_id, cookie));
    const auto protected_app = store->get_web_app(app_id);
    ASSERT_TRUE(protected_app);
    const auto app_session = lake::control::issue_web_app_session(
        instance_kek, app_id, principal->user_id, workspace_id);
    ASSERT_TRUE(app_session) << app_session.error();
    std::vector<double> app_auth_times;
    app_auth_times.reserve(20);
    for (int sample = 0; sample < 20; ++sample) {
        HttpConnection connection(server.port());
        const std::size_t calls_before = caller->call_count();
        const std::string response = connection.request("GET",
                                                        "/w/" + workspace_id + "/v1/apps/auth",
                                                        {},
                                                        "lake_app_session=" + *app_session,
                                                        "HTTP/1.1",
                                                        {},
                                                        {},
                                                        {{"X-Original-Method", "GET"},
                                                         {"X-Original-URL",
                                                          "http://" + protected_app->host + "/"}});
        ASSERT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
        EXPECT_EQ(workspace_call_count(response), 0U);
        EXPECT_EQ(caller->call_count(), calls_before);
        app_auth_times.push_back(total_server_milliseconds(response));
    }
    std::ranges::sort(app_auth_times);
    std::println("PAGE_PERF /w/:workspace_id/v1/apps/auth p95_ms={:.3f} calls=0 rounds=0",
                 app_auth_times.at(18));
    EXPECT_LT(app_auth_times.at(18), 100.0);

    std::vector<std::tuple<std::string, std::string, std::string>> pages;
    struct WorkspaceCallBudget {
        std::size_t calls = 0;
        std::size_t rounds = 0;
        // When set, exactly these Zetta capabilities, sorted, one entry per call (lake#446).
        std::optional<std::vector<std::string>> capabilities;
    };
    using Capabilities = std::vector<std::string>;
    // Measured slow on the dev site (docs/ui/performance.md, lake#446): Zetta's agent store reads
    // copy its whole replicated catalogue, and its dataset listing scans every table. No page may
    // wait for one except the pages named here, which are over the live budget until Zetta is
    // fixed. Deferred panels may.
    const std::set<std::string> slow_capabilities{
        "list_agents", "get_agent", "agent_training_status", "list_datasets"};
    const std::map<std::string, std::set<std::string>> slow_capability_exceptions{
        {"/w/:workspace_id/train/agent", {"get_agent", "agent_training_status"}},
        {"/w/:workspace_id/design/pipeline", {"list_datasets"}},
        {"/w/:workspace_id/design/project", {"list_datasets"}},
        {"/w/:workspace_id/design/solution", {"list_datasets"}},
        // Usage needs every dataset's byte total (lake#444).
        {"/metering", {"list_datasets"}}};
    // The deferred panels: JSON, filled in after their page renders. Each is held to its budget.
    const std::map<std::string, WorkspaceCallBudget> panel_call_budgets{
        {"/w/:workspace_id/ask/agents", {1, 1, Capabilities{"list_agents"}}},
        {"/w/:workspace_id/train/agents/list", {1, 1, Capabilities{"list_agents"}}},
        {"/w/:workspace_id/ship/indexes/datasets", {1, 1, Capabilities{"sql"}}}};
    const std::map<std::string, WorkspaceCallBudget> workspace_call_budgets{
        // Ask Orama, Agents, Search Indexes, Models and Experiments: lake#446.
        {"/w/:workspace_id/ask", {1, 1, Capabilities{"list_records"}}},
        // Design → Opportunities under its earlier path, the same page.
        {"/w/:workspace_id/design/discover",
         {2, 1, Capabilities{"list_methodologies", "list_records"}}},
        {"/w/:workspace_id/design/methodologies", {1, 1}},
        {"/w/:workspace_id/design/methodology", {2, 1}},
        // The saved Opportunities (lake#376) beside the methodology registry, concurrently.
        {"/w/:workspace_id/design/opportunities",
         {2, 1, Capabilities{"list_methodologies", "list_records"}}},
        {"/w/:workspace_id/design/pipeline", {3, 1}},
        {"/w/:workspace_id/design/project", {9, 2}},
        {"/w/:workspace_id/design/projects", {1, 1}},
        {"/w/:workspace_id/design/solution", {3, 1}},
        {"/w/:workspace_id/design/solutions", {1, 1}},
        {"/w/:workspace_id/ship/app", {1, 1}},
        {"/w/:workspace_id/ship/index", {1, 1}},
        {"/w/:workspace_id/ship/indexes", {1, 1, Capabilities{"list_indexes"}}},
        {"/w/:workspace_id/ship/models", {2, 1, Capabilities{"list_models", "list_studies"}}},
        {"/w/:workspace_id/train/agent", {3, 1}},
        {"/w/:workspace_id/train/agents", {0, 0, Capabilities{}}},
        {"/w/:workspace_id/train/agents/new", {1, 1, Capabilities{"list_records"}}},
        {"/w/:workspace_id/train/data", {3, 1}},
        {"/w/:workspace_id/train/dataset", {3, 1}},
        {"/w/:workspace_id/train/experiment", {2, 1}},
        {"/w/:workspace_id/train/experiment/run", {1, 1}},
        {"/w/:workspace_id/train/experiments", {2, 1, Capabilities{"list_runs", "list_studies"}}},
        {"/w/:workspace_id/train/experiments/compare", {2, 1}},
        {"/w/:workspace_id/train/run", {3, 1}},
        {"/w/:workspace_id/train/runs", {2, 1}},
        // The topic, its creator record (lake#379) and its messages, read together.
        {"/w/:workspace_id/train/stream", {3, 1}},
        {"/w/:workspace_id/train/streams", {1, 1}},
        // Usage reads each workspace's datasets, runs, and repository transfer and build records
        // (lake#348, lake#349) together, in one round.
        {"/metering", {4, 1}},
        // Activity and Audit read the central log and each workspace's audit (decision 52,
        // lake#317): one list_records walk per workspace, all in one round. The fixture has one.
        {"/activity", {1, 1, Capabilities{"list_records"}}},
        // Repository descriptions are settings records in each repository's workspace (lake#315):
        // one list_records per workspace, all started together.
        {"/repositories", {1, 1, Capabilities{"list_records"}}},
        {"/audit", {1, 1, Capabilities{"list_records"}}}};
    const auto workspace_call_budget = [&workspace_call_budgets](const std::string& pattern) {
        const auto found = workspace_call_budgets.find(pattern);
        return found == workspace_call_budgets.end() ? WorkspaceCallBudget{} : found->second;
    };
    // Every response names each Zetta call it made, and the page waits for no slow capability
    // it is not allowed to.
    const auto check_capabilities = [&](const std::string& pattern,
                                        const std::string& response,
                                        const Capabilities& called,
                                        const WorkspaceCallBudget& budget,
                                        bool panel) {
        EXPECT_EQ(server_timing_capabilities(response), called) << pattern;
        if (budget.capabilities) {
            EXPECT_EQ(called, *budget.capabilities) << pattern;
        }
        if (panel) {
            return;
        }
        const auto allowed = slow_capability_exceptions.find(pattern);
        for (const std::string& capability : called) {
            EXPECT_TRUE(!slow_capabilities.contains(capability) ||
                        (allowed != slow_capability_exceptions.end() &&
                         allowed->second.contains(capability)))
                << pattern << " waits for the slow Zetta capability " << capability;
        }
    };
    std::set<std::string> observed_workspace_pages;
    std::set<std::string> observed_panels;
    const auto routes = server.get_route_patterns();
    ASSERT_FALSE(routes.empty());
    for (const std::string& pattern : routes) {
        workspaces->invalidate_registry(customer_id, workspace_id);
        const std::string path = concrete_route(pattern, workspace_id) + query;
        constexpr std::string_view workspace_prefix = "/w/:workspace_id";
        const bool panel = pattern.starts_with(workspace_prefix) &&
                           lake::api::web_ui_deferred_panel_path(
                               pattern.substr(workspace_prefix.size()));
        const bool workspace_page = pattern.starts_with(workspace_prefix) &&
                                    !pattern.starts_with(
                                        "/w/:workspace_id/workspaces/") &&
                                    lake::api::workspace_page_path(
                                        pattern.substr(workspace_prefix.size())) &&
                                    !panel;
        std::string page_cookie = cookie;
        HttpConnection connection(server.port());
        std::size_t calls_before = caller->call_count();
        auto capabilities_before = caller->capability_counts();
        std::string response = connection.request("GET", path, {}, page_cookie);
        std::string content_type = response_header(response, "content-type");
        if (panel) {
            // A deferred panel answers JSON for the signed-in user, within its own budget.
            ASSERT_TRUE(response.starts_with("HTTP/1.1 200")) << pattern << '\n' << response;
            ASSERT_TRUE(content_type.starts_with("application/json")) << pattern;
            const auto body = json::parse(response.substr(response.find("\r\n\r\n") + 4));
            EXPECT_FALSE(body.contains("error")) << pattern << ' ' << body;
            const auto found = panel_call_budgets.find(pattern);
            ASSERT_NE(found, panel_call_budgets.end()) << pattern;
            EXPECT_EQ(workspace_call_count(response), caller->call_count() - calls_before)
                << pattern;
            EXPECT_EQ(workspace_call_count(response), found->second.calls) << pattern;
            EXPECT_EQ(workspace_round_count(response), found->second.rounds) << pattern;
            check_capabilities(pattern,
                               response,
                               called_capabilities(capabilities_before,
                                                   caller->capability_counts()),
                               found->second,
                               true);
            observed_panels.insert(pattern);
            continue;
        }
        if (!workspace_page && pattern != "/workspaces") {
            for (const std::string alternate : {"lake_session=page-performance-operator", ""}) {
                if (content_type.starts_with("text/html")) {
                    break;
                }
                HttpConnection alternate_connection(server.port());
                page_cookie = alternate;
                calls_before = caller->call_count();
                capabilities_before = caller->capability_counts();
                response = alternate_connection.request("GET", path, {}, page_cookie);
                content_type = response_header(response, "content-type");
            }
        }
        if (workspace_page || pattern == "/workspaces" || pattern == "/metering") {
            ASSERT_TRUE(response.starts_with("HTTP/1.1 200")) << pattern << '\n' << response;
            ASSERT_TRUE(content_type.starts_with("text/html")) << pattern;
            EXPECT_NE(response.find("<select name=\"workspace_id\""), std::string::npos)
                << pattern;
            EXPECT_NE(response.find(workspace_id), std::string::npos) << pattern;
        }
        if (content_type.starts_with("text/html")) {
            if (pattern == "/w/:workspace_id/train/data") {
                EXPECT_NE(response.find("dataset-49"), std::string::npos);
                EXPECT_NE(response.find("Open a dataset"), std::string::npos);
            }
            EXPECT_GE(total_server_milliseconds(response), 0.0) << pattern;
            EXPECT_EQ(workspace_call_count(response), caller->call_count() - calls_before)
                << pattern;
            const WorkspaceCallBudget budget = workspace_call_budget(pattern);
            EXPECT_EQ(workspace_call_count(response), budget.calls) << pattern;
            EXPECT_EQ(workspace_round_count(response), budget.rounds) << pattern;
            check_capabilities(pattern,
                               response,
                               called_capabilities(capabilities_before,
                                                   caller->capability_counts()),
                               budget,
                               false);
            if (workspace_call_budgets.contains(pattern)) {
                observed_workspace_pages.insert(pattern);
            }
            pages.emplace_back(pattern, path, page_cookie);
        }
    }

    ASSERT_FALSE(pages.empty());
    std::println("PAGE_COVERAGE routes={} latency_ms=30 renders=20 budget_ms=500", pages.size());
    EXPECT_EQ(observed_workspace_pages.size(), workspace_call_budgets.size());
    EXPECT_EQ(observed_panels.size(), panel_call_budgets.size());
    for (const auto& [pattern, path, page_cookie] : pages) {
        std::vector<std::pair<double, std::string>> samples;
        std::size_t calls = 0;
        std::size_t rounds = 0;
        samples.reserve(20);
        for (int sample = 0; sample < 20; ++sample) {
            workspaces->invalidate_registry(customer_id, workspace_id);
            HttpConnection connection(server.port());
            const std::size_t calls_before = caller->call_count();
            const auto capabilities_before = caller->capability_counts();
            const std::string response = connection.request("GET", path, {}, page_cookie);
            if (pattern.starts_with("/w/:workspace_id/") || pattern == "/workspaces") {
                ASSERT_TRUE(response.starts_with("HTTP/1.1 200")) << pattern << '\n' << response;
            }
            ASSERT_TRUE(response_header(response, "content-type").starts_with("text/html"))
                << pattern;
            const double elapsed = total_server_milliseconds(response);
            calls = std::max(calls, workspace_call_count(response));
            rounds = std::max(rounds, workspace_round_count(response));
            ASSERT_GE(elapsed, 0.0) << pattern;
            EXPECT_EQ(workspace_call_count(response), caller->call_count() - calls_before)
                << pattern;
            const WorkspaceCallBudget budget = workspace_call_budget(pattern);
            EXPECT_EQ(workspace_call_count(response), budget.calls) << pattern;
            EXPECT_EQ(workspace_round_count(response), budget.rounds) << pattern;
            check_capabilities(pattern,
                               response,
                               called_capabilities(capabilities_before,
                                                   caller->capability_counts()),
                               budget,
                               false);
            samples.emplace_back(elapsed, response_header(response, "server-timing"));
        }
        std::ranges::sort(samples);
        const double p95 = samples.at(18).first;
        std::println("PAGE_PERF {} p95_ms={:.3f} calls={} rounds={}", pattern, p95, calls, rounds);
        std::println("PAGE_TIMING {} {}", pattern, samples.at(18).second);
        EXPECT_LT(p95, 500.0) << pattern;
    }

    // The reported live population must remain a single projected list, without a sample
    // or metadata request per item. Changing it between requests must not show stale rows.
    caller->dataset_count(19);
    const auto samples_before = caller->call_count("get_dataset");
    const auto lists_before = caller->call_count("list_datasets");
    const auto sql_before = caller->call_count("sql");
    std::vector<double> dataset_times;
    for (int sample = 0; sample < 20; ++sample) {
        HttpConnection connection(server.port());
        const auto response = connection.request("GET", "/w/" + workspace_id +
            "/train/data?customer_id=" + customer_id, {}, cookie);
        ASSERT_TRUE(response.starts_with("HTTP/1.1 200"));
        EXPECT_NE(response.find("dataset-18"), std::string::npos);
        EXPECT_EQ(response.find("dataset-19"), std::string::npos);
        EXPECT_LE(workspace_round_count(response), 1U);
        dataset_times.push_back(total_server_milliseconds(response));
    }
    EXPECT_EQ(caller->call_count("get_dataset"), samples_before);
    EXPECT_EQ(caller->call_count("list_datasets"), lists_before);
    EXPECT_EQ(caller->call_count("sql") - sql_before, 20U);
    std::ranges::sort(dataset_times);
    std::println("PAGE_DATA_POPULATION datasets=19 p95_ms={:.3f}", dataset_times.at(18));
    EXPECT_LT(dataset_times.at(18), 500.0);
}
