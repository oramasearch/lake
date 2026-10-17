// The tunnel client downloads (lake#745, docs/api/tunnel-clients.md): the clients directory,
// tunnel_client_downloads' signed short-lived links, and GET /tunnel-clients/<platform>.
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/api/tunnel_clients.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "temporary_directory.hpp"

namespace {

using nlohmann::json;
using namespace lake::api;

constexpr std::string_view kek = "6c616b652d74756e6e656c2d636c69656e74732d746573742d696e7374616e63";
const std::string darwin_bytes = std::string("\xcf\xfa\xed\xfe", 4) + "darwin client";
const std::string linux_bytes = std::string("\x7f" "ELF", 4) + "linux client, a little longer";

void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A directory laid out as the zetta-tunnel-clients image's /clients/ is.
std::filesystem::path clients_directory(const std::filesystem::path& root) {
    const auto directory = root / "tunnel-clients";
    std::filesystem::create_directories(directory);
    write(directory / "zetta-tunnel-darwin-arm64", darwin_bytes);
    write(directory / "zetta-tunnel-linux-amd64", linux_bytes);
    write(directory / "VERSION", "0.0.1\n");
    write(directory / "SHA256SUMS",
          lake::control::identity_crypto::sha256_hex(darwin_bytes) +
              "  zetta-tunnel-darwin-arm64\n" +
              lake::control::identity_crypto::sha256_hex(linux_bytes) +
              " *zetta-tunnel-linux-amd64\n");
    return directory;
}

// A folder of this test process's own, removed afterwards.
class Scratch {
public:
    Scratch() : path_(lake::test::fixture_path("tunnel-clients-" + std::to_string(++count_))) {
        std::filesystem::create_directories(path_);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const std::filesystem::path& path() const {
        return path_;
    }

private:
    static inline int count_ = 0;
    std::filesystem::path path_;
};

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

TEST(TunnelClients, ReadsTheVersionSumsAndSizesOfTheAllowListedFiles) {
    Scratch root;
    const auto directory = clients_directory(root.path());
    write(directory / "zetta-tunnel-windows-amd64.exe", "not offered");
    const auto clients = read_tunnel_clients(directory);
    ASSERT_TRUE(clients);
    EXPECT_EQ(clients->version, "0.0.1");
    ASSERT_EQ(clients->clients.size(), 2U);
    EXPECT_EQ(clients->clients[0].platform, "darwin-arm64");
    EXPECT_EQ(clients->clients[0].sha256, lake::control::identity_crypto::sha256_hex(darwin_bytes));
    EXPECT_EQ(clients->clients[0].size, darwin_bytes.size());
    EXPECT_EQ(clients->clients[1].platform, "linux-amd64");
    EXPECT_EQ(clients->clients[1].size, linux_bytes.size());
}

TEST(TunnelClients, AMissingDirectoryVersionOrSumIsUnavailable) {
    Scratch root;
    EXPECT_FALSE(read_tunnel_clients({}));
    EXPECT_FALSE(read_tunnel_clients(root.path() / "absent"));
    const auto directory = clients_directory(root.path());
    // A file without a line in SHA256SUMS is not offered.
    write(directory / "SHA256SUMS",
          lake::control::identity_crypto::sha256_hex(linux_bytes) + "  zetta-tunnel-linux-amd64\n");
    const auto one = read_tunnel_clients(directory);
    ASSERT_TRUE(one);
    ASSERT_EQ(one->clients.size(), 1U);
    EXPECT_EQ(one->clients[0].platform, "linux-amd64");
    std::filesystem::remove(directory / "VERSION");
    EXPECT_FALSE(read_tunnel_clients(directory));
}

TEST(TunnelClients, OnlyTheAllowListedPlatformsAreKnown) {
    EXPECT_NE(find_tunnel_client_platform("darwin-arm64"), nullptr);
    EXPECT_NE(find_tunnel_client_platform("linux-amd64"), nullptr);
    for (const std::string_view name :
         {"", "windows-amd64", "../VERSION", "zetta-tunnel-linux-amd64", "linux-amd64/", "SHA256SUMS"}) {
        EXPECT_EQ(find_tunnel_client_platform(name), nullptr) << name;
    }
}

TEST(TunnelClients, ALinkIsBoundToItsPlatformVersionExpiryAndKey) {
    const std::int64_t now = 1'800'000'000;
    const std::int64_t expires = now + tunnel_client_link_seconds;
    const std::string signature = tunnel_client_signature(kek, "linux-amd64", "0.0.1", expires);
    ASSERT_FALSE(signature.empty());
    const std::string at = std::to_string(expires);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", at, signature, now),
              TunnelClientLinkCheck::valid);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", at, signature, expires + 1),
              TunnelClientLinkCheck::expired);
    EXPECT_EQ(check_tunnel_client_link(kek, "darwin-arm64", "0.0.1", at, signature, now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.2", at, signature, now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", std::to_string(expires + 60),
                                       signature, now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(check_tunnel_client_link("another key", "linux-amd64", "0.0.1", at, signature, now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(check_tunnel_client_link({}, "linux-amd64", "0.0.1", at, signature, now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", "soon", signature, now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", at, "", now),
              TunnelClientLinkCheck::invalid);
    EXPECT_EQ(tunnel_client_link("linux-amd64", expires, signature),
              "/tunnel-clients/linux-amd64?expires=" + at + "&signature=" + signature);
    // The Hub's upload hrefs and these links share the mechanism, never a payload.
    EXPECT_TRUE(signed_link_matches(kek, tunnel_client_link_payload("linux-amd64", "0.0.1", expires),
                                    signature));
    EXPECT_TRUE(signed_link_signature({}, "anything").empty());
}

namespace {

struct Downloads {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext ctx;
    Downloads() {
        register_core_capabilities(registry, "0.0.1");
        ctx.store = &store;
        ctx.instance_kek = std::string(kek);
        ctx.external_origin = "https://lake.example.test";
        const auto user = store.create_user({"taap:1:ed25519:developer", "dev@example.test", "Dev"});
        EXPECT_TRUE(user) << user.error().message;
        ctx.authorized = true;
        ctx.acting_identity = "taap:1:ed25519:developer";
    }
    ApiResult call() {
        ctx.request_id.clear();
        return registry.call("tunnel_client_downloads", json::object(), ctx);
    }
};

} // namespace

TEST(TunnelClientDownloads, AnswersSignedLinksThatWorkForFifteenMinutes) {
    Scratch root;
    Downloads s;
    s.ctx.tunnel_clients_dir = clients_directory(root.path()).string();
    const std::int64_t before = unix_now();
    const ApiResult result = s.call();
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value["available"], true);
    EXPECT_EQ(result.value["version"], "0.0.1");
    ASSERT_EQ(result.value["clients"].size(), 2U);
    const json& client = result.value["clients"][1];
    EXPECT_EQ(client["platform"], "linux-amd64");
    EXPECT_EQ(client["os"], "linux");
    EXPECT_EQ(client["arch"], "amd64");
    EXPECT_EQ(client["file_name"], "zetta-tunnel-linux-amd64");
    EXPECT_EQ(client["sha256"], lake::control::identity_crypto::sha256_hex(linux_bytes));
    EXPECT_EQ(client["size"], linux_bytes.size());
    const std::string url = client["download_url"];
    const std::string prefix = "https://lake.example.test/tunnel-clients/linux-amd64?expires=";
    ASSERT_TRUE(url.starts_with(prefix)) << url;
    const auto amp = url.find("&signature=");
    ASSERT_NE(amp, std::string::npos);
    const std::string expires = url.substr(prefix.size(), amp - prefix.size());
    const std::int64_t at = std::stoll(expires);
    EXPECT_GE(at, before + tunnel_client_link_seconds);
    EXPECT_LE(at, unix_now() + tunnel_client_link_seconds);
    const std::string signature = url.substr(amp + std::string("&signature=").size());
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", expires, signature, unix_now()),
              TunnelClientLinkCheck::valid);
    EXPECT_EQ(check_tunnel_client_link(kek, "linux-amd64", "0.0.1", expires, signature, at + 1),
              TunnelClientLinkCheck::expired);
    EXPECT_TRUE(std::string(result.value["expires_at"]).ends_with("Z"));
    // The schema REST, OpenAPI and MCP publish is the one the answer meets.
    EXPECT_TRUE(s.registry.find("tunnel_client_downloads"));
}

TEST(TunnelClientDownloads, WithoutClientsOrAKeyTheyAreUnavailable) {
    Scratch root;
    Downloads s;
    s.ctx.tunnel_clients_dir = (root.path() / "absent").string();
    const ApiResult missing = s.call();
    ASSERT_TRUE(missing.ok) << missing.error.message;
    EXPECT_EQ(missing.value,
              json({{"available", false},
                    {"version", nullptr},
                    {"expires_at", nullptr},
                    {"clients", json::array()}}));
    // A Lake with no instance key cannot sign a link, so it offers none.
    Downloads keyless_lake;
    keyless_lake.ctx.instance_kek.clear();
    keyless_lake.ctx.tunnel_clients_dir = clients_directory(root.path()).string();
    const ApiResult keyless = keyless_lake.call();
    ASSERT_TRUE(keyless.ok) << keyless.error.message;
    EXPECT_EQ(keyless.value["available"], false);
}

TEST(TunnelClientDownloads, OnlySignedInPeopleGetLinks) {
    Scratch root;
    Downloads s;
    s.ctx.tunnel_clients_dir = clients_directory(root.path()).string();
    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    const ApiResult anonymous = s.call();
    ASSERT_FALSE(anonymous.ok);
    EXPECT_EQ(anonymous.error.code, "unauthenticated");
}

namespace {

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers; // names lowercased
    std::string body;
};

HttpResponse get(unsigned short port, std::string_view path, std::string_view bearer = {}) {
    HttpResponse parsed;
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return parsed;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return parsed;
    }
    std::string wire = "GET " + std::string(path) + " HTTP/1.1\r\n";
    wire += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    wire += "Accept: */*\r\nConnection: close\r\n";
    if (!bearer.empty()) {
        wire += "Authorization: Bearer " + std::string(bearer) + "\r\n";
    }
    wire += "\r\n";
    for (std::size_t sent = 0; sent < wire.size();) {
        const ssize_t count = ::send(socket_fd, wire.data() + sent, wire.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return parsed;
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || response.size() < 12) {
        return parsed;
    }
    parsed.status = std::stoi(response.substr(9, 3));
    std::size_t line = response.find("\r\n") + 2;
    while (line < header_end) {
        const std::size_t end = response.find("\r\n", line);
        const std::size_t colon = response.find(':', line);
        if (colon < end) {
            std::string name = response.substr(line, colon - line);
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            parsed.headers[name] = response.substr(colon + 2, end - colon - 2);
        }
        line = end + 2;
    }
    parsed.body = response.substr(header_end + 4);
    return parsed;
}

std::string path_of(std::string_view url) {
    const std::size_t scheme = url.find("://");
    const std::size_t path = url.find('/', scheme == std::string_view::npos ? 0 : scheme + 3);
    return path == std::string_view::npos ? "/" : std::string(url.substr(path));
}

json error_of(const HttpResponse& response) {
    return json::parse(response.body, nullptr, false)["error"];
}

} // namespace

TEST(TunnelClientRoute, ServesTheSignedFileAndRefusesAnythingElse) {
    Scratch root;
    ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.instance_kek = std::string(kek);
    options.admin_token = "tunnel-admin";
    options.tunnel_clients_dir = clients_directory(root.path()).string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    // The capability over REST, then its link followed with no credential at all, as curl does.
    const HttpResponse listed = get(server.port(), "/v1/tunnel_client_downloads", "tunnel-admin");
    ASSERT_EQ(listed.status, 200) << listed.body;
    const json value = json::parse(listed.body)["value"];
    ASSERT_EQ(value["clients"].size(), 2U);
    const std::string url = value["clients"][0]["download_url"];
    EXPECT_TRUE(url.starts_with("http://127.0.0.1:" + std::to_string(server.port()) + "/")) << url;
    HttpResponse fetched = get(server.port(), path_of(url));
    ASSERT_EQ(fetched.status, 200) << fetched.body;
    EXPECT_EQ(fetched.body, darwin_bytes);
    EXPECT_EQ(fetched.headers["content-type"], "application/octet-stream");
    EXPECT_EQ(fetched.headers["content-length"], std::to_string(darwin_bytes.size()));
    EXPECT_EQ(fetched.headers["content-disposition"],
              "attachment; filename=\"zetta-tunnel-darwin-arm64\"");
    EXPECT_EQ(fetched.headers["cache-control"], "no-store");
    EXPECT_EQ(fetched.headers["etag"],
              "\"" + lake::control::identity_crypto::sha256_hex(darwin_bytes) + "\"");

    // An expired link, a tampered one, and one for another platform's signature.
    const std::int64_t past = unix_now() - 1;
    const HttpResponse expired = get(
        server.port(),
        tunnel_client_link("linux-amd64", past,
                           tunnel_client_signature(kek, "linux-amd64", "0.0.1", past)));
    EXPECT_EQ(expired.status, 401) << expired.body;
    EXPECT_EQ(error_of(expired)["code"], "unauthenticated");
    EXPECT_EQ(error_of(expired)["reason"], "expired");
    const std::int64_t later = unix_now() + 600;
    const std::string linux_signature = tunnel_client_signature(kek, "linux-amd64", "0.0.1", later);
    const HttpResponse swapped =
        get(server.port(), tunnel_client_link("darwin-arm64", later, linux_signature));
    EXPECT_EQ(swapped.status, 401) << swapped.body;
    EXPECT_EQ(error_of(swapped)["reason"], "invalid");
    const HttpResponse unsigned_link = get(server.port(), "/tunnel-clients/linux-amd64");
    EXPECT_EQ(unsigned_link.status, 401) << unsigned_link.body;
    const HttpResponse good =
        get(server.port(), tunnel_client_link("linux-amd64", later, linux_signature));
    ASSERT_EQ(good.status, 200) << good.body;
    EXPECT_EQ(good.body, linux_bytes);

    // Unknown platforms, and attempts to name another file, are not found before anything is read.
    for (const std::string path : {"/tunnel-clients/windows-amd64",
                                   "/tunnel-clients/VERSION",
                                   "/tunnel-clients/..%2fVERSION",
                                   "/tunnel-clients/zetta-tunnel-linux-amd64"}) {
        const HttpResponse refused = get(server.port(), path + "?expires=1&signature=x");
        EXPECT_EQ(refused.status, 404) << path << " " << refused.body;
        EXPECT_EQ(error_of(refused)["code"], "not_found") << path;
    }
    EXPECT_NE(get(server.port(), "/tunnel-clients/../VERSION").body, "0.0.1\n");
    server.stop();
}

TEST(TunnelClientRoute, ALakeWithoutClientsSaysSo) {
    Scratch root;
    ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.instance_kek = std::string(kek);
    options.admin_token = "tunnel-admin";
    options.tunnel_clients_dir = (root.path() / "absent").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const HttpResponse listed = get(server.port(), "/v1/tunnel_client_downloads", "tunnel-admin");
    ASSERT_EQ(listed.status, 200) << listed.body;
    EXPECT_EQ(json::parse(listed.body)["value"]["available"], false);
    const std::int64_t later = unix_now() + 600;
    const HttpResponse refused = get(
        server.port(),
        tunnel_client_link("linux-amd64", later,
                           tunnel_client_signature(kek, "linux-amd64", "0.0.1", later)));
    EXPECT_EQ(refused.status, 404) << refused.body;
    EXPECT_EQ(error_of(refused)["code"], "not_found");
    // Without a credential the capability is refused.
    EXPECT_EQ(get(server.port(), "/v1/tunnel_client_downloads").status, 401);
    server.stop();
}
