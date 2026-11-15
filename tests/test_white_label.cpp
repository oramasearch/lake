// A white-labelled Lake (lake#1136, docs/api/clerk.md): the host aliases and
// control host the operator gives it, the redirect of pages on an alias to the customer's host, and
// sign-in through the customer's own identity provider only.
#include <gtest/gtest.h>

#include <array>
#include <cctype>
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
#include "lake/api/white_label.hpp"
#include "lake/control/memory_store.hpp"

namespace {

using nlohmann::json;
using namespace lake::api;

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers; // names lowercased
    std::string body;
};

Response send(unsigned short port,
              std::string_view method,
              std::string_view target,
              std::string_view host,
              const std::map<std::string, std::string>& headers = {},
              std::string_view body = {}) {
    Response parsed;
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
    std::string wire = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\n";
    wire += "Host: " + std::string(host) + "\r\nConnection: close\r\n";
    for (const auto& [name, value] : headers) {
        wire += name + ": " + value + "\r\n";
    }
    if (!body.empty() || method == "POST") {
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    wire += "\r\n" + std::string(body);
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

} // namespace

TEST(WhiteLabelEnvironment, NothingSetIsTheUsualLake) {
    const auto hosts = instance_hosts_from_environment("eu.lake.example.com", "", "");
    ASSERT_TRUE(hosts) << hosts.error();
    EXPECT_TRUE(hosts->aliases.empty());
    EXPECT_EQ(hosts->control_host, "eu.lake.example.com");
    const auto none = instance_hosts_from_environment("", "", "");
    ASSERT_TRUE(none) << none.error();
    EXPECT_EQ(none->control_host, "");
}

TEST(WhiteLabelEnvironment, ReadsTheOperatorsValues) {
    const auto hosts = instance_hosts_from_environment(
        "data.example.com", " Acme.Lake.Example.Com , acme.lake.example.com,,old.example.com.", "acme.lake.example.com");
    ASSERT_TRUE(hosts) << hosts.error();
    EXPECT_EQ(hosts->public_host, "data.example.com");
    EXPECT_EQ(hosts->aliases, (std::vector<std::string>{"acme.lake.example.com", "old.example.com"}));
    EXPECT_EQ(hosts->control_host, "acme.lake.example.com");
    EXPECT_TRUE(is_host_alias(hosts->aliases, "ACME.lake.example.com:443"));
    EXPECT_FALSE(is_host_alias(hosts->aliases, "data.example.com"));
    EXPECT_FALSE(is_host_alias(hosts->aliases, "evil-acme.lake.example.com"));
}

TEST(WhiteLabelEnvironment, InvalidValuesAreStartupErrorsNamingTheVariable) {
    const auto bad_alias = instance_hosts_from_environment("data.example.com", "not a host", "");
    ASSERT_FALSE(bad_alias);
    EXPECT_NE(bad_alias.error().find("LAKE_HOST_ALIASES"), std::string::npos);
    const auto itself = instance_hosts_from_environment("data.example.com", "DATA.example.com", "");
    ASSERT_FALSE(itself);
    EXPECT_NE(itself.error().find("LAKE_PUBLIC_HOST itself"), std::string::npos);
    const auto alone = instance_hosts_from_environment("", "acme.lake.example.com", "");
    ASSERT_FALSE(alone);
    EXPECT_NE(alone.error().find("need LAKE_PUBLIC_HOST"), std::string::npos);
    const auto control = instance_hosts_from_environment("data.example.com", "", "-bad-");
    ASSERT_FALSE(control);
    EXPECT_NE(control.error().find("LAKE_CONTROL_HOST"), std::string::npos);
}

TEST(WhiteLabelEnvironment, SsoSignInIsNeverCombinedWithClerk) {
    EXPECT_EQ(sso_sign_in_from_environment("", true).value_or("x"), "");
    EXPECT_EQ(sso_sign_in_from_environment(" sso_AbC-1 ", false).value_or(""), "sso_AbC-1");
    const auto both = sso_sign_in_from_environment("sso_1", true);
    ASSERT_FALSE(both);
    EXPECT_NE(both.error().find("LAKE_CLERK_PUBLISHABLE_KEY"), std::string::npos);
    EXPECT_FALSE(sso_sign_in_from_environment("sso/1", false));
}

TEST(WhiteLabelRedirect, KeepsTheSameOriginPathAndQueryOnly) {
    EXPECT_EQ(alias_redirect_location("data.example.com", "/w/abc/design?tab=runs"),
              "https://data.example.com/w/abc/design?tab=runs");
    EXPECT_EQ(alias_redirect_location("data.example.com", "//evil.example/x"),
              "https://data.example.com/");
    EXPECT_EQ(alias_redirect_location("data.example.com", "/\\evil.example"),
              "https://data.example.com/");
    EXPECT_EQ(alias_redirect_location("data.example.com", ""), "https://data.example.com/");
}

TEST(WhiteLabelSsoOnly, RefusesLakesOwnMethodsInItsOwnPoolOnly) {
    const json lake_pool = {{"email", "a@example.com"}};
    const json app_pool = {{"pool_id", "app_123"}};
    for (const std::string_view method : {"sign_up_password",
                                          "sign_in_password",
                                          "verify_mfa",
                                          "sign_in_taap",
                                          "begin_passkey_sign_in",
                                          "sign_in_passkey",
                                          "start_oauth",
                                          "begin_email_recovery"}) {
        EXPECT_TRUE(sso_only_refusal("sso_1", method, lake_pool, "")) << method;
        EXPECT_TRUE(sso_only_refusal("sso_1", method, json{{"pool_id", "lake"}}, "")) << method;
        EXPECT_FALSE(sso_only_refusal("sso_1", method, app_pool, "")) << method;
        EXPECT_FALSE(sso_only_refusal("", method, lake_pool, "")) << method;
    }
    // Enterprise SSO: the configured provider only, among Lake's own pool's providers.
    for (const std::string_view start : {"start_sso_oidc", "start_sso_saml", "sign_in_saml"}) {
        EXPECT_FALSE(sso_only_refusal("sso_1", start, json{{"provider_id", "sso_1"}}, "lake"));
        EXPECT_TRUE(sso_only_refusal("sso_1", start, json{{"provider_id", "sso_2"}}, "lake"));
        EXPECT_FALSE(sso_only_refusal("sso_1", start, json{{"provider_id", "sso_3"}}, "app_123"));
    }
    EXPECT_FALSE(sso_only_refusal("sso_1", "server_info", json::object(), ""));
}

TEST(WhiteLabelSsoOnly, TheRegistryRefusesAndTheViewerNamesTheProvider) {
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    lake::control::InMemoryStore store;
    ApiContext ctx;
    ctx.store = &store;
    ctx.sign_in_sso_provider = "sso_customer";
    const ApiResult refused = registry.call(
        "sign_in_password",
        json{{"email", "a@example.com"}, {"password", "correct horse battery"}, {"device_name", "x"}},
        ctx);
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "unauthorized");
    EXPECT_NE(refused.error.message.find("identity provider only"), std::string::npos);
    const ApiResult sign_up = registry.call(
        "sign_up_password",
        json{{"email", "b@example.com"}, {"password", "correct horse battery"}, {"device_name", "x"}},
        ctx);
    ASSERT_FALSE(sign_up.ok);
    EXPECT_EQ(sign_up.error.code, "unauthorized");

    ApiContext viewer_ctx;
    viewer_ctx.store = &store;
    viewer_ctx.sign_in_sso_provider = "sso_customer";
    const ApiResult viewer = registry.call("viewer", json::object(), viewer_ctx);
    ASSERT_TRUE(viewer.ok) << viewer.error.message;
    EXPECT_EQ(viewer.value["password_sign_up"], false);
    ASSERT_TRUE(viewer.value["sign_in_sso"].is_object());
    EXPECT_EQ(viewer.value["sign_in_sso"]["provider_id"], "sso_customer");

    // Without it, nothing changes: password sign-up is offered and no provider is named.
    ApiContext usual;
    usual.store = &store;
    const ApiResult plain = registry.call("viewer", json::object(), usual);
    ASSERT_TRUE(plain.ok) << plain.error.message;
    EXPECT_EQ(plain.value["password_sign_up"], true);
    EXPECT_TRUE(plain.value["sign_in_sso"].is_null());
}

TEST(WhiteLabelAliases, PagesOnAnAliasGoToThePublicHostAndTheApiIsServed) {
    ServerOptions options;
    options.port = 0;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.public_host = "data.example.com";
    options.host_aliases = {"acme.lake.example.com"};
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const unsigned short port = server.port();

    // A browser page on the Orama host: a temporary redirect to the same path on the customer's.
    for (const auto& [target, location] :
         std::array<std::pair<std::string_view, std::string_view>, 3>{{
             {"/", "https://data.example.com/"},
             {"/sign-in?next=%2Fdocs", "https://data.example.com/sign-in?next=%2Fdocs"},
             {"/w/abc/design?tab=runs", "https://data.example.com/w/abc/design?tab=runs"},
         }}) {
        Response page = send(port, "GET", target, "acme.lake.example.com:443", {{"Accept", "text/html"}});
        EXPECT_EQ(page.status, 302) << target << " " << page.body;
        EXPECT_EQ(page.headers["location"], location) << target;
        EXPECT_EQ(page.headers["cache-control"], "no-store") << target;
    }
    // The API, MCP's discovery and health are served on the alias as on the public host.
    Response info = send(port, "GET", "/v1/server_info", "acme.lake.example.com");
    ASSERT_EQ(info.status, 200) << info.body;
    EXPECT_EQ(json::parse(info.body)["value"]["name"], "lake");
    EXPECT_EQ(send(port, "GET", "/healthz", "acme.lake.example.com").status, 200);
    // A browser opening the API's own pages is not redirected either: they are server routes.
    EXPECT_NE(send(port, "GET", "/v1/server_info", "acme.lake.example.com", {{"Accept", "text/html"}}).status,
              302);
    // On the public host nothing is redirected.
    EXPECT_NE(send(port, "GET", "/", "data.example.com", {{"Accept", "text/html"}}).status, 302);
    // Another host is not an alias: not redirected to the customer's host.
    Response other = send(port, "GET", "/", "unrelated.example.org", {{"Accept", "text/html"}});
    EXPECT_NE(other.headers["location"], "https://data.example.com/");
    server.stop();
}
