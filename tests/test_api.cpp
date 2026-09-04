#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <liquid/liquid.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/server.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/telemetry/metrics.hpp"

using nlohmann::json;
using namespace lake::api;

namespace {

struct Surface {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext ctx;
    Surface() {
        register_core_capabilities(registry, "0.0.1");
        ctx.store = &store;
        ctx.node_name = "test";
        ctx.instance_kek = "test-instance-kek";
        ctx.authorized = true;
        ctx.acting_identity = "admin";
        ctx.operator_credential = true;
    }
    ApiResult call(const std::string& name, json input = json::object()) {
        ctx.request_id.clear();
        return registry.call(name, input, ctx);
    }
};

struct RepositoryFixtureStore final : SyncStore {
    SyncStoreResult<std::vector<std::string>>
    find_missing(const std::string&, const std::vector<std::string>&) override {
        return std::vector<std::string>{};
    }

    SyncStoreResult<void>
    write(const std::string&, const std::string&, std::vector<std::byte>) override {
        return {};
    }

    SyncStoreResult<std::vector<std::byte>> read(const std::string&, const std::string&) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<RepositoryContentRef>>
    repository_refs(const std::string&, const std::string&) override {
        return std::vector<RepositoryContentRef>{{"main", "change-1"}};
    }

    SyncStoreResult<std::vector<RepositoryTreeEntry>>
    repository_tree(const std::string&, const std::string&, const std::string&) override {
        return std::vector<RepositoryTreeEntry>{{"readme.md", "blob", "blob-1", 0644}};
    }

    SyncStoreResult<RepositoryFile> repository_file(const std::string&,
                                                    const std::string&,
                                                    const std::string&,
                                                    const std::string&) override {
        return RepositoryFile{"readme.md", "blob-1", "hello", 5};
    }

    SyncStoreResult<std::vector<RepositoryChange>> repository_log(const std::string&,
                                                                  const std::string&) override {
        return std::vector<RepositoryChange>{
            {"change-1", {}, "taap:1:ed25519:author", "initial", "2026-09-04T00:00:00Z"}};
    }

    SyncStoreResult<std::vector<RepositoryDiffEntry>>
    repository_diff(const std::string&, const std::string&, const std::string&) override {
        return std::vector<RepositoryDiffEntry>{
            {.path = "readme.md",
             .kind = "modified",
             .changes = {"modified"},
             .old_mode = 0644,
             .new_mode = 0644,
             .old_blob = "blob-1",
             .new_blob = "blob-2",
             .line_hunks = std::vector<RepositoryLineHunk>{{1, 1, 1, 1, {"old"}, {"hello"}}}}};
    }

    SyncStoreResult<RepositoryArtifactPage> repository_artifacts(
        const std::string&, const std::string&, const RepositoryArtifactFilter&) override {
        return RepositoryArtifactPage{.items = {{.id = "result-1",
                                                 .action = "action-1",
                                                 .outputs = {{"build/app", 0755, "blob-3"}},
                                                 .cache_status = "hit",
                                                 .worker = "worker-1",
                                                 .observed_at = "2026-09-04T00:00:00Z"}},
                                      .done = true};
    }

    SyncStoreResult<RepositoryArtifact> repository_artifact(const std::string&,
                                                            const std::string&,
                                                            const std::string& result_id) override {
        return RepositoryArtifact{
            .id = result_id,
            .action = "action-1",
            .outputs = {{"build/app", 0755, "blob-3"}},
            .timing = {.queued_ms = 1, .sandbox_ms = 2, .exec_ms = 3, .hash_ms = 4, .store_ms = 5},
            .cache_status = "hit",
            .worker = "worker-1",
            .observed_at = "2026-09-04T00:00:00Z",
            .attestations = {{"attestation-1",
                              "slsa_provenance/1",
                              "subject-1",
                              "taap:1:ed25519:builder",
                              true,
                              ""}},
            .trace = RepositoryArtifactTrace{"trace-1", 12, 2, 1, 1, 1}};
    }

    SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string&, const std::string&) override {
        return std::vector<RepositoryConflict>{{"readme.md", {"blob-1", "blob-2"}}};
    }

    SyncStoreResult<std::vector<std::pair<std::string, std::string>>>
    refs(const std::string&, const std::string&) override {
        return std::vector<std::pair<std::string, std::string>>{{"main", "change-1"}};
    }

    SyncStoreResult<std::vector<std::byte>>
    find_missing(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<std::byte>> push(const std::string&,
                                                 const std::string&,
                                                 std::span<const std::byte>,
                                                 SyncChangeAuthorizer authorize) override {
        const auto authorized = authorize("change-refused");
        if (!authorized) {
            return std::unexpected(authorized.error());
        }
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<std::byte>>
    pull(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<void>
    upload_chunks(const std::string&, const std::string&, std::span<const std::byte>) override {
        return {};
    }

    SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string&,
                                                       const std::string&,
                                                       const std::string&,
                                                       std::uint64_t,
                                                       std::uint64_t) override {
        return std::vector<std::byte>{};
    }
};

class TemporaryRepositoryRoot {
public:
    TemporaryRepositoryRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = std::filesystem::temp_directory_path() / ("lake-web-ui-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryRepositoryRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class TemporarySyncRoot {
public:
    TemporarySyncRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = std::filesystem::temp_directory_path() / ("lake-sync-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporarySyncRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(const std::string& value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::vector<std::byte> sync_bytes(std::string_view value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::vector<std::string> tree_fingerprint(const liquid::tree& tree) {
    std::vector<std::string> output;
    output.reserve(tree.entries.size());
    for (const auto& entry : tree.entries) {
        output.push_back(entry.name + ":" + liquid::to_string(entry.ref));
    }
    std::ranges::sort(output);
    return output;
}

bool send_socket_bytes(int socket_fd, std::string_view bytes) {
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

std::size_t content_length_of(std::string_view header) {
    constexpr std::string_view name = "Content-Length:";
    const std::size_t start = header.find(name);
    if (start == std::string_view::npos) {
        return 0;
    }
    const std::size_t value_start = start + name.size();
    const std::size_t value_end = header.find("\r\n", value_start);
    return static_cast<std::size_t>(
        std::stoull(std::string(header.substr(value_start, value_end - value_start))));
}

std::string read_http_request(int socket_fd) {
    std::string request;
    char buffer[4096];
    std::size_t expected = 0;
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return request;
        }
        request.append(buffer, static_cast<std::size_t>(count));
        const std::size_t header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos && expected == 0) {
            expected = header_end + 4 + content_length_of(request.substr(0, header_end + 4));
        }
        if (expected != 0 && request.size() >= expected) {
            return request;
        }
    }
}

class RecordingProxy {
public:
    explicit RecordingProxy(unsigned short upstream_port) : upstream_port_(upstream_port) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ < 0 ||
            ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 16) != 0) {
            return;
        }
        socklen_t size = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~RecordingProxy() {
        stop();
    }

    [[nodiscard]] unsigned short port() const {
        return port_;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (wake >= 0) {
            static_cast<void>(
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            ::close(wake);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ >= 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }

    [[nodiscard]] std::vector<std::byte> push_body() const {
        std::lock_guard lock(mutex_);
        return push_body_;
    }

private:
    void run() {
        while (!stopped_.load()) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            if (stopped_.load()) {
                ::close(client);
                break;
            }
            const std::string request = read_http_request(client);
            const std::size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos &&
                request.substr(0, header_end).find("/sync/push ") != std::string::npos) {
                const auto* first =
                    reinterpret_cast<const std::byte*>(request.data() + header_end + 4);
                std::lock_guard lock(mutex_);
                push_body_.assign(first, first + request.size() - header_end - 4);
            }
            const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(upstream_port_);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (upstream >= 0 &&
                ::connect(upstream, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                send_socket_bytes(upstream, request)) {
                std::string response;
                char buffer[4096];
                for (;;) {
                    const ssize_t count = ::recv(upstream, buffer, sizeof(buffer), 0);
                    if (count <= 0) {
                        break;
                    }
                    response.append(buffer, static_cast<std::size_t>(count));
                }
                static_cast<void>(send_socket_bytes(client, response));
            }
            if (upstream >= 0) {
                ::close(upstream);
            }
            ::close(client);
        }
    }

    unsigned short upstream_port_{};
    int listener_{-1};
    unsigned short port_{};
    std::atomic<bool> stopped_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::byte> push_body_;
};

liquid::platform host_platform() {
#if defined(__APPLE__)
    return {"macos", "arm64"};
#elif defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
}

std::string
http_get(unsigned short port, std::string_view path, std::string_view authorization = {}) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }

    std::string request =
        "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (!authorization.empty())
        request += "Authorization: " + std::string(authorization) + "\r\n";
    request += "\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string http_request(unsigned short port,
                         std::string_view method,
                         std::string_view path,
                         std::string_view body,
                         std::string_view content_type,
                         std::string_view cookie = {},
                         std::string_view authorization = {}) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    std::string request = std::string(method) + " " + std::string(path) +
                          " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    request += "Content-Type: " + std::string(content_type) + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!cookie.empty())
        request += "Cookie: " + std::string(cookie) + "\r\n";
    if (!authorization.empty())
        request += "Authorization: " + std::string(authorization) + "\r\n";
    request += "\r\n" + std::string(body);
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string response_header(std::string_view response, std::string_view name) {
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const std::size_t start = response.find(prefix);
    if (start == std::string_view::npos)
        return {};
    const std::size_t value_start = start + prefix.size();
    const std::size_t end = response.find("\r\n", value_start);
    return std::string(response.substr(value_start, end - value_start));
}

json response_json(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return json();
    }
    return json::parse(response.substr(separator + 4), nullptr, false);
}

} // namespace

TEST(Registry, ServerInfoNeedsNoAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("server_info");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value["name"], "lake");
    EXPECT_EQ(r.value["store"], "memory");
}

TEST(Telemetry, PrometheusCounterAndHistogram) {
    lake::telemetry::Metrics metrics;
    metrics.increment_counter("test_requests_total", {{"result", "ok"}});
    metrics.increment_counter("test_requests_total", {{"result", "ok"}}, 2.0);
    metrics.observe_histogram("test_duration_seconds", {{"operation", "read"}}, 0.012);

    const std::string text = metrics.prometheus_text();
    EXPECT_NE(text.find("# TYPE test_requests_total counter\n"), std::string::npos);
    EXPECT_NE(text.find("test_requests_total{result=\"ok\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE test_duration_seconds histogram\n"), std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"0.01\"} 0\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"0.025\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"+Inf\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_count{operation=\"read\"} 1\n"), std::string::npos);
}

TEST(Telemetry, StorePingIsMeasured) {
    lake::telemetry::Metrics metrics;
    auto store = lake::telemetry::instrument_store(std::make_shared<lake::control::InMemoryStore>(),
                                                   metrics);
    ASSERT_TRUE(store->ping());

    const std::string text = metrics.prometheus_text();
    EXPECT_NE(text.find("lake_store_calls_total{store=\"memory\",operation="
                        "\"ping\",result=\"ok\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("lake_store_call_duration_seconds_count{store=\"memory\","
                        "operation=\"ping\",result=\"ok\"} 1\n"),
              std::string::npos);
}

TEST(TelemetryHttp, HealthAndMemoryReadinessNeedNoAuth) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string health = http_get(server.port(), "/healthz");
    EXPECT_NE(health.find(" 200 "), std::string::npos);
    EXPECT_TRUE(health.ends_with("{\"ok\":true}"));

    const std::string ready = http_get(server.port(), "/readyz");
    EXPECT_NE(ready.find(" 200 "), std::string::npos);
    EXPECT_TRUE(ready.ends_with("{\"ok\":true,\"store\":\"memory\"}"));
    server.stop();
}

TEST(TelemetryHttp, MetricsRequireExistingAuthAndUsePrometheusContentType) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.admin_token = "test-token";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string denied = http_get(server.port(), "/metrics");
    EXPECT_NE(denied.find(" 403 "), std::string::npos);
    EXPECT_NE(denied.find("\"code\":\"unauthorized\""), std::string::npos);

    const std::string served = http_get(server.port(), "/metrics", "Bearer test-token");
    EXPECT_NE(served.find(" 200 "), std::string::npos);
    EXPECT_NE(served.find("Content-Type: text/plain; version=0.0.4"), std::string::npos);
    EXPECT_NE(served.find("lake_build_info{version=\"0.0.1\",node=\"test-node\"} 1\n"),
              std::string::npos);
    EXPECT_NE(served.find("lake_up 1\n"), std::string::npos);
    EXPECT_NE(served.find("lake_http_requests_total{method=\"GET\",route=\"/"
                          "metrics\",status=\"403\"} 1\n"),
              std::string::npos);
    server.stop();
}

TEST(IdentityHttp, PagesIssueAndResolveHttpOnlySessions) {
    ServerOptions options;
    options.port = 0;
    options.public_host = "lake.example";
    options.admin_token = "operator-page-token";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string page = http_get(server.port(), "/sign-up");
    EXPECT_NE(page.find(" 200 "), std::string::npos);
    EXPECT_NE(page.find("action=\"/identity/sign-up\""), std::string::npos);
    const std::string mfa_challenge = http_get(server.port(), "/mfa");
    EXPECT_NE(mfa_challenge.find(" 200 "), std::string::npos);
    EXPECT_NE(mfa_challenge.find("action=\"/identity/mfa/verify\""), std::string::npos);

    const std::string operator_page =
        http_get(server.port(), "/sign-in", "Bearer operator-page-token");
    EXPECT_NE(operator_page.find(" 302 "), std::string::npos);
    EXPECT_EQ(response_header(operator_page, "Location"), "/repositories");
    const std::string operator_sessions =
        http_get(server.port(), "/sessions", "Bearer operator-page-token");
    EXPECT_NE(operator_sessions.find(" 302 "), std::string::npos);
    EXPECT_EQ(response_header(operator_sessions, "Location"), "/repositories");

    const std::string created =
        http_request(server.port(),
                     "POST",
                     "/identity/sign-up",
                     "email=page%40example.test&display_name=Page+User&password=correct+horse+"
                     "battery+staple&device_name=browser&identity_document=%7B%7D",
                     "application/x-www-form-urlencoded");
    EXPECT_NE(created.find(" 302 "), std::string::npos) << created;
    EXPECT_EQ(response_header(created, "Location"), "/sessions");
    const std::string set_cookie = response_header(created, "Set-Cookie");
    EXPECT_NE(set_cookie.find("HttpOnly"), std::string::npos);
    const std::size_t cookie_end = set_cookie.find(';');
    const std::string cookie = set_cookie.substr(0, cookie_end);

    const std::string sessions =
        http_request(server.port(), "GET", "/sessions", "", "text/plain", cookie);
    EXPECT_NE(sessions.find(" 200 "), std::string::npos) << sessions;
    EXPECT_NE(sessions.find("Sessions and devices"), std::string::npos);
    EXPECT_NE(sessions.find("browser"), std::string::npos);

    const std::string mfa_enrollment = http_request(server.port(),
                                                    "POST",
                                                    "/identity/mfa/enroll",
                                                    "",
                                                    "application/x-www-form-urlencoded",
                                                    cookie);
    EXPECT_NE(mfa_enrollment.find(" 200 "), std::string::npos) << mfa_enrollment;
    EXPECT_NE(mfa_enrollment.find("otpauth://totp/lake:"), std::string::npos);
    EXPECT_NE(mfa_enrollment.find("Recovery codes"), std::string::npos);
    EXPECT_NE(mfa_enrollment.find("action=\"/identity/mfa/disable\""), std::string::npos);

    const std::string rest_signup = http_request(
        server.port(),
        "POST",
        "/v1/sign_up_password",
        R"({"email":"rest-page@example.test","password":"correct horse battery staple","display_name":"REST Page","device_name":"browser","identity_document":"{}"})",
        "application/json");
    EXPECT_NE(rest_signup.find(" 200 "), std::string::npos) << rest_signup;
    EXPECT_NE(response_header(rest_signup, "Set-Cookie").find("HttpOnly"), std::string::npos);
    server.stop();
}

TEST(ScimHttp, RecordedClientSessionProvisionsUsersGroupsAndRevokesAccess) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto organization = store->create_organization({"", "SCIM HTTP", ""});
    ASSERT_TRUE(organization.has_value());
    const std::string token = "recorded-scim-token";
    const auto provider = store->register_sso_provider({organization->id,
                                                        "lake",
                                                        "recorded-client",
                                                        "oidc",
                                                        "https://identity.example.test",
                                                        "",
                                                        "",
                                                        "recorded-client-id",
                                                        "recorded-client-secret",
                                                        token,
                                                        json::object(),
                                                        false});
    ASSERT_TRUE(provider.has_value());

    ServerOptions options;
    options.port = 0;
    options.store = store;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string denied = http_get(server.port(), "/scim/v2/Users");
    EXPECT_NE(denied.find(" 401 "), std::string::npos) << denied;
    EXPECT_NE(denied.find("urn:ietf:params:scim:api:messages:2.0:Error"), std::string::npos);

    const auto create_user = [&](std::string_view external_id,
                                 std::string_view email,
                                 std::string_view display_name,
                                 std::string_view taap_id) {
        const json body{
            {"schemas", json::array({"urn:ietf:params:scim:schemas:core:2.0:User"})},
            {"externalId", external_id},
            {"userName", email},
            {"displayName", display_name},
            {"active", true},
            {"urn:orama:params:scim:schemas:extension:taap:2.0:User", {{"taapId", taap_id}}}};
        return http_request(server.port(),
                            "POST",
                            "/scim/v2/Users",
                            body.dump(),
                            "application/scim+json",
                            "",
                            "Bearer " + token);
    };
    const std::string first_response =
        create_user("user-one", "one@example.test", "One", "taap:1:ed25519:user-one");
    EXPECT_NE(first_response.find(" 201 "), std::string::npos) << first_response;
    const json first = response_json(first_response);
    ASSERT_TRUE(first.is_object());
    const std::string first_id = first.at("id");
    const std::string second_response =
        create_user("user-two", "two@example.test", "Two", "taap:1:ed25519:user-two");
    EXPECT_NE(second_response.find(" 201 "), std::string::npos) << second_response;
    const json second = response_json(second_response);
    ASSERT_TRUE(second.is_object());
    const std::string second_id = second.at("id");

    const std::string filtered =
        http_get(server.port(),
                 "/scim/v2/Users?filter=userName%20eq%20%22one%40example.test%22",
                 "Bearer " + token);
    EXPECT_NE(filtered.find(" 200 "), std::string::npos) << filtered;
    const json filtered_body = response_json(filtered);
    ASSERT_TRUE(filtered_body.is_object());
    EXPECT_EQ(filtered_body.at("totalResults"), 1);
    EXPECT_EQ(filtered_body.at("Resources").at(0).at("id"), first_id);

    const json user_patch{
        {"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:PatchOp"})},
        {"Operations",
         json::array({{{"op", "replace"}, {"path", "displayName"}, {"value", "One Updated"}}})}};
    const std::string patched_user = http_request(server.port(),
                                                  "PATCH",
                                                  "/scim/v2/Users/" + first_id,
                                                  user_patch.dump(),
                                                  "application/scim+json",
                                                  "",
                                                  "Bearer " + token);
    EXPECT_NE(patched_user.find(" 200 "), std::string::npos) << patched_user;
    EXPECT_EQ(response_json(patched_user).at("displayName"), "One Updated");

    const json group_body{
        {"schemas", json::array({"urn:ietf:params:scim:schemas:core:2.0:Group"})},
        {"externalId", "engineering"},
        {"displayName", "Engineering"},
        {"members", json::array({{{"value", first_id}}, {{"value", second_id}}})}};
    const std::string group_response = http_request(server.port(),
                                                    "POST",
                                                    "/scim/v2/Groups",
                                                    group_body.dump(),
                                                    "application/scim+json",
                                                    "",
                                                    "Bearer " + token);
    EXPECT_NE(group_response.find(" 201 "), std::string::npos) << group_response;
    const json group = response_json(group_response);
    ASSERT_TRUE(group.is_object());
    const std::string group_id = group.at("id");
    auto memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_EQ(memberships->size(), 2);

    const json group_patch{
        {"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:PatchOp"})},
        {"Operations",
         json::array({{{"op", "remove"}, {"path", "members[value eq \"" + first_id + "\"]"}}})}};
    const std::string patched_group = http_request(server.port(),
                                                   "PATCH",
                                                   "/scim/v2/Groups/" + group_id,
                                                   group_patch.dump(),
                                                   "application/scim+json",
                                                   "",
                                                   "Bearer " + token);
    EXPECT_NE(patched_group.find(" 200 "), std::string::npos) << patched_group;
    memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_EQ(memberships->size(), 1);
    EXPECT_EQ(memberships->at(0).principal_id, second.at("id").get<std::string>());

    ASSERT_TRUE(
        store->create_federation_issuer({organization->id, provider->issuer, "email"}).has_value());
    const auto session = store->sign_in_sso({provider->id,
                                             provider->issuer,
                                             "user-two",
                                             "two@example.test",
                                             "Two",
                                             "",
                                             "recorded-client",
                                             {{"email", "two@example.test"}}});
    ASSERT_TRUE(session.has_value());
    const std::string deleted_user = http_request(server.port(),
                                                  "DELETE",
                                                  "/scim/v2/Users/" + second_id,
                                                  "",
                                                  "application/scim+json",
                                                  "",
                                                  "Bearer " + token);
    EXPECT_NE(deleted_user.find(" 204 "), std::string::npos) << deleted_user;
    EXPECT_FALSE(store->context_for(session->secret).has_value());
    memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_TRUE(memberships->empty());

    const std::string deleted_group = http_request(server.port(),
                                                   "DELETE",
                                                   "/scim/v2/Groups/" + group_id,
                                                   "",
                                                   "application/scim+json",
                                                   "",
                                                   "Bearer " + token);
    EXPECT_NE(deleted_group.find(" 204 "), std::string::npos) << deleted_group;
    EXPECT_FALSE(store->get_team(group_id).has_value());
    const auto events = store->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events.has_value());
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.action == "scim_delete_user" && event.draft.outcome == "success";
    }));
    server.stop();
}

TEST(Registry, EverythingElseNeedsAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("list_users");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.code, "unauthorized");
    EXPECT_EQ(rest_status(r), 403);
}

TEST(Registry, ValidatesInput) {
    Surface s;
    ApiResult r = s.call("create_customer", json::object());
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(r.error.path, "/customer_type");
    r = s.call("get_user", {{"id", "x"}, {"bogus", 1}});
    EXPECT_EQ(r.error.path, "/bogus");
    r = s.call("list_users", {{"limit", "ten"}});
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(s.call("no_such").error.code, "unknown_capability");
}

TEST(Audit, RegistryRecordsEachCapabilityOutcomeAndDetectsDeletedRows) {
    Surface s;
    for (const auto& capability : s.registry.list()) {
        s.call(capability->name);
    }
    auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), s.registry.list().size());
    for (const auto& capability : s.registry.list()) {
        const auto count =
            std::count_if(events->begin(), events->end(), [&](const lake::audit::Event& event) {
                return event.draft.action == capability->name;
            });
        EXPECT_EQ(count, 1) << capability->name;
    }
    EXPECT_TRUE(lake::audit::verify(s.store, *events));

    std::vector<lake::audit::Event> deleted = *events;
    deleted.erase(deleted.begin() + 1);
    const auto verification = lake::audit::verify(s.store, deleted);
    EXPECT_FALSE(verification);
    EXPECT_EQ(verification.error().code, "invalid_chain");
}

TEST(Audit, DeniedAuthorizationAndJsonLinesExportAreRecorded) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult denied = s.call("list_users");
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");

    s.ctx.authorized = true;
    const ApiResult exported = s.call("export_audit");
    ASSERT_TRUE(exported.ok) << exported.error.message;
    std::istringstream lines(exported.value["jsonl"].get<std::string>());
    std::string line;
    std::vector<json> decoded;
    while (std::getline(lines, line)) {
        if (!line.empty()) {
            decoded.push_back(json::parse(line));
        }
    }
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0]["outcome"], "denied");
    EXPECT_EQ(decoded[0]["action"], "list_users");

    auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 2u);
    EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(Audit, IdentityRotatesAndRetentionFloorIsEnforced) {
    Surface s;
    const ApiResult initial = s.call("identity");
    ASSERT_TRUE(initial.ok) << initial.error.message;
    const std::string initial_identity = initial.value["taap_id"];
    const ApiResult rotated = s.call("rotate_identity");
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_NE(rotated.value["taap_id"], initial_identity);

    lake::control::InMemoryStore externally_seeded_store;
    const lake::audit::IdentityOptions externally_seeded_options{
        .configured_seed = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        .instance_kek = "test-instance-kek",
    };
    auto externally_seeded =
        lake::audit::identity(externally_seeded_store, externally_seeded_options);
    ASSERT_TRUE(externally_seeded) << externally_seeded.error().message;
    auto externally_rotated =
        lake::audit::rotate_identity(externally_seeded_store, externally_seeded_options);
    ASSERT_TRUE(externally_rotated) << externally_rotated.error().message;
    lake::audit::EventDraft externally_seeded_event{
        .kind = "administrative",
        .principal = "admin",
        .object = "lake",
        .action = "verify_external_rotation",
        .outcome = "success",
        .request_id = lake::control::new_uuid_v4(),
    };
    auto recorded = lake::audit::record(
        externally_seeded_store, externally_seeded_event, externally_seeded_options);
    ASSERT_TRUE(recorded) << recorded.error().message;
    auto externally_seeded_events =
        externally_seeded_store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(externally_seeded_events);
    EXPECT_TRUE(lake::audit::verify(externally_seeded_store, *externally_seeded_events));

    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult accepted =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 500}});
    EXPECT_TRUE(accepted.ok);
    const ApiResult lowered =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 400}});
    EXPECT_FALSE(lowered.ok);
    EXPECT_EQ(lowered.error.code, "conflict");
    const ApiResult refused =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 399}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");

    auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(events);
    const auto retention =
        std::find_if(events->rbegin(), events->rend(), [](const lake::audit::Event& event) {
            return event.draft.kind == "retention" && event.draft.outcome == "failure";
        });
    ASSERT_NE(retention, events->rend());
    EXPECT_EQ(retention->draft.action, "set_audit_retention");
    EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(PostgresAudit, RegistryEventsPersistAndVerify) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(database_uri);
    const auto ready = store->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const auto before = store->list_audit_events(lake::audit::Filter{.limit = 1000000});
    ASSERT_TRUE(before) << before.error().message;
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    ApiContext context;
    context.store = store.get();
    context.node_name = "postgres-test";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    context.authorized = true;
    context.acting_identity = "admin";

    for (const auto& capability : registry.list()) {
        context.request_id.clear();
        registry.call(capability->name, json::object(), context);
    }
    context.authorized = false;
    context.request_id.clear();
    const ApiResult result = registry.call("list_users", json::object(), context);
    EXPECT_EQ(result.error.code, "unauthorized");

    auto events = store->list_audit_events(lake::audit::Filter{.limit = 1000000});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_EQ(events->size(), before->size() + registry.list().size() + 1);
    EXPECT_EQ(events->back().draft.outcome, "denied");
    const auto verification = lake::audit::verify(*store, *events);
    EXPECT_TRUE(verification) << verification.error().message;
}

TEST(Customers, IndividualLifecycleWithGeneratedHandle) {
    Surface s;
    const ApiResult user =
        s.call("create_user", {{"email", "Ada@Example.com"}, {"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const std::string user_id = user.value["id"];
    EXPECT_EQ(user_id.size(), 36u);

    const ApiResult customer =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string name = customer.value["name"];
    EXPECT_TRUE(lake::control::valid_handle(name)) << name;
    EXPECT_EQ(customer.value["user_id"], user_id);
    EXPECT_TRUE(customer.value["organization_id"].is_null());

    const ApiResult again =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    EXPECT_EQ(again.error.code, "conflict"); // one customer per user
    EXPECT_EQ(rest_status(again), 409);

    const ApiResult dup_email = s.call("create_user", {{"email", "ada@example.com"}});
    EXPECT_EQ(dup_email.error.code, "conflict"); // case-insensitive email

    const ApiResult listed = s.call("list_customers");
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["items"].size(), 1u);
    const ApiResult got = s.call("get_customer", {{"id", customer.value["id"]}});
    ASSERT_TRUE(got.ok);
    EXPECT_EQ(got.value["name"], name);
    EXPECT_EQ(s.call("get_customer", {{"id", "missing"}}).error.code, "not_found");
}
TEST(Customers, OrganizationAndTheTenantRules) {
    Surface s;
    const ApiResult org = s.call("create_organization",
                                 {{"display_name", "Orama"}, {"taap_id", "taap:1:ed25519:abc"}});
    ASSERT_TRUE(org.ok);
    const std::string org_id = org.value["id"];
    EXPECT_EQ(s.call("create_organization", {{"taap_id", "taap:1:ed25519:abc"}}).error.code,
              "conflict");

    // xor: an organization customer must not carry a user_id; type must match the
    // id given.
    ApiResult bad = s.call(
        "create_customer",
        {{"customer_type", "organization"}, {"organization_id", org_id}, {"user_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad = s.call("create_customer", {{"customer_type", "individual"}, {"organization_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad =
        s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", "nope"}});
    EXPECT_EQ(bad.error.code, "invalid_input"); // FK

    // Handle format and the single global namespace.
    bad = s.call(
        "create_customer",
        {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "Orama Inc"}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    const ApiResult ok =
        s.call("create_customer",
               {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "orama"}});
    ASSERT_TRUE(ok.ok) << ok.error.message;
    const ApiResult user = s.call("create_user", {{"display_name", "Bob"}});
    const ApiResult taken =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "orama"}});
    EXPECT_EQ(taken.error.code, "conflict");
}

TEST(Rest, PathsMethodsAndQueryCoercion) {
    Surface s;
    const auto list = s.registry.find("list_users");
    const auto create = s.registry.find("create_user");
    EXPECT_EQ(rest_path(*list), "/v1/list_users");
    EXPECT_EQ(rest_method(*list), "GET");
    EXPECT_EQ(rest_method(*create), "POST");
    const json input = rest_input_from_query(*list, {{"limit", "5"}, {"other", "x"}});
    EXPECT_EQ(input["limit"], 5);
    EXPECT_EQ(input["other"], "x");
    const json envelope = rest_envelope(ApiResult::failure("not_found", "gone", "/id"));
    EXPECT_FALSE(envelope["ok"]);
    EXPECT_EQ(envelope["error"]["code"], "not_found");
    EXPECT_EQ(rest_status(ApiResult::failure("mfa_required", "verify MFA")), 403);
    EXPECT_EQ(rest_status(ApiResult::failure("store_unavailable", "")), 503);
}

TEST(Rest, OpenApiCoversEveryCapability) {
    Surface s;
    const json doc = openapi_document(s.registry, "0.0.1");
    EXPECT_EQ(doc["openapi"], "3.1.0");
    for (const auto& capability : s.registry.list()) {
        if (capability->transport == CapabilityTransport::LiquidSync) {
            continue;
        }
        const std::string method = rest_method(*capability) == "GET" ? "get" : "post";
        ASSERT_TRUE(doc["paths"].contains(rest_path(*capability))) << capability->name;
        EXPECT_TRUE(doc["paths"][rest_path(*capability)].contains(method)) << capability->name;
    }
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/push"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/pull"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/find-missing"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/chunks"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/chunks/{digest}"));
    EXPECT_TRUE(doc["paths"].contains("/mcp"));
    EXPECT_TRUE(doc["paths"]["/v1/server_info"]["get"]["security"].empty());
}

TEST(SyncRegistry, DeclaresTransportPolicyAndRecordsDenial) {
    Surface surface;
    const auto push = surface.registry.find("push");
    const auto pull = surface.registry.find("pull");
    const auto refs = surface.registry.find("refs");
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    ASSERT_NE(refs, nullptr);
    EXPECT_EQ(push->transport, CapabilityTransport::LiquidSync);
    EXPECT_EQ(push->action, "write");
    EXPECT_EQ(pull->transport, CapabilityTransport::LiquidSync);
    EXPECT_EQ(pull->action, "read");
    EXPECT_EQ(refs->transport, CapabilityTransport::Json);

    surface.ctx.authorized = false;
    const ApiResult denied = surface.call("push", {{"repository_id", "missing"}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    const auto events = surface.store.list_audit_events({.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_FALSE(events->empty());
    EXPECT_EQ(events->back().draft.action, "push");
    EXPECT_EQ(events->back().draft.outcome, "denied");
}

TEST(SyncRegistry, RevokedOfflineGrantRefusesPublicationWithChangeAndReason) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    const auto issued = surface.store.sign_up_password({"lake",
                                                        "offline-writer@example.test",
                                                        "correct horse battery staple",
                                                        "Offline writer",
                                                        "offline device",
                                                        "{}"});
    ASSERT_TRUE(issued) << issued.error().message;
    const auto principal = surface.store.context_for(issued->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto customer =
        surface.store.create_customer({"individual", principal->user_id, "", "offline-writer"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        surface.store.create_repository({customer->id, "offline-sync", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto object = surface.store.find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(surface.store.create_grant({principal->taap_id,
                                            object->id,
                                            "write",
                                            "offline-sync-attestation",
                                            "admin",
                                            "offline-sync"}));
    surface.ctx.authorized = true;
    surface.ctx.operator_credential = false;
    surface.ctx.session_credential = true;
    surface.ctx.acting_user_id = principal->user_id;
    surface.ctx.acting_identity = principal->taap_id;
    surface.ctx.credential_device_id = principal->device_id;
    surface.ctx.scopes = principal->scopes;
    const std::string scope = "repository:" + repository->id + ":read";
    const ApiResult grant = surface.call("issue_offline_grant",
                                         {{"device_id", principal->device_id},
                                          {"scopes", json::array({scope})},
                                          {"duration_seconds", 3600}});
    ASSERT_TRUE(grant.ok) << grant.error.message;
    ASSERT_TRUE(surface.call("revoke_offline_grant", {{"id", grant.value["id"]}}).ok);

    surface.ctx.sync_store = &repository_store;
    surface.ctx.sync_wire_request = true;
    surface.ctx.offline_grant_id = grant.value["id"];
    surface.ctx.reauthorization_results = json::array();
    const ApiResult pushed = surface.call("push", {{"repository_id", repository->id}});
    ASSERT_FALSE(pushed.ok);
    EXPECT_EQ(pushed.error.code, "unauthorized");
    ASSERT_EQ(surface.ctx.reauthorization_results.size(), 1U);
    EXPECT_EQ(surface.ctx.reauthorization_results[0]["change"], "change-refused");
    EXPECT_FALSE(surface.ctx.reauthorization_results[0]["allowed"]);
    EXPECT_EQ(surface.ctx.reauthorization_results[0]["reason"],
              "offline grant was revoked before publication");
    const auto events = surface.store.list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push_change" && event.draft.outcome == "denied" &&
               event.draft.details.value("change", "") == "change-refused" &&
               event.draft.details.value("reason", "") ==
                   "offline grant was revoked before publication";
    }));
}

TEST(Mcp, InitializeListAndCall) {
    Surface s;
    const auto init = mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, s.ctx, "0.0.1");
    ASSERT_TRUE(init);
    EXPECT_EQ(json::parse(*init)["result"]["serverInfo"]["name"], "lake");
    EXPECT_FALSE(mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, s.ctx, "0.0.1"));

    const auto tools = json::parse(*mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, s.ctx, "0.0.1"));
    const auto json_capabilities = std::ranges::count_if(s.registry.list(), [](const auto& entry) {
        return entry->transport == CapabilityTransport::Json;
    });
    EXPECT_EQ(tools["result"]["tools"].size(), json_capabilities);

    const json call = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "create_user"}, {"arguments", {{"display_name", "Eve"}}}}}};
    const auto reply = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_FALSE(reply["result"]["isError"]);
    EXPECT_EQ(reply["result"]["structuredContent"]["value"]["display_name"], "Eve");

    s.ctx.authorized = false;
    const auto denied = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_TRUE(denied["result"]["isError"]);
    EXPECT_EQ(denied["result"]["structuredContent"]["error"]["code"], "unauthorized");
    const auto unknown = json::parse(*mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "nope"}}, s.ctx, "0.0.1"));
    EXPECT_EQ(unknown["error"]["code"], -32601);
}

TEST(Repositories, OwnedByACustomerNamedWithinIt) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const std::string cid = customer.value["id"];

    ApiResult repo =
        s.call("create_repository",
               {{"customer_id", cid}, {"name", "liquid"}, {"description", "the tool"}});
    ASSERT_TRUE(repo.ok) << repo.error.message;
    EXPECT_EQ(repo.value["visibility"], "private");
    EXPECT_EQ(repo.value["customer_id"], cid);

    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "liquid"}}).error.code,
              "conflict");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "Bad Name"}}).error.code,
              "invalid_input");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", "nope"}, {"name", "x"}}).error.code,
              "invalid_input");
    EXPECT_EQ(
        s.call("create_repository", {{"customer_id", cid}, {"name", "y"}, {"visibility", "secret"}})
            .error.code,
        "invalid_input");
    ASSERT_TRUE(s.call("create_repository",
                       {{"customer_id", cid}, {"name", "lake"}, {"visibility", "public"}})
                    .ok);

    const ApiResult all = s.call("list_repositories");
    ASSERT_TRUE(all.ok);
    EXPECT_EQ(all.value["items"].size(), 2u);
    EXPECT_EQ(all.value["items"][0]["name"], "lake"); // newest first
    const ApiResult mine = s.call("list_repositories", {{"customer_id", cid}, {"limit", 1}});
    EXPECT_EQ(mine.value["items"].size(), 1u);
    const ApiResult none = s.call("list_repositories", {{"customer_id", "other"}});
    EXPECT_EQ(none.value["items"].size(), 0u);
    EXPECT_EQ(s.call("get_repository", {{"id", repo.value["id"]}}).value["name"], "liquid");
    EXPECT_EQ(s.call("get_repository", {{"id", "missing"}}).error.code, "not_found");
}

TEST(Forge, PersistsIssueLifecycleAndMapsReviewCommentAnchors) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "forge"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];

    const ApiResult issue = surface.call(
        "create_issue",
        {{"repository_id", repository_id},
         {"title", "Document anchors"},
         {"body", "Discuss `src/lib.cpp`."},
         {"labels", json::array({"docs"})},
         {"assignees", json::array({"taap:1:ed25519:ada"})},
         {"references", json::array({{"change", "change-1"}, {"path", "src/lib.cpp"}})}});
    ASSERT_TRUE(issue.ok) << issue.error.message;
    EXPECT_EQ(issue.value["value"]["state"], "open");
    EXPECT_TRUE(surface
                    .call("update_issue",
                          {{"repository_id", repository_id},
                           {"issue_id", issue.value["id"]},
                           {"state", "closed"}})
                    .ok);
    EXPECT_TRUE(surface
                    .call("create_issue_comment",
                          {{"repository_id", repository_id},
                           {"issue_id", issue.value["id"]},
                           {"body", "Resolved by change-2."}})
                    .ok);

    const ApiResult review = surface.call("create_review",
                                          {{"repository_id", repository_id},
                                           {"change_id", "change-1"},
                                           {"reviewers", json::array({"taap:1:ed25519:ada"})}});
    ASSERT_TRUE(review.ok) << review.error.message;
    const ApiResult comment = surface.call(
        "create_review_comment",
        {{"repository_id", repository_id},
         {"review_id", review.value["id"]},
         {"body", "Use a more specific name."},
         {"anchor",
          {{"change", "change-1"}, {"path", "src/lib.cpp"}, {"line", 8}, {"column", 4}}}});
    ASSERT_TRUE(comment.ok) << comment.error.message;

    const ApiResult mapped = surface.call(
        "map_review_anchors",
        {{"repository_id", repository_id},
         {"source_change", "change-1"},
         {"target_change", "change-2"},
         {"path", "src/lib.cpp"},
         {"hunks",
          json::array({{{"old_line", 1}, {"old_count", 0}, {"new_line", 1}, {"new_count", 2}}})}});
    ASSERT_TRUE(mapped.ok) << mapped.error.message;
    ASSERT_EQ(mapped.value["items"].size(), 1);
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["change"], "change-2");
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["line"], 10);
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["disposition"], "shifted");
}

TEST(Forge, ReleaseRetainsVerifiedArtifactAttestations) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "releases"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;

    const ApiResult release = surface.call("create_release",
                                           {{"repository_id", repository.value["id"]},
                                            {"tag", "v1.0.0"},
                                            {"change_id", "change-1"},
                                            {"artifact_ids", json::array({"result-1"})}});
    ASSERT_TRUE(release.ok) << release.error.message;
    EXPECT_EQ(release.value["value"]["artifacts"][0]["id"], "result-1");
    EXPECT_EQ(release.value["value"]["artifacts"][0]["attestations"][0]["id"], "attestation-1");
}

TEST(Forge, WebhookDeliveryRetriesAndRetainsAnEventLog) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "hooks"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    const ApiResult webhook = surface.call("create_webhook",
                                           {{"repository_id", repository_id},
                                            {"url", "https://example.test/lake"},
                                            {"events", json::array({"issue"})},
                                            {"secret", "delivery-secret"}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;

    int calls = 0;
    surface.ctx.webhook_sender = [&calls](const std::string&,
                                          const json& headers,
                                          const std::string&) -> WebhookDeliveryResult {
        EXPECT_TRUE(headers.at("x-lake-signature-256").get<std::string>().starts_with("sha256="));
        ++calls;
        if (calls == 1) {
            return {.status = 503, .error = "service unavailable"};
        }
        return {.status = 204};
    };
    ASSERT_TRUE(
        surface.call("create_issue", {{"repository_id", repository_id}, {"title", "Trigger hook"}})
            .ok);

    const ApiResult listed =
        surface.call("list_webhook_deliveries", {{"repository_id", repository_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1);
    EXPECT_FALSE(listed.value["items"][0]["value"]["delivered"]);
    const ApiResult retried = surface.call(
        "retry_webhook_delivery",
        {{"repository_id", repository_id}, {"delivery_id", listed.value["items"][0]["id"]}});
    ASSERT_TRUE(retried.ok) << retried.error.message;
    EXPECT_TRUE(retried.value["value"]["delivered"]);
    EXPECT_EQ(retried.value["value"]["attempt"], 2);
    EXPECT_EQ(calls, 2);
}

TEST(Forge, BranchProtectionRequiresAnApprovedReview) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "protected"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    ASSERT_TRUE(surface
                    .call("set_branch_protection",
                          {{"repository_id", repository_id},
                           {"ref", "refs/heads/main"},
                           {"required_review", true},
                           {"allowed_pushers", json::array({"admin"})}})
                    .ok);

    const BranchRefUpdate update{
        .name = "refs/heads/main", .new_head = "change-1", .fast_forward = true};
    const auto rejected = evaluate_branch_protection(
        surface.store, "admin", surface.ctx.authorization_cache, repository_id, update);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "conflict");

    const ApiResult review = surface.call(
        "create_review", {{"repository_id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(review.ok) << review.error.message;
    ASSERT_TRUE(surface
                    .call("update_review",
                          {{"repository_id", repository_id},
                           {"review_id", review.value["id"]},
                           {"state", "approved"}})
                    .ok);
    EXPECT_TRUE(evaluate_branch_protection(
        surface.store, "admin", surface.ctx.authorization_cache, repository_id, update));
}

TEST(Repositories, AnonymousCallersOnlySeePublicRepositories) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult private_repository =
        s.call("create_repository",
               {{"customer_id", customer.value["id"]}, {"name", "private-repository"}});
    const ApiResult public_repository = s.call("create_repository",
                                               {{"customer_id", customer.value["id"]},
                                                {"name", "public-repository"},
                                                {"visibility", "public"}});
    ASSERT_TRUE(private_repository.ok);
    ASSERT_TRUE(public_repository.ok);

    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    const ApiResult listed = s.call("list_repositories");
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1u);
    EXPECT_EQ(listed.value["items"][0]["id"], public_repository.value["id"]);
    EXPECT_TRUE(s.call("get_repository", {{"id", public_repository.value["id"]}}).ok);
    const ApiResult hidden = s.call("get_repository", {{"id", private_repository.value["id"]}});
    EXPECT_EQ(hidden.error.code, "unauthorized");

    for (const auto& capability : s.registry.list()) {
        if (capability->name.starts_with("list_")) {
            EXPECT_TRUE(capability->authorizes_candidates) << capability->name;
        }
    }
}

TEST(Repositories, PublicRepositoryContentCapabilitiesUseTheRepositoryStore) {
    Surface s;
    RepositoryFixtureStore repository_store;
    s.ctx.sync_store = &repository_store;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = s.call("create_repository",
                                        {{"customer_id", customer.value["id"]},
                                         {"name", "public-repository"},
                                         {"visibility", "public"}});
    ASSERT_TRUE(repository.ok);
    const std::string repository_id = repository.value["id"];

    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    EXPECT_EQ(s.call("repository_refs", {{"id", repository_id}}).value["items"][0]["name"], "main");
    EXPECT_EQ(s.call("repository_tree", {{"id", repository_id}, {"ref", "main"}})
                  .value["items"][0]["path"],
              "readme.md");
    EXPECT_EQ(
        s.call("repository_file", {{"id", repository_id}, {"ref", "main"}, {"path", "readme.md"}})
            .value["contents"],
        "hello");
    EXPECT_EQ(s.call("list_changes", {{"id", repository_id}}).value["items"].size(), 1u);
    EXPECT_EQ(
        s.call("get_change", {{"id", repository_id}, {"change_id", "change-1"}}).value["message"],
        "initial");
    const ApiResult difference =
        s.call("change_diff", {{"id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(difference.ok) << difference.error.message;
    EXPECT_EQ(difference.value["items"][0]["path"], "readme.md");
    EXPECT_EQ(difference.value["items"][0]["line_hunks"][0]["added_lines"][0], "hello");
    EXPECT_EQ(s.call("list_conflicts", {{"id", repository_id}}).value["items"][0]["blobs"].size(),
              2u);
    const ApiResult artifacts = s.call("list_artifacts", {{"id", repository_id}, {"limit", 1}});
    ASSERT_TRUE(artifacts.ok) << artifacts.error.message;
    EXPECT_EQ(artifacts.value["items"][0]["id"], "result-1");
    EXPECT_TRUE(artifacts.value["done"]);
    const ApiResult artifact =
        s.call("get_artifact", {{"id", repository_id}, {"result_id", "result-1"}});
    ASSERT_TRUE(artifact.ok) << artifact.error.message;
    EXPECT_EQ(artifact.value["outputs"][0]["path"], "build/app");
    EXPECT_EQ(artifact.value["attestations"][0]["profile"], "slsa_provenance/1");
    EXPECT_EQ(artifact.value["trace"]["id"], "trace-1");
}

TEST(WebUiHttp, PublicRepositoriesAreReadableAndPrivateRepositoriesAreHidden) {
    TemporaryRepositoryRoot root;
    const auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"", "", "Ada"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer);
    const auto public_repository =
        store->create_repository({customer->id, "public-repository", "visible", "public"});
    const auto private_repository =
        store->create_repository({customer->id, "private-repository", "hidden", "private"});
    const auto reformatting_repository =
        store->create_repository({customer->id, "reformatting-repository", "empty diff", "public"});
    ASSERT_TRUE(public_repository);
    ASSERT_TRUE(private_repository);
    ASSERT_TRUE(reformatting_repository);

    const std::filesystem::path repository_path =
        root.path() / customer->id / "repositories" / public_repository->id;
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(root.path() / customer->id / "store"));
    ASSERT_TRUE(customer_store) << customer_store.error().message;
    liquid::repository_options repository_options;
    repository_options.store = &*customer_store;
    auto opened =
        liquid::runtime::block_on(liquid::repository::create(repository_path, repository_options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("readme.md", bytes("public content"))));
    const auto first_change = liquid::runtime::block_on(repository.cut("initial public content"));
    ASSERT_TRUE(first_change) << first_change.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("readme.md", bytes("public content\nupdated\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.md", bytes("new file\n"))));
    const auto second_change = liquid::runtime::block_on(repository.cut("update public content"));
    ASSERT_TRUE(second_change) << second_change.error().message;
    const std::vector<std::byte> artifact_bytes = bytes("artifact output");
    const auto artifact_content = liquid::hash(liquid::algorithm::blake3, artifact_bytes);
    ASSERT_TRUE(artifact_content) << artifact_content.error().message;
    const liquid::blob remote_artifact{
        .chunks = {*artifact_content},
        .size = artifact_bytes.size(),
        .chunk_sizes = {artifact_bytes.size()},
        .content = *artifact_content,
        .remote = liquid::remote_blob_location{"test", "worker-1"},
    };
    const auto artifact_record =
        liquid::runtime::block_on(repository.store_remote_blob(remote_artifact));
    ASSERT_TRUE(artifact_record) << artifact_record.error().message;
    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_FALSE(history->empty());
    const liquid::action_input build_input{.command = {"remote-build"},
                                           .outputs = {"build/app"},
                                           .inputs = history->front().root,
                                           .toolchain = history->front().root,
                                           .platform = host_platform()};
    auto first_result_value = liquid::action_result{
        .outputs = {{"build/app", 0100755, *artifact_record}},
        .stdout_output = *artifact_record,
        .stderr_output = *artifact_record,
        .timing = {.queued = std::chrono::milliseconds{1},
                   .sandbox = std::chrono::milliseconds{2},
                   .exec = std::chrono::milliseconds{3},
                   .hash = std::chrono::milliseconds{4},
                   .store = std::chrono::milliseconds{5}},
        .worker = "worker-1",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{100}},
        .cache = liquid::cache_status::hit,
    };
    first_result_value.started_at = std::chrono::sys_seconds{std::chrono::seconds{98}};
    first_result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{100}};
    const auto first_result = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, build_input, first_result_value));
    ASSERT_TRUE(first_result) << first_result.error().message;
    auto second_result_value = first_result_value;
    second_result_value.observed_at = std::chrono::sys_seconds{std::chrono::seconds{200}};
    second_result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{200}};
    const auto second_result = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, build_input, second_result_value));
    ASSERT_TRUE(second_result) << second_result.error().message;

    const std::filesystem::path reformatting_path =
        root.path() / customer->id / "repositories" / reformatting_repository->id;
    auto reformatting_opened = liquid::runtime::block_on(
        liquid::repository::create(reformatting_path, repository_options));
    ASSERT_TRUE(reformatting_opened) << reformatting_opened.error().message;
    auto reformatting_source = std::move(*reformatting_opened);
    ASSERT_TRUE(
        liquid::runtime::block_on(reformatting_source.write("format.cpp", bytes("int value;\n"))));
    const auto formatted_before =
        liquid::runtime::block_on(reformatting_source.cut("before formatting"));
    ASSERT_TRUE(formatted_before) << formatted_before.error().message;
    const auto formatting_change =
        liquid::runtime::block_on(reformatting_source.cut("reformatting"));
    ASSERT_TRUE(formatting_change) << formatting_change.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.admin_token = "test-token";
    options.store = store;
    options.store_root = root.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string listed = http_get(server.port(), "/repositories");
    EXPECT_NE(listed.find(" 200 "), std::string::npos);
    EXPECT_NE(listed.find("public-repository"), std::string::npos);
    EXPECT_EQ(listed.find("private-repository"), std::string::npos);

    const std::string public_page =
        http_get(server.port(), "/repository?id=" + public_repository->id);
    EXPECT_NE(public_page.find(" 200 "), std::string::npos);
    EXPECT_NE(public_page.find("public-repository"), std::string::npos);
    const std::string tree_page =
        http_get(server.port(), "/repository/tree?id=" + public_repository->id + "&ref=main");
    EXPECT_NE(tree_page.find(" 200 "), std::string::npos);
    EXPECT_NE(tree_page.find("readme.md"), std::string::npos);
    const std::string file_page = http_get(
        server.port(), "/file?repository_id=" + public_repository->id + "&ref=main&path=readme.md");
    EXPECT_NE(file_page.find(" 200 "), std::string::npos);
    EXPECT_NE(file_page.find("public content"), std::string::npos);
    const std::string change_page = http_get(server.port(),
                                             "/change?repository_id=" + public_repository->id +
                                                 "&id=" + liquid::to_string(second_change->value));
    EXPECT_NE(change_page.find(" 200 "), std::string::npos);
    EXPECT_NE(change_page.find("readme.md"), std::string::npos);
    EXPECT_NE(change_page.find("notes.md"), std::string::npos);
    EXPECT_NE(change_page.find("@@ -2,0 +2,1 @@"), std::string::npos);
    EXPECT_NE(change_page.find("+updated"), std::string::npos);
    const std::string reformatting_page =
        http_get(server.port(),
                 "/change?repository_id=" + reformatting_repository->id +
                     "&id=" + liquid::to_string(formatting_change->value));
    EXPECT_NE(reformatting_page.find(" 200 "), std::string::npos);
    EXPECT_NE(reformatting_page.find("No changes to display."), std::string::npos);
    EXPECT_EQ(reformatting_page.find("<code>format.cpp</code>"), std::string::npos);
    const std::string artifacts_page =
        http_get(server.port(), "/artifacts?repository_id=" + public_repository->id + "&limit=1");
    EXPECT_NE(artifacts_page.find(" 200 "), std::string::npos);
    EXPECT_NE(artifacts_page.find("Build results"), std::string::npos);
    EXPECT_NE(artifacts_page.find("Next page"), std::string::npos);
    const std::string artifact_page =
        http_get(server.port(),
                 "/artifact?repository_id=" + public_repository->id +
                     "&id=" + liquid::to_string(second_result->value));
    EXPECT_NE(artifact_page.find(" 200 "), std::string::npos);
    EXPECT_NE(artifact_page.find("build/app"), std::string::npos);
    EXPECT_NE(artifact_page.find("slsa_provenance/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("sbom/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("verified"), std::string::npos);

    const std::string private_page =
        http_get(server.port(), "/repository?id=" + private_repository->id);
    EXPECT_NE(private_page.find(" 302 "), std::string::npos);
    const std::string private_change =
        http_get(server.port(), "/change?repository_id=" + private_repository->id + "&id=hidden");
    EXPECT_NE(private_change.find(" 302 "), std::string::npos);
    const std::string private_artifacts =
        http_get(server.port(), "/artifacts?repository_id=" + private_repository->id);
    EXPECT_NE(private_artifacts.find(" 302 "), std::string::npos);
    EXPECT_NE(private_page.find("Location: /login"), std::string::npos);
    const std::string private_api =
        http_get(server.port(), "/v1/get_repository?id=" + private_repository->id);
    EXPECT_NE(private_api.find(" 403 "), std::string::npos);
    server.stop();
}

TEST(Repositories, LifecycleUsesPolicyAndAuditRegistry) {
    Surface surface;
    const ApiResult first_user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult second_user = surface.call("create_user", {{"display_name", "Grace"}});
    ASSERT_TRUE(first_user.ok);
    ASSERT_TRUE(second_user.ok);
    const ApiResult first_customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", first_user.value["id"]}, {"name", "ada"}});
    const ApiResult second_customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", second_user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(first_customer.ok);
    ASSERT_TRUE(second_customer.ok);
    const ApiResult created = surface.call(
        "create_repository", {{"customer_id", first_customer.value["id"]}, {"name", "liquid"}});
    ASSERT_TRUE(created.ok);
    const std::string repository_id = created.value["id"];

    ASSERT_TRUE(surface
                    .call("set_repository_visibility",
                          {{"id", repository_id}, {"visibility", "organization"}})
                    .ok);
    ASSERT_TRUE(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "liquid-next"}}).ok);
    const ApiResult transferred =
        surface.call("transfer_repository",
                     {{"id", repository_id}, {"customer_id", second_customer.value["id"]}});
    ASSERT_TRUE(transferred.ok);
    EXPECT_EQ(transferred.value["customer_id"], second_customer.value["id"]);
    const ApiResult deleted = surface.call("delete_repository", {{"id", repository_id}});
    ASSERT_TRUE(deleted.ok);
    EXPECT_FALSE(deleted.value["deleted_at"].is_null());
    EXPECT_FALSE(deleted.value["retention_until"].is_null());
    EXPECT_EQ(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "again"}}).error.code,
        "conflict");

    const ApiResult events = surface.call("audit", {{"limit", 100}});
    ASSERT_TRUE(events.ok);
    EXPECT_TRUE(std::ranges::any_of(events.value["items"], [](const json& event) {
        return event["action"] == "delete_repository" && event["outcome"] == "success";
    }));

    surface.ctx.acting_identity = "guest";
    EXPECT_EQ(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "denied"}}).error.code,
        "unauthorized");
}

TEST(SyncHttp, PushPullShareCustomerStoreAndTransferOnlyMissingChunks) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:sync-owner", "sync-owner@example.test", "Sync Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "sync-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "project", "sync fixture", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto bad_repository =
        control->create_repository({customer->id, "bad-signature", "", "private"});
    ASSERT_TRUE(bad_repository) << bad_repository.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "source"));
    ASSERT_TRUE(source) << source.error().message;
    const std::string first_contents(128 * 1024, 'a');
    ASSERT_TRUE(liquid::runtime::block_on(source->write("first.txt", sync_bytes(first_contents))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "sync-test";
    options.admin_token = "sync-admin";
    options.instance_kek = "sync-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string direct_url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    RecordingProxy proxy(server.port());
    ASSERT_NE(proxy.port(), 0);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(proxy.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(url,
                                   liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto first_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    EXPECT_GT(first_push->sent.bytes, 0U);

    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("second.txt", sync_bytes("second version"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("second")));
    const auto second_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second_push) << second_push.error().message;
    EXPECT_GT(second_push->sent.bytes, 0U);
    EXPECT_LT(second_push->sent.bytes, first_push->sent.bytes);
    const auto unchanged_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(unchanged_push) << unchanged_push.error().message;
    EXPECT_EQ(unchanged_push->sent.bytes, 0U);
    proxy.stop();

    auto corrupt_push = proxy.push_body();
    ASSERT_FALSE(corrupt_push.empty());
    auto page = std::vector<liquid::sync_item>(256);
    const auto exported =
        liquid::runtime::block_on(source->export_drips(liquid::journal_cursor{}, page));
    ASSERT_TRUE(exported) << exported.error().message;
    const auto signed_drip = std::ranges::find_if(
        page.begin(), page.begin() + exported->count, [](const liquid::sync_item& item) {
            return item.kind == liquid::sync_item_kind::drip && !item.signature.empty();
        });
    ASSERT_NE(signed_drip, page.begin() + exported->count);
    const auto signature = std::search(corrupt_push.begin(),
                                       corrupt_push.end(),
                                       signed_drip->signature.begin(),
                                       signed_drip->signature.end());
    ASSERT_NE(signature, corrupt_push.end());
    *signature ^= std::byte{1};
    const std::string corrupt_body(reinterpret_cast<const char*>(corrupt_push.data()),
                                   corrupt_push.size());
    const std::string rejected =
        http_request(server.port(),
                     "POST",
                     "/v1/repositories/" + bad_repository->id + "/sync/push",
                     corrupt_body,
                     "application/vnd.liquid.sync-v1",
                     {},
                     "Bearer sync-admin");
    EXPECT_NE(rejected.find(" 400 "), std::string::npos) << rejected;

    auto destination =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    liquid::sync_endpoint pull_endpoint(
        direct_url, liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto pulled = liquid::runtime::block_on(pull_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));
    const auto source_first = liquid::runtime::block_on(source->read("first.txt"));
    const auto destination_first = liquid::runtime::block_on(destination->read("first.txt"));
    ASSERT_TRUE(source_first) << source_first.error().message;
    ASSERT_TRUE(destination_first) << destination_first.error().message;
    EXPECT_EQ(*destination_first, *source_first);
    const auto source_second = liquid::runtime::block_on(source->read("second.txt"));
    const auto destination_second = liquid::runtime::block_on(destination->read("second.txt"));
    ASSERT_TRUE(source_second) << source_second.error().message;
    ASSERT_TRUE(destination_second) << destination_second.error().message;
    EXPECT_EQ(*destination_second, *source_second);
    auto rejected_destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "rejected-destination"));
    ASSERT_TRUE(rejected_destination) << rejected_destination.error().message;
    const std::string rejected_url = "http://127.0.0.1:" + std::to_string(server.port()) +
                                     "/v1/repositories/" + bad_repository->id;
    liquid::sync_endpoint rejected_endpoint(
        rejected_url, liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto rejected_pull =
        liquid::runtime::block_on(rejected_endpoint.pull(*rejected_destination));
    ASSERT_TRUE(rejected_pull) << rejected_pull.error().message;
    const auto rejected_tree = liquid::runtime::block_on(rejected_destination->root());
    ASSERT_TRUE(rejected_tree) << rejected_tree.error().message;
    EXPECT_TRUE(rejected_tree->entries.empty());
    EXPECT_FALSE(liquid::runtime::block_on(rejected_destination->head()));
    server.stop();

    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "store"));
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "store" /
                                        "repositories"));
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "repositories" /
                                         repository->id / ".liquid-store"));
    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_FALSE(references->empty());
    const auto events = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push_change" && event.draft.outcome == "success";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push" && event.draft.outcome == "failure";
    }));
}

TEST(SyncHttp, PushWithoutWriteAccessIsRejectedAndAudited) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = control->create_user(
        {"taap:1:ed25519:sync-owner-denial", "owner-denial@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        control->create_customer({"individual", owner->id, "", "sync-denial-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "private-project", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto reader = control->sign_up_password(
        {"lake", "reader@example.test", "correct horse battery staple", "Reader", "test", "{}"});
    ASSERT_TRUE(reader) << reader.error().message;
    const auto principal = control->context_for(reader->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto object = control->find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    const auto grant = control->create_grant({principal->taap_id,
                                              object->id,
                                              "read",
                                              "attestation:sync-reader",
                                              owner->taap_id,
                                              "sync-denial"});
    ASSERT_TRUE(grant) << grant.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "denied-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("private.txt", sync_bytes("not authorized"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("denied")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "sync-denial";
    options.instance_kek = "sync-denial-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(url,
                                   liquid::sync_endpoint_options{.bearer_token = reader->secret});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    EXPECT_FALSE(pushed);
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code, liquid::errc::unauthorized);
    server.stop();

    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_TRUE(references->empty());
    const auto events = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.outcome == "denied" &&
               (event.draft.action == "upload_chunks" || event.draft.action == "push");
    }));
}

TEST(PostgresSync, PushAndPullUsePostgresControlPlane) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    std::shared_ptr<lake::control::Store> control =
        lake::control::make_postgres_store(database_uri);
    const auto ready = control->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const std::string suffix = lake::control::new_uuid_v4();
    const auto user = control->create_user({"taap:1:ed25519:postgres-sync-" + suffix,
                                            "postgres-sync-" + suffix + "@example.test",
                                            "Postgres Sync"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        control->create_customer({"individual", user->id, "", "postgres-sync-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "project", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "postgres-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        source->write("postgres.txt", sync_bytes("postgres-backed sync"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("postgres")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "postgres-sync";
    options.admin_token = "postgres-sync-admin";
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = "postgres-sync-admin"});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "postgres-destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    server.stop();

    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));
    const auto contents = liquid::runtime::block_on(destination->read("postgres.txt"));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ(*contents, sync_bytes("postgres-backed sync"));

    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_FALSE(references->empty());
    const auto events = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push" && event.draft.outcome == "success";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "pull" && event.draft.outcome == "success";
    }));
}

TEST(Identity, PasswordSessionsTokensAndRevocation) {
    Surface s;
    const ApiResult sign_up = s.call("sign_up_password",
                                     {{"email", "ada@example.com"},
                                      {"password", "correct horse battery"},
                                      {"identity_document", "{}"}});
    ASSERT_TRUE(sign_up.ok) << sign_up.error.message;
    const std::string browser_secret = sign_up.value["secret"];
    const std::string user_id = sign_up.value["session"]["user_id"];

    auto principal = s.store.context_for(browser_secret);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->user_id, user_id);

    const ApiResult provider = s.call("register_oauth_provider",
                                      {{"provider", "stub"},
                                       {"issuer", "https://stub.example.test"},
                                       {"client_id", "lake-test"},
                                       {"client_secret", "test-client-secret"},
                                       {"scopes", json::array({"openid", "email"})}});
    ASSERT_TRUE(provider.ok) << provider.error.message;
    ApiContext user_context;
    user_context.store = &s.store;
    user_context.authorized = true;
    user_context.acting_user_id = principal->user_id;
    user_context.acting_identity = principal->taap_id;
    user_context.instance_kek = s.ctx.instance_kek;
    user_context.session_credential = true;
    user_context.scopes = principal->scopes;

    const ApiResult enrollment = s.registry.call("enroll_mfa", json::object(), user_context);
    ASSERT_TRUE(enrollment.ok) << enrollment.error.message;
    EXPECT_TRUE(
        enrollment.value["otpauth_uri"].get<std::string>().starts_with("otpauth://totp/lake:"));
    ASSERT_EQ(enrollment.value["recovery_codes"].size(), 8);
    EXPECT_FALSE(enrollment.value.contains("secret"));
    const std::string first_recovery = enrollment.value["recovery_codes"][0];
    const std::string second_recovery = enrollment.value["recovery_codes"][1];

    const json sign_in_input = {{"email", "ada@example.com"},
                                {"password", "correct horse battery"}};
    const ApiResult mfa_required = s.call("sign_in_password", sign_in_input);
    EXPECT_EQ(mfa_required.error.code, "mfa_required");
    const ApiResult verified = s.call("verify_mfa",
                                      {{"email", "ada@example.com"},
                                       {"password", "correct horse battery"},
                                       {"code", first_recovery}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_TRUE(s.store.context_for(verified.value["secret"]));
    const ApiResult reused = s.call("verify_mfa",
                                    {{"email", "ada@example.com"},
                                     {"password", "correct horse battery"},
                                     {"code", first_recovery}});
    EXPECT_FALSE(reused.ok);

    const ApiResult stale_disable =
        s.registry.call("disable_mfa", {{"code", first_recovery}}, user_context);
    EXPECT_EQ(stale_disable.error.code, "unauthorized");
    const ApiResult disabled =
        s.registry.call("disable_mfa", {{"code", second_recovery}}, user_context);
    ASSERT_TRUE(disabled.ok) << disabled.error.message;
    EXPECT_TRUE(s.call("sign_in_password", sign_in_input).ok);

    const ApiResult token =
        s.registry.call("create_token",
                        {{"name", "read sessions"}, {"scopes", json::array({"list_sessions"})}},
                        user_context);
    ASSERT_TRUE(token.ok) << token.error.message;
    EXPECT_FALSE(token.value["session"]["expires_at"].get<std::string>().empty());

    auto token_principal = s.store.context_for(token.value["secret"]);
    ASSERT_TRUE(token_principal);
    ApiContext token_context;
    token_context.store = &s.store;
    token_context.authorized = true;
    token_context.acting_user_id = token_principal->user_id;
    token_context.acting_identity = token_principal->taap_id;
    token_context.instance_kek = s.ctx.instance_kek;
    token_context.session_credential = true;
    token_context.scopes = token_principal->scopes;
    EXPECT_TRUE(s.registry.call("list_sessions", json::object(), token_context).ok);
    EXPECT_EQ(
        s.registry.call("create_token", {{"name", "no"}, {"scopes", json::array()}}, token_context)
            .error.code,
        "unauthorized");

    const std::string session_id = sign_up.value["session"]["id"];
    ASSERT_TRUE(s.store.revoke_session(user_id, session_id));
    EXPECT_FALSE(s.store.context_for(browser_secret));
    const ApiResult static_without_reason = s.registry.call(
        "create_token",
        {{"name", "legacy"}, {"scopes", json::array({"list_sessions"})}, {"static", true}},
        user_context);
    EXPECT_EQ(static_without_reason.error.code, "invalid_input");
    const ApiResult static_token = s.registry.call("create_token",
                                                   {{"name", "legacy"},
                                                    {"scopes", json::array({"list_sessions"})},
                                                    {"static", true},
                                                    {"reason", "migration"}},
                                                   user_context);
    ASSERT_TRUE(static_token.ok) << static_token.error.message;
    EXPECT_TRUE(static_token.value["session"]["expires_at"].is_null());
    auto audit_events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(audit_events);
    EXPECT_TRUE(std::ranges::any_of(*audit_events, [](const lake::audit::Event& event) {
        return event.draft.action == "create_token" && event.draft.outcome == "success";
    }));
}
