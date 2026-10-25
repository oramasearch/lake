// Browser keys (lake#1152, docs/api/browser-keys.md): the key a website's own page holds. Each test
// runs over the in-memory store and, with LAKE_TEST_DB_URI, over PostgreSQL (migration
// 0102_browser_keys): what a key may call (its index, its model, its workspace, its account, search
// and predict only), from where (its origins), how often (its calls a minute), how long (until
// revoked, whatever happens to its maker's sign-in), who may make one (a person, an API token),
// and that what it does is metered to the workspace. The CORS answer is in
// tests/test_http_server.cpp (ABrowserKeyAnswersOnlyItsWebsiteAndOutlivesSignOut).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/zetta_control.hpp"
#include "postgres_fixture.hpp"
#include "workspace_fixture.hpp"

namespace {

using json = nlohmann::json;
using lake::control::ZettaControlError;
using lake::control::ZettaControlResult;

constexpr std::string_view kek = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string site = "https://shop.example.test";

// A workspace engine with published indexes `index-1` and `index-2` and published models
// `model-1` and `model-2`.
std::optional<ZettaControlResult<json>> engine(const std::string& capability, const json& input) {
    if (capability == "get_index") {
        const std::string id = input.value("index_id", std::string());
        if (id != "index-1" && id != "index-2") {
            return ZettaControlResult<json>(
                std::unexpected(ZettaControlError{"not_found", "no index " + id}));
        }
        return json{{"index_id", id}, {"exposed", true}};
    }
    if (capability == "search") {
        return json{
            {"hits",
             json::array({{{"id", "1"}, {"score", 1.5}, {"fields", {{"title", "Alien"}}}}})}};
    }
    if (capability == "get_model") {
        const std::string id = input.value("model_id", std::string());
        if (id != "model-1" && id != "model-2") {
            return ZettaControlResult<json>(
                std::unexpected(ZettaControlError{"not_found", "no model " + id}));
        }
        return json{{"id", id}, {"exposed", true}};
    }
    if (capability == "score") {
        return json{{"ranked", json::array({{{"entity", 0}, {"risk", 0.75}}})}};
    }
    return std::nullopt;
}

class BrowserKeys : public testing::TestWithParam<bool> {
protected:
    struct Account {
        std::string identity, user_id, customer_id, workspace_id;
    };

    void SetUp() override {
        if (GetParam()) {
            const char* uri = lake::test::postgres_uri();
            if (uri == nullptr || *uri == '\0') {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            uri_ = uri;
            store_ = lake::control::make_postgres_store(uri_, std::string(kek));
        } else {
            store_ = std::make_shared<lake::control::InMemoryStore>();
        }
        workspaces_ = std::make_unique<lake::test::RecordWorkspaces>(store_, uri_);
        workspaces_->zetta->responder = engine;
        recorder_ =
            std::make_unique<lake::api::UsageRecorder>(store_.get(), workspaces_->service.get());
        lake::api::register_core_capabilities(registry_, "test");
        first_ = account("first");
        second_ = account("second");
    }

    Account person(const std::string& name) {
        Account out;
        const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
        out.identity = "taap:1:ed25519:browser-key-" + name + "-" + suffix;
        const auto user =
            store_->create_user({out.identity, name + "-" + suffix + "@example.test", name});
        EXPECT_TRUE(user) << user.error().message;
        out.user_id = user ? user->id : std::string();
        return out;
    }

    Account account(const std::string& name) {
        Account out = person(name);
        const auto customer =
            store_->create_customer({"individual",
                                     out.user_id,
                                     "",
                                     "bk" + name + out.identity.substr(out.identity.size() - 8)});
        EXPECT_TRUE(customer) << customer.error().message;
        out.customer_id = customer->id;
        grant(out.identity, out.customer_id, "admin");
        out.workspace_id = workspaces_->create(out.customer_id, "home");
        return out;
    }

    void
    grant(const std::string& identity, const std::string& customer_id, const std::string& role) {
        const auto object = store_->find_policy_object("customer", customer_id);
        ASSERT_TRUE(object) << object.error().message;
        ASSERT_TRUE(store_->create_grant(
            {identity, object->id, role, "attestation-" + identity, "admin", "request"}));
    }

    lake::api::ApiContext base() {
        lake::api::ApiContext context;
        context.store = store_.get();
        context.workspaces = workspaces_->service.get();
        context.usage_recorder = recorder_.get();
        context.request_id = lake::control::new_uuid_v4();
        context.instance_kek = std::string(kek);
        context.registry = &registry_;
        return context;
    }

    // What the HTTP server makes of a bearer credential (http_server.cpp, context_for).
    lake::api::ApiContext bearer(const std::string& secret, const std::string& origin = {}) {
        auto context = base();
        context.authentication_method = "bearer";
        context.caller_origin = origin;
        const auto principal = store_->context_for(secret);
        if (!principal) {
            context.credential_rejection =
                principal.error().code == "expired" ? "expired" : "invalid";
            return context;
        }
        context.authorized = true;
        context.session_credential = !principal->browser_key;
        context.acting_user_id = principal->user_id;
        context.acting_identity = principal->taap_id;
        context.credential_session_id = principal->session_id;
        context.credential_kind = principal->kind;
        context.credential_workload_id = principal->workload_id;
        context.credential_customer_id = principal->workload_customer_id;
        context.credential_token_hash = principal->workload_token_hash;
        context.credential_expires_at_unix = principal->expires_at_unix;
        context.scopes = principal->scopes;
        context.browser_key = principal->browser_key;
        return context;
    }

    // A signed-in person: a credential that may call anything they may.
    std::string session(const Account& who) {
        const auto issued =
            store_->create_token({who.user_id, "lake", "browser", "", {"*"}, 3600, false});
        EXPECT_TRUE(issued) << issued.error().message;
        return issued ? issued->secret : std::string();
    }

    lake::api::ApiResult call(const std::string& name, json input, lake::api::ApiContext& context) {
        auto completed = liquid::runtime::block_on(registry_.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? std::move(*completed) : lake::api::ApiResult{};
    }

    lake::api::ApiResult call(const std::string& name,
                              json input,
                              const std::string& secret,
                              const std::string& origin = {}) {
        auto context = bearer(secret, origin);
        return call(name, std::move(input), context);
    }

    json key_input() const {
        return {{"customer_id", first_.customer_id},
                {"workspace_id", first_.workspace_id},
                {"index_id", "index-1"},
                {"origins", json::array({site})}};
    }

    // A key of the first account on index-1 and model-1, from `site`; `extra` changes the input
    // (a null value leaves the field out).
    json make_key(const json& extra = json::object()) {
        json input = key_input();
        input["model_id"] = "model-1";
        for (const auto& [field, value] : extra.items()) {
            if (value.is_null()) {
                input.erase(field);
            } else {
                input[field] = value;
            }
        }
        const auto made = call("create_browser_key", input, session(first_));
        EXPECT_TRUE(made.ok) << made.error.message;
        return made.value;
    }

    json search(const std::string& index = "index-1") const {
        return {{"customer_id", first_.customer_id},
                {"workspace_id", first_.workspace_id},
                {"index_id", index},
                {"query", "alien"}};
    }

    json predict(const std::string& model = "model-1") const {
        return {{"customer_id", first_.customer_id},
                {"workspace_id", first_.workspace_id},
                {"model_id", model},
                {"rows", json::array({{{"title", "Alien"}}})}};
    }

    std::string uri_;
    std::shared_ptr<lake::control::Store> store_;
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces_;
    std::unique_ptr<lake::api::UsageRecorder> recorder_;
    lake::api::Registry registry_;
    Account first_, second_;
};

INSTANTIATE_TEST_SUITE_P(Stores,
                         BrowserKeys,
                         testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

TEST_P(BrowserKeys, AKeySearchesItsIndexAndAsksItsModelAndNothingElse) {
    const json made = make_key();
    const std::string key = made.at("secret");
    ASSERT_TRUE(key.starts_with("lbk.")) << key;
    EXPECT_EQ(made["key"]["index_id"], "index-1");
    EXPECT_EQ(made["key"]["model_id"], "model-1");
    EXPECT_EQ(made["key"]["capabilities"], json::array({"search_index", "predict"}));
    EXPECT_EQ(made["key"]["origins"], json::array({site}));
    EXPECT_EQ(made["key"]["rate_limit_per_minute"], 600);
    EXPECT_EQ(made["key"]["state"], "active");
    EXPECT_EQ(made["key"]["expires_at"], nullptr);
    EXPECT_EQ(made["key"]["created_by"], first_.identity);
    EXPECT_EQ(made["index_published"], true);

    const auto found = call("search_index", search(), key, site);
    ASSERT_TRUE(found.ok) << found.error.message;
    EXPECT_EQ(found.value["hits"][0]["fields"]["title"], "Alien");
    const auto answered = call("predict", predict(), key, site);
    ASSERT_TRUE(answered.ok) << answered.error.message;
    EXPECT_EQ(answered.value["ranked"][0]["risk"], 0.75);

    const auto refused = [&](const std::string& name, const json& input, const std::string& path) {
        const auto result = call(name, input, key, site);
        EXPECT_FALSE(result.ok) << name << " " << input.dump();
        EXPECT_EQ(result.error.code, "unauthorized") << name << ": " << result.error.message;
        EXPECT_EQ(result.error.path, path) << name << ": " << result.error.message;
    };
    // Another index or model of the same workspace.
    refused("search_index", search("index-2"), "/index_id");
    refused("predict", predict("model-2"), "/model_id");
    // Another workspace of the same account.
    const std::string lab = workspaces_->create(first_.customer_id, "lab");
    json in_lab = search();
    in_lab["workspace_id"] = lab;
    refused("search_index", in_lab, "/workspace_id");
    // Another account's workspace.
    json theirs = search();
    theirs["customer_id"] = second_.customer_id;
    theirs["workspace_id"] = second_.workspace_id;
    refused("search_index", theirs, "/customer_id");
    // Any other capability, reads and writes alike.
    refused("list_indexes",
            {{"customer_id", first_.customer_id}, {"workspace_id", first_.workspace_id}},
            "");
    refused("list_customers", json::object(), "");
    refused("create_browser_key", key_input(), "");
    refused("list_browser_keys", {{"customer_id", first_.customer_id}}, "");

    // A key made without a model only searches.
    const json searching = make_key({{"model_id", nullptr}});
    EXPECT_EQ(searching["key"]["model_id"], nullptr);
    EXPECT_EQ(searching["key"]["capabilities"], json::array({"search_index"}));
    const auto asked = call("predict", predict(), searching.at("secret"), site);
    EXPECT_EQ(asked.error.code, "unauthorized") << asked.error.message;
    EXPECT_TRUE(call("search_index", search(), searching.at("secret"), site).ok);
}

TEST_P(BrowserKeys, AKeyAnswersOnlyTheWebsitesItLists) {
    const json made = make_key(
        {{"origins", json::array({"https://SHOP.example.test/", "http://localhost:5173"})}});
    EXPECT_EQ(made["key"]["origins"], json::array({site, "http://localhost:5173"}))
        << "kept lowercase, without a trailing slash";
    const std::string key = made.at("secret");
    EXPECT_TRUE(call("search_index", search(), key, site).ok);
    EXPECT_TRUE(call("search_index", search(), key, "http://localhost:5173").ok);
    for (const std::string origin : {std::string(),
                                     std::string("https://evil.example.test"),
                                     std::string("http://shop.example.test"),
                                     std::string("https://shop.example.test:8443"),
                                     std::string("null")}) {
        const auto result = call("search_index", search(), key, origin);
        EXPECT_EQ(result.error.code, "unauthorized") << "'" << origin << "'";
    }

    // What an origin may be.
    for (const std::string bad : {"www.example.com",
                                  "http://example.com",
                                  "https://example.com/path",
                                  "https://exa mple.com",
                                  "ftp://example.com",
                                  "https://example.com:99999"}) {
        json input = key_input();
        input["origins"] = json::array({bad});
        const auto result = call("create_browser_key", input, session(first_));
        EXPECT_EQ(result.error.code, "invalid_input") << bad;
        EXPECT_EQ(result.error.path, "/origins/0") << bad;
    }
}

TEST_P(BrowserKeys, AKeyOutlivesItsMakersSignInUntilItIsRevoked) {
    // A person signs in and makes an API token under that sign-in; the token makes the key.
    const std::string signed_in = session(first_);
    auto parent = bearer(signed_in);
    const auto token =
        call("create_token", {{"name", "website setup"}, {"scopes", json::array({"*"})}}, parent);
    ASSERT_TRUE(token.ok) << token.error.message;
    const std::string token_secret = token.value.at("secret");
    const auto made = call("create_browser_key", key_input(), token_secret);
    ASSERT_TRUE(made.ok) << made.error.message;
    const std::string key = made.value.at("secret");
    const std::string id = made.value.at("key").at("id");

    // Signing out ends the session and every token under it, not the key.
    ASSERT_TRUE(store_->revoke_session(first_.user_id, parent.credential_session_id));
    EXPECT_FALSE(store_->context_for(signed_in));
    EXPECT_FALSE(store_->context_for(token_secret)) << "the token stopped with its sign-in";
    EXPECT_TRUE(call("search_index", search(), key, site).ok) << "the website keeps working";

    // Another account can neither see nor end it.
    const auto foreign = call(
        "revoke_browser_key", {{"customer_id", second_.customer_id}, {"id", id}}, session(second_));
    EXPECT_EQ(foreign.error.code, "not_found") << foreign.error.message;
    const auto meddling = call(
        "revoke_browser_key", {{"customer_id", first_.customer_id}, {"id", id}}, session(second_));
    EXPECT_EQ(meddling.error.code, "unauthorized") << meddling.error.message;
    EXPECT_TRUE(call("search_index", search(), key, site).ok);

    // Listed, never with its secret; revoked, it is refused at once.
    const std::string mine = session(first_);
    const auto listed = call("list_browser_keys", {{"customer_id", first_.customer_id}}, mine);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["keys"].size(), 1U);
    EXPECT_EQ(listed.value["keys"][0]["id"], id);
    EXPECT_EQ(listed.value["keys"][0].dump().find(key.substr(41)), std::string::npos);
    EXPECT_TRUE(call("list_browser_keys",
                     {{"customer_id", first_.customer_id}, {"index_id", "index-2"}},
                     mine)
                    .value["keys"]
                    .empty());
    const auto revoked =
        call("revoke_browser_key", {{"customer_id", first_.customer_id}, {"id", id}}, mine);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_EQ(revoked.value["key"]["state"], "revoked");
    EXPECT_NE(revoked.value["key"]["revoked_at"], nullptr);
    const auto after = call("search_index", search(), key, site);
    EXPECT_EQ(after.error.code, "unauthenticated") << after.error.message;
    EXPECT_EQ(after.error.reason, "invalid");
    const auto again =
        call("revoke_browser_key", {{"customer_id", first_.customer_id}, {"id", id}}, mine);
    EXPECT_TRUE(again.ok) << "revoking a revoked key changes nothing";
    EXPECT_EQ(again.value["key"]["revoked_at"], revoked.value["key"]["revoked_at"]);
}

TEST_P(BrowserKeys, AnApiTokenWithWriteOnTheAccountMakesOne) {
    const auto scoped = store_->create_token({first_.user_id,
                                              "lake",
                                              "agent",
                                              "",
                                              {"create_browser_key", "list_browser_keys"},
                                              3600,
                                              false});
    ASSERT_TRUE(scoped) << scoped.error().message;
    const json input = key_input();
    const auto made = call("create_browser_key", input, scoped->secret);
    ASSERT_TRUE(made.ok) << made.error.message;
    EXPECT_TRUE(call("search_index", search(), made.value.at("secret"), site).ok);

    // A token not scoped to it, and a person who may only read the account, are refused.
    const auto narrow = store_->create_token(
        {first_.user_id, "lake", "reader", "", {"list_browser_keys"}, 3600, false});
    ASSERT_TRUE(narrow);
    EXPECT_EQ(call("create_browser_key", input, narrow->secret).error.code, "unauthorized");
    const Account reader = person("reader");
    grant(reader.identity, first_.customer_id, "read");
    EXPECT_EQ(call("create_browser_key", input, session(reader)).error.code, "unauthorized");
    EXPECT_TRUE(
        call("list_browser_keys", {{"customer_id", first_.customer_id}}, session(reader)).ok);

    // What it is bound to must be there: an unknown index or model, another account's workspace.
    json unknown = input;
    unknown["index_id"] = "index-9";
    EXPECT_FALSE(call("create_browser_key", unknown, scoped->secret).ok);
    unknown = input;
    unknown["model_id"] = "model-9";
    const auto no_model = call("create_browser_key", unknown, scoped->secret);
    EXPECT_EQ(no_model.error.code, "not_found");
    EXPECT_EQ(no_model.error.path, "/model_id");
    unknown = input;
    unknown["workspace_id"] = second_.workspace_id;
    EXPECT_EQ(call("create_browser_key", unknown, scoped->secret).error.code, "invalid_input");
}

TEST_P(BrowserKeys, AKeyMakesOnlyItsCallsAMinute) {
    const std::string key = make_key({{"rate_limit_per_minute", 3}}).at("secret");
    for (int i = 0; i < 3; ++i) {
        const auto result = call("search_index", search(), key, site);
        EXPECT_TRUE(result.ok) << i << ": " << result.error.message;
    }
    const auto limited = call("search_index", search(), key, site);
    EXPECT_EQ(limited.error.code, "rate_limited") << limited.error.message;
    EXPECT_EQ(lake::api::rest_status(limited), 429);
    EXPECT_FALSE(call("predict", predict(), key, site).ok) << "one minute for both capabilities";
    // Another key of the same workspace has its own minute.
    EXPECT_TRUE(call("search_index", search(), make_key().at("secret"), site).ok);
    json too_many = key_input();
    too_many["rate_limit_per_minute"] = 6001;
    EXPECT_EQ(call("create_browser_key", too_many, session(first_)).error.code, "invalid_input");
}

TEST_P(BrowserKeys, WhatAKeyDoesIsMeteredToItsWorkspace) {
    const std::string key = make_key().at("secret");
    ASSERT_TRUE(call("search_index", search(), key, site).ok);
    ASSERT_TRUE(call("search_index", search(), key, site).ok);
    ASSERT_TRUE(call("predict", predict(), key, site).ok);
    static_cast<void>(liquid::runtime::block_on(recorder_->flush()));
    auto& usage = workspaces_->records(first_.workspace_id);
    EXPECT_EQ(usage.metric_total("lake_usage", {{"dimension", "searches"}}), 2);
    EXPECT_EQ(usage.metric_total("lake_usage", {{"dimension", "predictions"}}), 1);
    EXPECT_EQ(workspaces_->records(second_.workspace_id).metric_total("lake_usage"), 0);
}

TEST_P(BrowserKeys, ADeletedWorkspacesKeysStopWorking) {
    const std::string key = make_key().at("secret");
    ASSERT_TRUE(call("search_index", search(), key, site).ok);
    ASSERT_TRUE(store_->update_workspace(first_.workspace_id, {.desired = "deleted"}));
    EXPECT_FALSE(store_->context_for(key));
}

TEST_P(BrowserKeys, AnExpiredKeyIsToldItExpired) {
    if (!GetParam()) {
        GTEST_SKIP() << "the in-memory store has no way to move a key's expiry into the past";
    }
    const json made = make_key({{"expires_in_seconds", 3600}});
    EXPECT_NE(made["key"]["expires_at"], nullptr);
    const std::string key = made.at("secret");
    const std::string id = made["key"]["id"];
    EXPECT_TRUE(call("search_index", search(), key, site).ok);
    {
        pqxx::connection connection(uri_);
        pqxx::work tx(connection);
        tx.exec_params(
            "UPDATE browser_keys SET expires_at = now() - interval '1 second' WHERE id = $1::uuid",
            id);
        tx.commit();
    }
    const auto expired = call("search_index", search(), key, site);
    EXPECT_EQ(expired.error.code, "unauthenticated");
    EXPECT_EQ(expired.error.reason, "expired");
    const auto listed =
        call("list_browser_keys", {{"customer_id", first_.customer_id}}, session(first_));
    EXPECT_EQ(listed.value["keys"][0]["state"], "expired");

    // The table keeps only the hash, and its checks hold for any writer.
    pqxx::connection connection(uri_);
    pqxx::work tx(connection);
    const auto row =
        tx.exec_params1("SELECT secret_sha256 FROM browser_keys WHERE id = $1::uuid", id);
    EXPECT_EQ(row[0].as<std::string>().size(), 64U);
    EXPECT_EQ(key.find(row[0].as<std::string>()), std::string::npos);
    EXPECT_THROW(
        tx.exec_params("UPDATE browser_keys SET origins = ARRAY[]::text[] WHERE id = $1::uuid", id),
        pqxx::check_violation);
}

} // namespace
