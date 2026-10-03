#include "audit_test_support.hpp"
#include "personal_customer_test_support.hpp"
#include "postgres_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/application_webhook_records.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "stream_fixture.hpp"
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
                    const std::string& method,
                    bool cookie = false) {
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
        wire += (cookie ? "Cookie: lake_session=" : "Authorization: Bearer ") + secret + "\r\n";
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
            const char* uri = lake::test::postgres_uri();
            if (!uri || !*uri) {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            // The runner's database is private to this test (tests/test_main.cpp).
            database_uri_ = uri;
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
        // Application events live in the application's workspace Zetta (lake#356).
        workspace_dir_ = lake::test::fixture_path("attenuation-workspaces-" +
                                                  lake::control::new_uuid_v4());
        workspaces_ = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{workspace_dir_.string(), 0}, store_, zetta_);
        options.workspaces = workspaces_;
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
        const auto events = lake::test::audit_events(*store_, {.limit = 100});
        ASSERT_TRUE(events);
        const auto verified = lake::audit::verify(*store_, *events);
        ASSERT_TRUE(verified) << verified.error().message;
        const std::string audit = lake::audit::json_lines(*events);
        for (const auto& secret : secrets_) {
            EXPECT_EQ(audit.find(secret), std::string::npos);
        }
        server_.reset();
        store_.reset();
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
        // A credential lake no longer accepts is 401 unauthenticated; one that does not grant
        // the call is 403 unauthorized (lake#450).
        EXPECT_NE(wire.find(mcp_ || result.at("ok").get<bool>()                  ? " 200 "
                            : result.at("error").at("code") == "unauthenticated" ? " 401 "
                            : result.at("error").at("code") == "unauthorized"    ? " 403 "
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
        const auto events = lake::test::audit_events(*store_, {.request_id = id});
        ASSERT_TRUE(events);
        std::vector<lake::audit::Event> matches;
        std::ranges::copy_if(*events, std::back_inserter(matches), [&](const auto& event) {
            return event.draft.request_id == id;
        });
        ASSERT_EQ(matches.size(), 1u) << name;
        EXPECT_EQ(matches[0].draft.action, name);
        EXPECT_EQ(matches[0].draft.outcome,
                  result.at("ok").get<bool>() ? "success"
                  : result.at("error").at("code") == "unauthorized" ||
                          result.at("error").at("code") == "unauthenticated"
                      ? "denied"
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
    std::shared_ptr<lake::test::StreamCaller> zetta_ = std::make_shared<lake::test::StreamCaller>();
    std::shared_ptr<lake::control::WorkspaceService> workspaces_;
    std::filesystem::path workspace_dir_;
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

// lake#234: a signed-in person may create a token that never expires, only when their space opts
// in, with a reason and named scopes. It is a standing credential: signing out does not end it,
// revoking it does.
TEST_P(TokenAttenuation, NeverExpiringTokensNeedTheOptInAndSurviveSignOut) {
    const std::string user_id = browser_.at("session").at("user_id");
    const auto customers = store_->list_customers(10, user_id);
    ASSERT_TRUE(customers && !customers->empty());
    const std::string customer_id = customers->front().id;
    const json standing{{"name", "ci deploy"},
                        {"scopes", {"list_sessions"}},
                        {"never_expires", true},
                        {"reason", "the release pipeline"}};
    denied(browser_,
           standing,
           "credentials that never expire are off for your space; turn on "
           "allow_non_expiring_credentials with set_customer_credential_policy");
    ASSERT_TRUE(store_->set_customer_non_expiring_credentials(customer_id, true));
    json wildcard = standing;
    wildcard["scopes"] = {"*"};
    const auto refused = call("create_token", wildcard, browser_.at("secret"));
    ASSERT_FALSE(refused.at("ok").get<bool>());
    EXPECT_EQ(refused.at("error").at("code"), "invalid_input");
    json no_reason = standing;
    no_reason.erase("reason");
    EXPECT_FALSE(call("create_token", no_reason, browser_.at("secret")).at("ok").get<bool>());

    const auto issued = call("create_token", standing, browser_.at("secret"));
    ASSERT_TRUE(issued.at("ok").get<bool>()) << issued.dump();
    const json token = issued.at("value");
    secrets_.push_back(token.at("secret"));
    const auto principal = store_->context_for(token.at("secret"));
    ASSERT_TRUE(principal);
    EXPECT_FALSE(principal->expires_at_unix) << "no expiry";

    // Signing out ends the browser session, not the standing token.
    const auto signed_out =
        call("revoke_session", {{"id", browser_.at("session").at("id")}}, browser_.at("secret"));
    ASSERT_TRUE(signed_out.at("ok").get<bool>());
    EXPECT_FALSE(store_->context_for(browser_.at("secret")));
    EXPECT_TRUE(store_->context_for(token.at("secret")));
    EXPECT_TRUE(call("list_sessions", json::object(), token.at("secret")).at("ok").get<bool>());

    ASSERT_TRUE(store_->revoke_session(user_id, token.at("session").at("id")));
    EXPECT_FALSE(store_->context_for(token.at("secret")));
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
    const std::string workspace_id = lake::test::ready_workspace(
        *store_, workspace_dir_, customer->id, "attenuation", postgres_ ? database_uri_ : "");
    const auto application = call("register_application",
                                  {{"customer_id", customer->id},
                                   {"workspace_id", workspace_id},
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
    // The application's events, read from its workspace Zetta (lake#356).
    auto records = lake::control::WorkspaceRecords::open(
        [this, customer = customer->id, workspace_id](std::string capability, bool read,
                                                      json input) {
            return workspaces_->call(customer, workspace_id, std::move(capability), read,
                                     std::move(input));
        },
        workspace_id, instance_kek);
    ASSERT_TRUE(records) << records.error().message;
    const auto events = lake::control::settled(liquid::runtime::block_on(
        lake::control::list_application_webhook_events(
            *records, application.at("value").at("id").get<std::string>(), 100)));
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_LT(events->size(), 100U)
        << "The application event query must return its complete fixture";
    EXPECT_EQ(std::ranges::count_if(
                  *events, [](const auto& event) { return event.event == "session.started"; }),
              3);
}

class PostgresSessionAuthentication : public testing::Test {
protected:
    void SetUp() override {
        const char* uri = lake::test::postgres_uri();
        if (!uri) {
            GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
        }
        database_uri_ = uri;
        store_ = lake::control::make_postgres_store(database_uri_, std::string(instance_kek));
        const auto account = store_->sign_up_password({"lake",
                                                       "lookup@example.test",
                                                       "correct horse battery staple",
                                                       "Lookup test",
                                                       "lookup device",
                                                       identity_document("lookup")});
        ASSERT_TRUE(account) << account.error().message;
        browser_ = *account;
        lake::api::ServerOptions options;
        options.port = 0;
        options.admin_token = operator_secret_;
        options.worker_coroutines = 1;
        options.instance_kek = instance_kek;
        options.store = store_;
        server_ = std::make_unique<lake::api::ApiServer>(std::move(options));
        ASSERT_TRUE(server_->start()) << server_->reason();
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    json read(const std::string& path, const std::string& secret, bool cookie = false) {
        const auto wire = request(server_->port(), path, {}, secret, "", "GET", cookie);
        const auto body = wire.find("\r\n\r\n");
        EXPECT_NE(body, std::string::npos);
        if (body == std::string::npos) {
            return {{"ok", false}};
        }
        return json::parse(wire.substr(body + 4));
    }

    std::array<double, 3>
    samples(int population, const std::string& label, const std::string& secret) {
        std::array<double, 3> elapsed{};
        for (auto& seconds : elapsed) {
            const auto start = std::chrono::steady_clock::now();
            const auto response = read("/v1/server_info", secret);
            seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            EXPECT_TRUE(response.at("ok"));
        }
        std::cout << "sessions=" << population << " bearer=" << label << " seconds=";
        for (const auto seconds : elapsed) {
            std::cout << ' ' << std::fixed << std::setprecision(6) << seconds;
        }
        std::cout << '\n';
        std::ranges::sort(elapsed);
        return elapsed;
    }

    lake::control::Result<lake::control::IssuedSession> token() {
        return store_->create_token({browser_.session.user_id,
                                     "lake",
                                     "population",
                                     "",
                                     {"list_sessions"},
                                     3600,
                                     false,
                                     browser_.session.id});
    }

    std::string database_uri_;
    const std::string operator_secret_ = "lookup-test-operator";
    std::shared_ptr<lake::control::Store> store_;
    std::unique_ptr<lake::api::ApiServer> server_;
    lake::control::IssuedSession browser_;
};

TEST_F(PostgresSessionAuthentication, PopulationDoesNotIncreaseRequestCost) {
    std::string session_secret;
    for (int index = 0; index < 6; ++index) {
        const auto issued = token();
        ASSERT_TRUE(issued) << issued.error().message;
        session_secret = issued->secret;
    }
    const auto small_operator = samples(7, "operator", operator_secret_);
    const auto small_session = samples(7, "session", session_secret);
    for (int index = 0; index < 10; ++index) {
        const auto issued = token();
        ASSERT_TRUE(issued) << issued.error().message;
        session_secret = issued->secret;
    }
    const auto large_operator = samples(17, "operator", operator_secret_);
    const auto large_session = samples(17, "session", session_secret);
    samples(17, "anonymous", "");
    EXPECT_LE(large_operator[1], small_operator[1] * 1.5 + 0.020);
    EXPECT_LE(large_session[1], small_session[1] * 1.5 + 0.020);
}

TEST_F(PostgresSessionAuthentication, OnlyTheRandomSecretAuthenticates) {
    const auto issued = token();
    ASSERT_TRUE(issued);
    pqxx::connection connection(database_uri_);
    std::string lookup;
    std::string digest;
    {
        pqxx::read_transaction tx(connection);
        const auto rows =
            tx.exec("SELECT lookup_id::text, secret_hash FROM sessions WHERE id=$1::uuid",
                    pqxx::params{issued->session.id});
        ASSERT_EQ(rows.size(), 1);
        const auto row = rows.front();
        lookup = row[0].as<std::string>();
        digest = row[1].as<std::string>();
        EXPECT_EQ(digest, lake::control::identity_crypto::sha256_hex(issued->secret));
        const auto passwords = tx.exec("SELECT secret_hash FROM credentials WHERE kind='password'");
        ASSERT_EQ(passwords.size(), 1);
        EXPECT_TRUE(passwords.front()[0].as<std::string>().starts_with("argon2id$65536$3$1$"));
    }
    ASSERT_EQ(issued->secret.size(), 105);
    EXPECT_TRUE(issued->secret.starts_with("ls1." + lookup + "."));
    EXPECT_TRUE(store_->context_for(issued->secret));
    EXPECT_FALSE(store_->context_for(digest));
    EXPECT_FALSE(store_->context_for(lookup));
    EXPECT_FALSE(store_->context_for("ls1." + lookup + "." + digest));
    EXPECT_FALSE(store_->context_for("ls1." + lookup + "." + browser_.secret.substr(41)));
    for (const auto& malformed : {std::string(),
                                  issued->secret.substr(1),
                                  issued->secret + "a",
                                  "ls2." + issued->secret.substr(4),
                                  "ls1.x" + issued->secret.substr(5)}) {
        EXPECT_FALSE(store_->context_for(malformed));
    }
    EXPECT_TRUE(store_->sign_in_password(
        {"lake", "lookup@example.test", "correct horse battery staple", "", "new device"}));
    EXPECT_FALSE(store_->sign_in_password(
        {"lake", "lookup@example.test", "wrong password", "", "new device"}));
}

TEST_F(PostgresSessionAuthentication, ScopeExpiryAndRevocationApplyOnNextRequest) {
    const auto issued = token();
    ASSERT_TRUE(issued);
    EXPECT_TRUE(read("/v1/list_sessions", issued->secret).at("ok"));
    EXPECT_FALSE(read("/v1/list_devices", issued->secret).at("ok"));
    EXPECT_TRUE(read("/v1/list_sessions", issued->secret, true).at("ok"));
    ASSERT_TRUE(store_->revoke_session(browser_.session.user_id, issued->session.id));
    EXPECT_FALSE(read("/v1/list_sessions", issued->secret).at("ok"));
    EXPECT_FALSE(read("/v1/list_sessions", issued->secret, true).at("ok"));

    const auto expiring = token();
    ASSERT_TRUE(expiring);
    EXPECT_TRUE(read("/v1/list_sessions", expiring->secret).at("ok"));
    {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec("UPDATE sessions SET expires_at=now()-interval '1 second' WHERE id=$1::uuid",
                pqxx::params{expiring->session.id});
        tx.commit();
    }
    EXPECT_FALSE(read("/v1/list_sessions", expiring->secret).at("ok"));
    EXPECT_FALSE(read("/v1/list_sessions", expiring->secret, true).at("ok"));
    const auto child = token();
    ASSERT_TRUE(child);
    EXPECT_TRUE(read("/v1/list_sessions", child->secret).at("ok"));
    ASSERT_TRUE(store_->revoke_device(browser_.session.user_id, browser_.session.device_id));
    EXPECT_FALSE(read("/v1/list_sessions", child->secret).at("ok"));
    EXPECT_FALSE(read("/v1/list_sessions", browser_.secret).at("ok"));
}

TEST_F(PostgresSessionAuthentication, OperatorPrecedesEvenAMatchingSession) {
    server_->stop();
    lake::api::ServerOptions options;
    options.port = 0;
    options.admin_token = browser_.secret;
    options.store = store_;
    options.instance_kek = instance_kek;
    server_ = std::make_unique<lake::api::ApiServer>(std::move(options));
    ASSERT_TRUE(server_->start());
    EXPECT_TRUE(read("/v1/list_users", browser_.secret).at("ok"));
    EXPECT_TRUE(read("/v1/list_users", browser_.secret, true).at("ok"));
    pqxx::connection connection(database_uri_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT last_used_at FROM sessions WHERE id=$1::uuid",
                              pqxx::params{browser_.session.id});
    ASSERT_EQ(rows.size(), 1);
    EXPECT_TRUE(rows.front()[0].is_null()) << "operator authentication must not touch sessions";
}

TEST_F(PostgresSessionAuthentication, MigrationRevokesLegacySessionsAndIsRepeatable) {
    const std::string legacy_secret = lake::control::identity_crypto::random_secret();
    const auto legacy_hash = lake::control::identity_crypto::hash_secret(legacy_secret);
    ASSERT_FALSE(legacy_hash.empty());
    std::ifstream schema_file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "db/schema.sql");
    const std::string schema_sql((std::istreambuf_iterator<char>(schema_file)), {});
    ASSERT_FALSE(schema_sql.empty());
    pqxx::connection connection(database_uri_);
    {
        pqxx::work tx(connection);
        tx.exec("ALTER TABLE sessions DROP COLUMN lookup_id");
        tx.exec("DROP TABLE session_lookup_guard");
        tx.exec("DELETE FROM schema_migrations WHERE version='0022_session_lookup'");
        tx.exec("UPDATE sessions SET secret_hash=$1 WHERE id=$2::uuid",
                pqxx::params{legacy_hash, browser_.session.id});
        tx.exec(schema_sql);
        const auto rows = tx.exec("SELECT revoked_at, lookup_id FROM sessions WHERE id=$1::uuid",
                                  pqxx::params{browser_.session.id});
        EXPECT_FALSE(rows.front()[0].is_null());
        EXPECT_TRUE(rows.front()[1].is_null());
        tx.commit();
    }
    EXPECT_FALSE(store_->context_for(legacy_secret));
    EXPECT_FALSE(store_->context_for(browser_.secret));
    const auto replacement = store_->sign_in_password(
        {"lake", "lookup@example.test", "correct horse battery staple", "", "replacement"});
    ASSERT_TRUE(replacement) << replacement.error().message;
    {
        pqxx::work tx(connection);
        tx.exec(schema_sql);
        tx.exec(schema_sql);
        tx.commit();
    }
    auto reopened = lake::control::make_postgres_store(database_uri_, std::string(instance_kek));
    EXPECT_TRUE(reopened->context_for(replacement->secret));
    EXPECT_FALSE(reopened->context_for(legacy_secret));
}

std::array<long long, 2> session_read_counts(const std::string& uri) {
    pqxx::connection connection(uri);
    // Closing a measured store flushes its counters. Wait for that backend to exit before reading.
    for (int attempt = 0; attempt < 200; ++attempt) {
        pqxx::read_transaction tx(connection);
        const auto active = tx.exec("SELECT count(*) FROM pg_stat_activity WHERE datname="
                                    "current_database() AND pid<>pg_backend_pid()");
        if (active.front()[0].as<int>() == 0) {
            const auto rows = tx.exec("SELECT seq_tup_read, idx_tup_fetch FROM pg_stat_user_tables "
                                      "WHERE relname='sessions'");
            return {rows.front()[0].as<long long>(), rows.front()[1].as<long long>()};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "measured PostgreSQL connection did not close";
    return {};
}

TEST_F(PostgresSessionAuthentication, IndexedReadsStayBoundedAtSevenThousandSessions) {
    {
        pqxx::connection connection(database_uri_);
        pqxx::work tx(connection);
        tx.exec("INSERT INTO sessions (user_id, kind, secret_hash, lookup_id, scopes, expires_at) "
                "SELECT $1::uuid, 'api_token', md5('unrelated-' || n)::text, gen_random_uuid(), "
                "ARRAY['list_sessions'], now()+interval '1 hour' FROM generate_series(1,6999) n",
                pqxx::params{browser_.session.user_id});
        tx.exec("ANALYZE sessions");
        tx.commit();
    }
    samples(7000, "operator", operator_secret_);
    samples(7000, "session", browser_.secret);
    server_->stop();
    server_.reset();
    store_.reset();
    const auto before = session_read_counts(database_uri_);
    {
        auto measured =
            lake::control::make_postgres_store(database_uri_, std::string(instance_kek));
        std::string wrong = browser_.secret;
        wrong.back() = wrong.back() == 'a' ? 'b' : 'a';
        const auto unknown = "ls1." + lake::control::new_uuid_v4() + "." + wrong.substr(41);
        for (int count = 0; count < 3; ++count) {
            ASSERT_TRUE(measured->context_for(browser_.secret));
            EXPECT_FALSE(measured->context_for(wrong));
            EXPECT_FALSE(measured->context_for(unknown));
        }
    }
    const auto after = session_read_counts(database_uri_);
    EXPECT_EQ(after[0] - before[0], 0) << "authentication scanned unrelated session rows";
    EXPECT_GT(after[1] - before[1], 0);
    EXPECT_LE(after[1] - before[1], 24) << "authentication fetched unrelated indexed rows";
    std::cout << "sessions=7000 auth_calls=9 sequential_rows=" << after[0] - before[0]
              << " indexed_rows=" << after[1] - before[1] << '\n';
}

INSTANTIATE_TEST_SUITE_P(Transports,
                         TokenAttenuation,
                         testing::Values("MemoryREST", "MemoryMCP", "PostgresREST", "PostgresMCP"),
                         [](const testing::TestParamInfo<std::string>& info) {
                             return info.param;
                         });
} // namespace
