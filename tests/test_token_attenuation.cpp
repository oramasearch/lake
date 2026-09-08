#include "personal_customer_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liquid/identity.hpp>
#include <liquid/runtime.hpp>
#include <openssl/evp.h>
#include <pqxx/pqxx>

#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "temporary_directory.hpp"

namespace {
using nlohmann::json;
constexpr std::string_view instance_kek =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

std::string identity_document(const std::string& suffix) {
    const auto home = lake::test::fixture_path("attenuation-" + suffix);
    setenv("LIQUID_HOME", home.c_str(), 1);
    const auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer);
    if (!signer) {
        return {};
    }
    std::ifstream input(home / "identity" / (signer->identity().fingerprint + ".identity.cbor"),
                        std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    std::string encoded(((bytes.size() + 2) / 3) * 4, '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                    reinterpret_cast<const unsigned char*>(bytes.data()),
                    bytes.size());
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::ranges::replace(encoded, '+', '-');
    std::ranges::replace(encoded, '/', '_');
    return encoded;
}

std::string request(unsigned short port,
                    const std::string& path,
                    const json& input,
                    const std::string& secret,
                    const std::string& request_id,
                    const std::string& method) {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return {};
    }
    timeval timeout{60, 0};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_fd);
        return {};
    }
    const std::string body = method == "GET" ? "" : input.dump();
    std::string wire =
        method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    wire += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size());
    wire += "\r\nX-Request-ID: " + request_id + "\r\n";
    if (!secret.empty()) {
        wire += "Authorization: Bearer " + secret + "\r\n";
    }
    wire += "\r\n" + body;
    std::size_t sent = 0;
    while (sent < wire.size()) {
        const auto count = send(socket_fd, wire.data() + sent, wire.size() - sent, 0);
        if (count <= 0) {
            close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(socket_fd);
    return response;
}

class TokenAttenuation : public testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        postgres_ = GetParam().starts_with("Postgres");
        mcp_ = GetParam().ends_with("MCP");
        if (postgres_) {
            const char* uri = std::getenv("LAKE_TEST_DB_URI");
            if (!uri || !*uri) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            schema_ = "attenuation_" + lake::control::new_uuid_v4();
            std::erase(schema_, '-');
            pqxx::connection connection(uri);
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
            database_uri_ = uri;
            database_uri_ += database_uri_.contains('?') ? "&" : "?";
            database_uri_ += "options=-csearch_path%3D" + schema_;
            store_ = lake::control::make_postgres_store(database_uri_, std::string(instance_kek));
        } else {
            store_ = std::make_shared<lake::control::InMemoryStore>();
        }
        lake::api::ServerOptions options;
        options.port = 0;
        options.public_host = "lake.example.test";
        options.admin_token = operator_secret_;
        options.worker_coroutines = 1;
        options.instance_kek = instance_kek;
        options.store = store_;
        server_ = std::make_unique<lake::api::ApiServer>(std::move(options));
        ASSERT_TRUE(server_->start()) << server_->reason();
        const std::string suffix = lake::control::new_uuid_v4();
        auto signup = call("sign_up_password",
                           {{"email", suffix + "@example.test"},
                            {"password", "correct horse battery staple"},
                            {"display_name", "Token attenuation"},
                            {"identity_document", identity_document(suffix)}},
                           "");
        ASSERT_TRUE(signup.value("ok", false));
        browser_ = signup.at("value");
    }

    void TearDown() override {
        if (!server_) {
            return;
        }
        server_->stop();
        const auto events = store_->list_audit_events({.limit = 100000});
        ASSERT_TRUE(events);
        const auto verified = lake::audit::verify(*store_, *events);
        ASSERT_TRUE(verified) << verified.error().message;
        const std::string audit = lake::audit::json_lines(*events);
        for (const auto& secret : secrets_) {
            EXPECT_EQ(audit.find(secret), std::string::npos);
        }
        server_.reset();
        store_.reset();
        if (postgres_) {
            pqxx::connection connection(std::getenv("LAKE_TEST_DB_URI"));
            pqxx::work tx(connection);
            tx.exec("SET client_min_messages TO warning");
            tx.exec("DROP SCHEMA " + tx.quote_name(schema_) + " CASCADE");
            tx.commit();
        }
    }

    json call(const std::string& name, const json& input, const std::string& secret) {
        const std::string id = lake::control::new_uuid_v4();
        json payload = input;
        if (mcp_) {
            payload = {{"jsonrpc", "2.0"},
                       {"id", id},
                       {"method", "tools/call"},
                       {"params", {{"name", name}, {"arguments", input}}}};
        }
        const bool read = name == "list_devices" || name == "list_sessions";
        const auto wire = request(server_->port(),
                                  mcp_ ? "/mcp" : "/v1/" + name,
                                  payload,
                                  secret,
                                  id,
                                  !mcp_ && read ? "GET" : "POST");
        const auto body = wire.find("\r\n\r\n");
        EXPECT_NE(body, std::string::npos);
        if (body == std::string::npos) {
            return json::object();
        }
        json result = json::parse(wire.substr(body + 4));
        if (mcp_) {
            EXPECT_EQ(result.at("result").at("isError"),
                      !result.at("result").at("structuredContent").at("ok").get<bool>());
            result = result.at("result").at("structuredContent");
        }
        EXPECT_NE(wire.find(mcp_ || result.at("ok").get<bool>()               ? " 200 "
                            : result.at("error").at("code") == "unauthorized" ? " 403 "
                                                                              : " 400 "),
                  std::string::npos);
        if (!secret.empty()) {
            secrets_.push_back(secret);
        }
        if (result.at("ok") && result.at("value").contains("secret")) {
            secrets_.push_back(result.at("value").at("secret"));
        }
        if (result.at("ok") && result.at("value").contains("access_token")) {
            secrets_.push_back(result.at("value").at("access_token"));
        }
        verify_call_audit(id, name, result);
        return result;
    }

    void verify_call_audit(const std::string& id, const std::string& name, const json& result) {
        const auto events = store_->list_audit_events({.limit = 100000});
        ASSERT_TRUE(events);
        std::vector<lake::audit::Event> matches;
        std::ranges::copy_if(*events, std::back_inserter(matches), [&](const auto& event) {
            return event.draft.request_id == id;
        });
        ASSERT_EQ(matches.size(), 1u) << name;
        EXPECT_EQ(matches[0].draft.action, name);
        EXPECT_EQ(matches[0].draft.outcome,
                  result.at("ok").get<bool>()                       ? "success"
                  : result.at("error").at("code") == "unauthorized" ? "denied"
                                                                    : "failure");
        EXPECT_FALSE(matches[0].signature.empty());
    }

    json issue(const json& issuer, const json& scopes, int lifetime = 0) {
        json input = {{"name", "child"}, {"scopes", scopes}};
        if (lifetime) {
            input["expires_in_seconds"] = lifetime;
        }
        const auto result = call("create_token", input, issuer.at("secret"));
        EXPECT_TRUE(result.at("ok").get<bool>());
        if (!result.at("ok")) {
            return json::object();
        }
        return result.at("value");
    }

    void denied(const json& issuer, const json& input, const std::string& message) {
        const auto before = store_->list_sessions(browser_.at("session").at("user_id"), "lake");
        ASSERT_TRUE(before);
        const auto result = call("create_token", input, issuer.at("secret"));
        ASSERT_FALSE(result.at("ok").get<bool>());
        EXPECT_EQ(result.at("error").at("code"), "unauthorized");
        EXPECT_EQ(result.at("error").at("message"), message);
        EXPECT_FALSE(result.contains("value"));
        const auto after = store_->list_sessions(browser_.at("session").at("user_id"), "lake");
        ASSERT_TRUE(after);
        EXPECT_EQ(after->size(), before->size());
    }

    void verify_migration() {
        const auto legacy = store_->create_token({browser_.at("session").at("user_id"),
                                                  "lake",
                                                  "legacy",
                                                  "before migration",
                                                  {"create_token"},
                                                  0,
                                                  true});
        ASSERT_TRUE(legacy);
        std::ifstream schema_file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
        const std::string schema_sql((std::istreambuf_iterator<char>(schema_file)), {});
        ASSERT_FALSE(schema_sql.empty());
        pqxx::connection connection(database_uri_);
        {
            pqxx::work tx(connection);
            tx.exec("SET client_min_messages TO warning");
            tx.exec("DELETE FROM schema_migrations WHERE version = '0021_token_attenuation'");
            tx.exec(schema_sql);
            tx.commit();
        }
        EXPECT_FALSE(store_->context_for(legacy->secret));
        const auto replacement = issue(browser_, {"create_token"}, 60);
        {
            pqxx::work tx(connection);
            tx.exec(schema_sql);
            tx.commit();
        }
        const auto reopened =
            lake::control::make_postgres_store(database_uri_, std::string(instance_kek));
        EXPECT_TRUE(reopened->context_for(replacement.at("secret")));
        {
            pqxx::work tx(connection);
            tx.exec_params("DELETE FROM sessions WHERE id = $1::uuid",
                           browser_.at("session").at("id").get<std::string>());
            tx.commit();
        }
        EXPECT_FALSE(reopened->context_for(replacement.at("secret")));
    }

    void bounded_by(const json& child, const json& parent) {
        const auto child_principal = store_->context_for(child.at("secret"));
        const auto parent_principal = store_->context_for(parent.at("secret"));
        ASSERT_TRUE(child_principal);
        ASSERT_TRUE(parent_principal);
        ASSERT_TRUE(child_principal->expires_at_unix);
        ASSERT_TRUE(parent_principal->expires_at_unix);
        EXPECT_LE(*child_principal->expires_at_unix, *parent_principal->expires_at_unix);
        EXPECT_EQ(child.at("session").at("parent_session_id"), parent.at("session").at("id"));
    }

    std::string operator_secret_ = lake::control::new_uuid_v4();
    std::string schema_;
    std::string database_uri_;
    bool postgres_ = false;
    bool mcp_ = false;
    std::shared_ptr<lake::control::Store> store_;
    std::unique_ptr<lake::api::ApiServer> server_;
    json browser_;
    std::vector<std::string> secrets_;
};

TEST_P(TokenAttenuation, ScopeLifetimeAndRecursiveRevocation) {
    const auto parent = issue(browser_, {"create_token"}, 60);
    ASSERT_TRUE(parent.contains("secret"));
    bounded_by(parent, browser_);
    EXPECT_FALSE(call("list_devices", json::object(), parent.at("secret")).at("ok"));
    denied(parent,
           {{"name", "wildcard-child"},
            {"scopes", {"*"}},
            {"static", true},
            {"reason", "local review proof"}},
           "requested scopes exceed issuing credential");
    denied(parent,
           {{"name", "extra"}, {"scopes", {"create_token", "list_devices"}}},
           "requested scopes exceed issuing credential");
    denied(parent,
           {{"name", "wildcard"}, {"scopes", {"*"}}, {"expires_in_seconds", 10}},
           "requested scopes exceed issuing credential");
    denied(parent,
           {{"name", "static"},
            {"scopes", {"create_token"}},
            {"static", true},
            {"reason", "local review proof"}},
           "issuing credential cannot derive a static token");
    denied(parent,
           {{"name", "long"}, {"scopes", {"create_token"}}, {"expires_in_seconds", 120}},
           "requested lifetime exceeds issuing credential");
    const auto child = issue(parent, {"create_token"}, 30);
    const auto grandchild = issue(child, {"create_token"}, 10);
    const auto default_child = issue(parent, {"create_token"});
    bounded_by(child, parent);
    bounded_by(grandchild, child);
    bounded_by(default_child, parent);
    const auto revoked =
        call("revoke_session", {{"id", parent.at("session").at("id")}}, browser_.at("secret"));
    ASSERT_TRUE(revoked.at("ok"));
    for (const auto& credential : {parent, child, grandchild, default_child}) {
        EXPECT_FALSE(store_->context_for(credential.at("secret")));
        EXPECT_FALSE(call("create_token",
                          {{"name", "after revocation"}, {"scopes", {"create_token"}}},
                          credential.at("secret"))
                         .at("ok"));
    }
}

TEST_P(TokenAttenuation, WildcardStaticAndDeviceAuthority) {
    const auto wildcard = issue(browser_, {"*"}, 600);
    const auto wildcard_child = issue(wildcard, {"*"}, 300);
    const auto narrowed = issue(wildcard, {"list_devices"}, 300);
    EXPECT_TRUE(call("list_devices", json::object(), narrowed.at("secret")).at("ok"));
    bounded_by(wildcard_child, wildcard);
    denied(wildcard,
           {{"name", "static"}, {"scopes", {"*"}}, {"static", true}, {"reason", "proof"}},
           "issuing credential cannot derive a static token");
    denied(browser_,
           {{"name", "static"}, {"scopes", {"*"}}, {"static", true}, {"reason", "proof"}},
           "issuing credential cannot derive a static token");
    // Provision an existing static authority through the trusted store interface.
    const auto root = store_->create_token({browser_.at("session").at("user_id"),
                                            "lake",
                                            "static root",
                                            "operator provisioned",
                                            {"create_token"},
                                            0,
                                            true});
    ASSERT_TRUE(root);
    const auto static_child = call("create_token",
                                   {{"name", "static child"},
                                    {"scopes", {"create_token"}},
                                    {"static", true},
                                    {"reason", "existing static authority"}},
                                   root->secret);
    ASSERT_TRUE(static_child.at("ok"));
    EXPECT_TRUE(static_child.at("value").at("session").at("expires_at").is_null());
    const auto expiring = issue(static_child.at("value"), {"create_token"}, 60);
    ASSERT_TRUE(store_->revoke_session(root->session.user_id, root->session.id));
    EXPECT_FALSE(store_->context_for(static_child.at("value").at("secret")));
    EXPECT_FALSE(store_->context_for(expiring.at("secret")));
    const auto revoked = call(
        "revoke_device", {{"id", browser_.at("session").at("device_id")}}, browser_.at("secret"));
    ASSERT_TRUE(revoked.at("ok"));
    EXPECT_FALSE(call("list_devices", json::object(), wildcard_child.at("secret")).at("ok"));
    EXPECT_FALSE(call("list_devices", json::object(), narrowed.at("secret")).at("ok"));
}

TEST_P(TokenAttenuation, InvalidLifetimesAndExpiredAncestors) {
    const auto parent = issue(browser_, {"create_token"}, 60);
    for (const json lifetime :
         {json(0), json(-1), json(2147483648LL), json(18446744073709551615ULL)}) {
        const auto result = call(
            "create_token",
            {{"name", "invalid"}, {"scopes", {"create_token"}}, {"expires_in_seconds", lifetime}},
            parent.at("secret"));
        ASSERT_FALSE(result.at("ok"));
        EXPECT_EQ(result.at("error").at("code"), "invalid_input");
    }
    if (postgres_) {
        const auto child = issue(parent, {"create_token"}, 30);
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec_params(
            "UPDATE sessions SET expires_at = now() - interval '1 second' WHERE id = $1::uuid",
            parent.at("session").at("id").get<std::string>());
        tx.commit();
        EXPECT_FALSE(store_->context_for(child.at("secret")));
        EXPECT_FALSE(call("create_token",
                          {{"name", "expired"}, {"scopes", {"create_token"}}},
                          child.at("secret"))
                         .at("ok"));
        verify_migration();
    }
}

TEST_P(TokenAttenuation, ApplicationPoolsReturnOnlyDerivedSessionBearer) {
    const auto customer =
        lake::test::personal_customer(*store_, browser_.at("session").at("user_id"));
    ASSERT_TRUE(customer);
    const auto application = call("register_application",
                                  {{"customer_id", customer->id},
                                   {"name", "Attenuation"},
                                   {"allowed_origins", {"https://client.example.test"}},
                                   {"redirect_uris", {"https://client.example.test/callback"}}},
                                  operator_secret_);
    ASSERT_TRUE(application.at("ok"));
    const auto suffix = lake::control::new_uuid_v4();
    const auto signup = call("sign_up_password",
                             {{"pool_id", application.at("value").at("pool_id")},
                              {"email", suffix + "@example.test"},
                              {"password", "correct horse battery staple"},
                              {"display_name", "Application token issuer"},
                              {"identity_document", identity_document(suffix)}},
                             "");
    ASSERT_TRUE(signup.at("ok"));
    ASSERT_TRUE(signup.at("value").contains("access_token"));
    const auto parent = issue(signup.at("value"), {"create_token"}, 60);
    const auto child = issue(parent, {"create_token"}, 30);
    EXPECT_FALSE(parent.contains("access_token"));
    EXPECT_FALSE(child.contains("access_token"));
    bounded_by(child, parent);
    EXPECT_EQ(child.at("session").at("pool_id"), application.at("value").at("pool_id"));
    const auto revoked = call(
        "revoke_session", {{"id", parent.at("session").at("id")}}, signup.at("value").at("secret"));
    ASSERT_TRUE(revoked.at("ok"));
    EXPECT_FALSE(store_->context_for(child.at("secret")));
    const auto events =
        store_->list_application_webhook_events(application.at("value").at("id"), 100);
    ASSERT_TRUE(events);
    EXPECT_EQ(std::ranges::count_if(
                  *events, [](const auto& event) { return event.event == "session.started"; }),
              3);
}

INSTANTIATE_TEST_SUITE_P(Transports,
                         TokenAttenuation,
                         testing::Values("MemoryREST", "MemoryMCP", "PostgresREST", "PostgresMCP"),
                         [](const testing::TestParamInfo<std::string>& info) {
                             return info.param;
                         });
} // namespace
