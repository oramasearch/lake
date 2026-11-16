#include "one_thread_io.hpp"
#include "postgres_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <pqxx/pqxx>

#include "lake/api/capabilities.hpp"
#include "lake/api/clerk.hpp"
#include "lake/api/oidc_jwks.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"

namespace {

using nlohmann::json;

constexpr std::string_view test_kek = "clerk-test-instance-kek-0123456789";

std::string base64(std::string_view value) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    unsigned buffer = 0;
    int bits = 0;
    for (const unsigned char byte : value) {
        buffer = (buffer << 8U) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(buffer >> static_cast<unsigned>(bits)) & 0x3FU]);
        }
    }
    if (bits > 0) {
        output.push_back(alphabet[(buffer << static_cast<unsigned>(6 - bits)) & 0x3FU]);
    }
    while (output.size() % 4 != 0) {
        output.push_back('=');
    }
    return output;
}

std::string base64url(const unsigned char* data, std::size_t size) {
    std::string text = base64(std::string_view(reinterpret_cast<const char*>(data), size));
    while (!text.empty() && text.back() == '=') {
        text.pop_back();
    }
    for (char& character : text) {
        character = character == '+' ? '-' : character == '/' ? '_' : character;
    }
    return text;
}

std::string base64url(std::string_view value) {
    return base64url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Signs Clerk-shaped JWT template tokens and publishes the matching JWKS.
class ClerkInstance {
public:
    ClerkInstance() {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (context != nullptr && EVP_PKEY_keygen_init(context) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1) {
            EVP_PKEY_keygen(context, &key_);
        }
        EVP_PKEY_CTX_free(context);
    }
    ~ClerkInstance() {
        EVP_PKEY_free(key_);
    }
    ClerkInstance(const ClerkInstance&) = delete;
    ClerkInstance& operator=(const ClerkInstance&) = delete;

    static constexpr std::string_view issuer = "https://example-123.clerk.accounts.dev";

    std::string token(json claims) const {
        const std::string header =
            base64url(json{{"alg", "RS256"}, {"kid", "ins_test"}, {"typ", "JWT"}}.dump());
        const std::string input = header + "." + base64url(claims.dump());
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        std::size_t size = 0;
        std::vector<unsigned char> signature;
        if (context != nullptr &&
            EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key_) == 1 &&
            EVP_DigestSignUpdate(context, input.data(), input.size()) == 1 &&
            EVP_DigestSignFinal(context, nullptr, &size) == 1) {
            signature.resize(size);
            EVP_DigestSignFinal(context, signature.data(), &size);
            signature.resize(size);
        }
        EVP_MD_CTX_free(context);
        return input + "." + base64url(signature.data(), signature.size());
    }

    // The claims Clerk puts in a `lake` template token (docs/api/clerk.md).
    std::string template_token(long offset = 0) const {
        const long now = now_unix() + offset;
        return token({{"iss", issuer},
                      {"aud", "lake"},
                      {"azp", "https://lake.example.test"},
                      {"sub", "user_test"},
                      {"email", "person@example.test"},
                      {"name", "A Person"},
                      {"iat", now},
                      {"nbf", now - 5},
                      {"exp", now + 300}});
    }

    std::string jwks() const {
        BIGNUM* modulus = nullptr;
        BIGNUM* exponent = nullptr;
        EVP_PKEY_get_bn_param(key_, OSSL_PKEY_PARAM_RSA_N, &modulus);
        EVP_PKEY_get_bn_param(key_, OSSL_PKEY_PARAM_RSA_E, &exponent);
        std::vector<unsigned char> modulus_bytes(BN_num_bytes(modulus));
        std::vector<unsigned char> exponent_bytes(BN_num_bytes(exponent));
        BN_bn2bin(modulus, modulus_bytes.data());
        BN_bn2bin(exponent, exponent_bytes.data());
        BN_free(modulus);
        BN_free(exponent);
        return json{
            {"keys",
             json::array({{{"kty", "RSA"},
                           {"alg", "RS256"},
                           {"use", "sig"},
                           {"kid", "ins_test"},
                           {"n", base64url(modulus_bytes.data(), modulus_bytes.size())},
                           {"e", base64url(exponent_bytes.data(), exponent_bytes.size())}}})}}
            .dump();
    }

private:
    EVP_PKEY* key_ = nullptr;
};

std::optional<lake::control::identity_crypto::OidcClaims> verify(const ClerkInstance& clerk,
                                                                 const std::string& token) {
    return lake::control::identity_crypto::verify_oidc_id_token(
        token, clerk.jwks(), ClerkInstance::issuer, "lake", "");
}

lake::control::ClerkIdentity minted(std::string subject, std::string email) {
    const auto identity = lake::audit::mint_custodial_identity(test_kek);
    EXPECT_TRUE(identity) << (identity ? "" : identity.error().message);
    return {std::move(subject),
            std::move(email),
            "A Person",
            "browser",
            identity ? identity->taap_id : "",
            identity ? identity->public_key : "",
            identity ? identity->wrapped_seed : "",
            static_cast<std::int64_t>(std::time(nullptr))};
}

void expect_clerk_sign_in_contract(lake::control::Store& store, const std::string& suffix) {
    const std::string email = "clerk-" + suffix + "@example.test";
    const auto first_identity = minted("user_" + suffix, email);
    const auto first = store.sign_in_clerk(first_identity);
    ASSERT_TRUE(first) << first.error().message;
    const auto principal = store.context_for(first->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->taap_id, first_identity.custodial_taap_id);
    EXPECT_EQ(principal->pool_id, "lake");

    // A personal customer the new user administers, as password sign-up provisions.
    const auto customers = store.list_customers(100, principal->user_id);
    ASSERT_TRUE(customers) << customers.error().message;
    ASSERT_EQ(customers->size(), 1U);
    EXPECT_EQ(customers->front().customer_type, "individual");

    // The same Clerk subject signs in to the same user, and a fresh custodial identity is ignored.
    const auto again = store.sign_in_clerk(minted("user_" + suffix, email));
    ASSERT_TRUE(again) << again.error().message;
    const auto again_principal = store.context_for(again->secret);
    ASSERT_TRUE(again_principal);
    EXPECT_EQ(again_principal->user_id, principal->user_id);
    EXPECT_EQ(again_principal->taap_id, first_identity.custodial_taap_id);

    // A second Clerk subject with the same verified email links to the same lake account.
    const auto linked = store.sign_in_clerk(minted("user_other_" + suffix, email));
    ASSERT_TRUE(linked) << linked.error().message;
    const auto linked_principal = store.context_for(linked->secret);
    ASSERT_TRUE(linked_principal);
    EXPECT_EQ(linked_principal->user_id, principal->user_id);
    const auto still_one = store.list_customers(100, principal->user_id);
    ASSERT_TRUE(still_one);
    EXPECT_EQ(still_one->size(), 1U);

    // A new person needs a custodial identity to be created at all.
    auto without_identity = minted("user_new_" + suffix, "new-" + email);
    without_identity.custodial_wrapped_seed.clear();
    EXPECT_FALSE(store.sign_in_clerk(without_identity));
    EXPECT_FALSE(store.sign_in_clerk({"", email, "", "browser", "", "", ""}));
}

} // namespace

TEST(Clerk, PublishableKeyYieldsTheFrontendApiAndIssuer) {
    const std::string key = "pk_test_" + base64("example-123.clerk.accounts.dev$");
    const auto config = lake::api::clerk_config(key);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->frontend_api, "example-123.clerk.accounts.dev");
    EXPECT_EQ(config->issuer, "https://example-123.clerk.accounts.dev");
    EXPECT_EQ(config->jwks_url, "https://example-123.clerk.accounts.dev/.well-known/jwks.json");
    EXPECT_EQ(config->publishable_key, key);
    const auto live = lake::api::clerk_config("pk_live_" + base64("clerk.example.com$"));
    ASSERT_TRUE(live);
    EXPECT_EQ(live->issuer, "https://clerk.example.com");
    // Unpadded, as Clerk issues them.
    std::string unpadded = "pk_test_" + base64("above-seahorse-7535.clerk.accounts.dev$");
    while (unpadded.back() == '=') {
        unpadded.pop_back();
    }
    ASSERT_TRUE(lake::api::clerk_config(unpadded));

    for (const std::string& invalid : {std::string(),
                                       std::string("xxx"),
                                       std::string("sk_test_") + base64("x.clerk.dev$"),
                                       std::string("pk_test_") + base64("x.clerk.dev"),
                                       std::string("pk_test_") + base64("bad host.dev$"),
                                       std::string("pk_test_") + base64("nodots$"),
                                       std::string("pk_test_!!!")}) {
        EXPECT_FALSE(lake::api::clerk_config(invalid)) << invalid;
    }
}

TEST(Clerk, TemplateTokensVerifyAgainstTheInstanceKeys) {
    ClerkInstance clerk;
    const auto claims = verify(clerk, clerk.template_token());
    ASSERT_TRUE(claims);
    EXPECT_EQ(claims->subject, "user_test");
    EXPECT_EQ(claims->email, "person@example.test");
    EXPECT_EQ(claims->display_name, "A Person");

    EXPECT_FALSE(verify(clerk, clerk.template_token(-600))) << "expired";
    EXPECT_FALSE(verify(clerk, clerk.template_token(600))) << "issued in the future";
    const long now = now_unix();
    EXPECT_FALSE(verify(clerk,
                        clerk.token({{"iss", ClerkInstance::issuer},
                                     {"aud", "other"},
                                     {"sub", "user_test"},
                                     {"iat", now},
                                     {"exp", now + 300}})))
        << "a token for another audience, such as Clerk's default session token";
    EXPECT_FALSE(verify(clerk,
                        clerk.token({{"iss", "https://attacker.example"},
                                     {"aud", "lake"},
                                     {"sub", "user_test"},
                                     {"iat", now},
                                     {"exp", now + 300}})));
    EXPECT_FALSE(verify(
        clerk,
        clerk.token(
            {{"iss", ClerkInstance::issuer}, {"aud", "lake"}, {"iat", now}, {"exp", now + 300}})))
        << "no subject";
    std::string tampered = clerk.template_token();
    tampered[tampered.find('.') + 5] ^= 1;
    EXPECT_FALSE(verify(clerk, tampered));
    ClerkInstance other;
    EXPECT_FALSE(lake::control::identity_crypto::verify_oidc_id_token(
        clerk.template_token(), other.jwks(), ClerkInstance::issuer, "lake", ""))
        << "signed by another instance";
}

TEST(Clerk, CustodialIdentitiesAreFreshAndWrapped) {
    const auto first = lake::audit::mint_custodial_identity(test_kek);
    const auto second = lake::audit::mint_custodial_identity(test_kek);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second);
    EXPECT_TRUE(first->taap_id.starts_with("taap:1:ed25519:"));
    EXPECT_NE(first->taap_id, second->taap_id);
    EXPECT_FALSE(first->public_key.empty());
    EXPECT_FALSE(first->wrapped_seed.empty());
    EXPECT_FALSE(lake::audit::mint_custodial_identity(""));
}

TEST(Clerk, MemoryStoreSignsInLinksAndProvisions) {
    lake::control::InMemoryStore store;
    expect_clerk_sign_in_contract(store, lake::control::new_uuid_v4().substr(0, 8));
}

TEST(Clerk, PostgresStoreSignsInLinksProvisionsAndHoldsTheSeed) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(uri, std::string(test_kek));
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    expect_clerk_sign_in_contract(*store, suffix);

    pqxx::connection connection(uri);
    pqxx::read_transaction tx(connection);
    const auto rows =
        tx.exec_params("SELECT c.taap_id, c.wrapped_seed, u.taap_id FROM custodial_identities c "
                       "JOIN users u ON u.id = c.user_id WHERE lower(u.email) = lower($1)",
                       "clerk-" + suffix + "@example.test");
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0][0].as<std::string>(), rows[0][2].as<std::string>());
    EXPECT_FALSE(rows[0][1].as<std::string>().empty());
    const auto credentials =
        tx.exec_params("SELECT count(*) FROM credentials WHERE kind = 'oauth' AND subject LIKE $1",
                       "clerk:user_%" + suffix);
    EXPECT_EQ(credentials[0][0].as<int>(), 2);
}

TEST(Clerk, OperatorsRevokeEveryLiveSessionOfAUser) {
    lake::control::InMemoryStore store;
    const auto first = store.sign_in_clerk(minted("user_revoked", "revoked@example.test"));
    const auto second = store.sign_in_clerk(minted("user_revoked", "revoked@example.test"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto principal = store.context_for(first->secret);
    ASSERT_TRUE(principal);

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext user;
    user.store = &store;
    user.authorized = true;
    user.acting_user_id = principal->user_id;
    user.acting_identity = principal->taap_id;
    EXPECT_FALSE(registry.call("revoke_user_sessions", {{"user_id", principal->user_id}}, user).ok)
        << "only an operator may revoke another user's sessions";

    lake::api::ApiContext operator_context;
    operator_context.store = &store;
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    const auto revoked =
        registry.call("revoke_user_sessions", {{"user_id", principal->user_id}}, operator_context);
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_EQ(revoked.value["revoked"], 2);
    EXPECT_FALSE(store.context_for(first->secret));
    EXPECT_FALSE(store.context_for(second->secret));
}

TEST(Clerk, AccountsSeeOnlyTheirOwnActivity) {
    lake::control::InMemoryStore store;
    const auto first = store.sign_in_clerk(minted("user_activity_a", "a@example.test"));
    const auto second = store.sign_in_clerk(minted("user_activity_b", "b@example.test"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto a = store.context_for(first->secret);
    const auto b = store.context_for(second->secret);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    const auto as = [&](const lake::control::Principal& principal) {
        lake::api::ApiContext context;
        context.store = &store;
        context.authorized = true;
        context.acting_user_id = principal.user_id;
        context.acting_identity = principal.taap_id;
        context.request_id = lake::control::new_uuid_v4();
        return context;
    };
    auto a_context = as(*a);
    ASSERT_TRUE(registry.call("list_sessions", json::object(), a_context).ok);
    auto a_again = as(*a);
    const auto mine = registry.call("my_audit", {{"limit", 100}, {"include_reads", true}}, a_again);
    ASSERT_TRUE(mine.ok) << mine.error.message;
    ASSERT_FALSE(mine.value["items"].empty());
    for (const json& event : mine.value["items"]) {
        EXPECT_EQ(event.value("principal", ""), a->taap_id);
    }
    auto b_context = as(*b);
    const auto theirs = registry.call(
        "my_audit", {{"principal", a->taap_id}, {"limit", 100}, {"include_reads", true}}, b_context);
    ASSERT_TRUE(theirs.ok);
    for (const json& event : theirs.value["items"]) {
        EXPECT_EQ(event.value("principal", ""), b->taap_id) << "a principal filter cannot widen it";
    }
    auto b_audit = as(*b);
    EXPECT_FALSE(registry.call("audit", {{"limit", 10}}, b_audit).ok)
        << "the instance audit log stays operator-only";
}

TEST(Clerk, BackendStatusFailsClosedAndUsesOnlyTheFixedApiOrigin) {
    int calls = 0;
    lake::api::ClerkResponse response{
        200, R"({"id":"user_test","updated_at":123,"banned":false,"locked":false})", ""};
    const lake::api::ClerkGet get = [&](const std::string& url, const auto& headers) {
        ++calls;
        EXPECT_EQ(url, "https://api.clerk.com/v1/users/user_test");
        EXPECT_EQ(headers.front().first, "Authorization");
        EXPECT_EQ(headers.front().second, "Bearer dummy-clerk-key");
        return response;
    };
    EXPECT_FALSE(lake::api::clerk_user_status("user_test", "", get));
    EXPECT_FALSE(lake::api::clerk_user_status("../users/other", "dummy-clerk-key", get));
    EXPECT_EQ(calls, 0);
    const auto valid = lake::api::clerk_user_status("user_test", "dummy-clerk-key", get);
    ASSERT_TRUE(valid);
    EXPECT_FALSE(valid->banned);
    EXPECT_FALSE(valid->locked);
    EXPECT_FALSE(valid->deprovisioned);
    EXPECT_EQ(valid->updated_at, 123);
    response.body =
        R"({"id":"user_test","updated_at":123,"banned":false,"locked":false,"deprovisioned":true})";
    const auto deprovisioned = lake::api::clerk_user_status("user_test", "dummy-clerk-key", get);
    ASSERT_TRUE(deprovisioned);
    EXPECT_TRUE(deprovisioned->deprovisioned);
    for (
        const std::string body :
        {R"({"id":"user_other","updated_at":123,"banned":false,"locked":false})",
         R"({"id":"user_test","updated_at":123,"locked":false})",
         R"({"id":"user_test","updated_at":123,"banned":"false","locked":false})",
         R"({"id":"user_test","updated_at":123,"banned":false,"locked":false,"deprovisioned":"false"})",
         R"({"id":"user_test","updated_at":null,"banned":false,"locked":false})",
         R"({"id":"user_test","updated_at":-1,"banned":false,"locked":false})",
         "null",
         "invalid"}) {
        response.body = body;
        EXPECT_FALSE(lake::api::clerk_user_status("user_test", "dummy-clerk-key", get)) << body;
    }
    response.status = 503;
    EXPECT_FALSE(lake::api::clerk_user_status("user_test", "dummy-clerk-key", get));
    response.status = 200;
    response.error = "timeout";
    EXPECT_FALSE(lake::api::clerk_user_status("user_test", "dummy-clerk-key", get));
    EXPECT_FALSE(lake::api::clerk_user_status(
        "user_test", "dummy-clerk-key", [](const auto&, const auto&) -> lake::api::ClerkResponse {
            throw std::runtime_error("failed");
        }));
}

TEST(Clerk, TemplateExchangeChecksLiveAccountStateWithoutCaching) {
    ClerkInstance clerk;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    std::atomic<bool> banned{false};
    std::atomic<bool> locked{false};
    std::atomic<bool> deprovisioned{false};
    std::atomic<int> status{200};
    std::atomic<int> calls{0};
    lake::api::ServerOptions options;
    // The origins the test tokens' azp name are this Lake's (lake#921).
    options.public_host = "lake.example.test";
    options.host_aliases = {"us.lake.example.test"};
    options.port = 0;
    options.store = store;
    options.instance_kek = std::string(test_kek);
    options.clerk_publishable_key = "pk_test_" + base64("example-123.clerk.accounts.dev$");
    options.clerk_secret_key = "dummy-clerk-key";
    options.clerk_http = [&](const std::string& url, const auto&) -> lake::api::ClerkResponse {
        if (url == std::string(ClerkInstance::issuer) + "/.well-known/jwks.json") {
            return {200, clerk.jwks(), ""};
        }
        EXPECT_EQ(url, "https://api.clerk.com/v1/users/user_test");
        ++calls;
        return {status.load(),
                json{{"id", "user_test"},
                     {"updated_at", 100 + calls.load()},
                     {"banned", banned.load()},
                     {"locked", locked.load()},
                     {"deprovisioned", deprovisioned.load()}}
                    .dump(),
                ""};
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto exchange = [&] {
        const auto body = "token=" + clerk.template_token();
        const auto* bytes = reinterpret_cast<const std::byte*>(body.data());
        const auto response = lake::test::http_request(
            {.url = "http://127.0.0.1:" + std::to_string(server.port()),
             .method = "POST",
             .suffix = "/identity/clerk-session",
             .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
             .body = std::vector<std::byte>(bytes, bytes + body.size())});
        EXPECT_TRUE(response);
        if (response)
            EXPECT_EQ(response->status, 302U);
    };
    const auto count_active = [&] {
        const auto users = store->list_users(10);
        if (!users || users->empty())
            return std::ptrdiff_t{0};
        const auto sessions = store->list_sessions(users->front().id, "lake");
        return sessions ? std::ranges::count_if(*sessions,
                                                [](const auto& s) { return s.revoked_at.empty(); })
                        : std::ptrdiff_t{0};
    };
    exchange();
    EXPECT_EQ(count_active(), 1);
    banned = true;
    exchange();
    EXPECT_EQ(count_active(), 0) << "ban is checked even with a correctly signed template JWT";
    banned = false;
    exchange();
    EXPECT_EQ(count_active(), 1);
    locked = true;
    exchange();
    EXPECT_EQ(count_active(), 0);
    locked = false;
    deprovisioned = true;
    exchange();
    EXPECT_EQ(count_active(), 0) << "Directory Sync deprovisioning also suspends exchange";
    deprovisioned = false;
    status = 503;
    exchange();
    EXPECT_EQ(count_active(), 0) << "an outage refuses exchange";
    status = 200;
    exchange();
    EXPECT_EQ(count_active(), 1);
    EXPECT_EQ(calls.load(), 7) << "no cached status can hide an account restriction";
    server.stop();
}

TEST(HttpServer, ClerkBackendStatusDoesNotBlockOtherRequests) {
    ClerkInstance clerk;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    lake::api::ServerOptions options;
    // The origins the test tokens' azp name are this Lake's (lake#921).
    options.public_host = "lake.example.test";
    options.host_aliases = {"us.lake.example.test"};
    options.port = 0;
    options.worker_coroutines = 2;
    options.store = std::make_shared<lake::control::InMemoryStore>();
    options.clerk_publishable_key = "pk_test_" + base64("example-123.clerk.accounts.dev$");
    options.clerk_secret_key = "dummy-clerk-key";
    options.clerk_http = [&](const std::string& url, const auto&) -> lake::api::ClerkResponse {
        if (url == std::string(ClerkInstance::issuer) + "/.well-known/jwks.json") {
            return {200, clerk.jwks(), ""};
        }
        entered.set_value();
        released.wait();
        return {503, "", "unavailable"};
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto origin = "http://127.0.0.1:" + std::to_string(server.port());
    const auto body = "token=" + clerk.template_token();
    auto exchange = std::async(std::launch::async, [&] {
        const auto* bytes = reinterpret_cast<const std::byte*>(body.data());
        return lake::test::http_request(
            {.url = origin,
             .method = "POST",
             .suffix = "/identity/clerk-session",
             .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
             .body = std::vector<std::byte>(bytes, bytes + body.size())});
    });
    EXPECT_EQ(entered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto probe = std::async(std::launch::async, [&] {
        return lake::test::http_request({.url = origin, .method = "GET", .suffix = "/healthz"});
    });
    const auto responsive = probe.wait_for(std::chrono::seconds(2));
    release.set_value();
    EXPECT_EQ(responsive, std::future_status::ready);
    const auto health = probe.get();
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200U);
    const auto denied = exchange.get();
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 302U);
}

namespace {
// What lake.live's production instance issued on 2026-10-24 for a person with no name in Clerk:
// the `lake` template renders `{{user.full_name}}` as null (lake#1115).
std::string nameless_template_token(const ClerkInstance& clerk,
                                    json email = "person@example.test") {
    const long now = now_unix();
    return clerk.token({{"iss", ClerkInstance::issuer},
                        {"aud", "lake"},
                        {"azp", "https://us.lake.example.test"},
                        {"sub", "user_test"},
                        {"email", std::move(email)},
                        {"name", nullptr},
                        {"jti", "02aec7b63bb547cf8501"},
                        {"iat", now},
                        {"nbf", now - 5},
                        {"exp", now + 300}});
}

lake::api::OidcJwks::Fetch serve(const ClerkInstance& clerk) {
    return [document = clerk.jwks()](const std::string&, bool) {
        return std::optional<std::string>(document);
    };
}
} // namespace

TEST(Clerk, ATemplateTokenWithANullNameOrEmailVerifies) {
    ClerkInstance clerk;
    const auto direct = verify(clerk, nameless_template_token(clerk));
    ASSERT_TRUE(direct) << "a person without a name in Clerk must still sign in (lake#1115)";
    EXPECT_EQ(direct->subject, "user_test");
    EXPECT_EQ(direct->email, "person@example.test");
    EXPECT_EQ(direct->display_name, "");
    const auto no_email = verify(clerk, nameless_template_token(clerk, nullptr));
    ASSERT_TRUE(no_email);
    EXPECT_EQ(no_email->email, "");

    lake::api::OidcJwks cache;
    std::string reason;
    const auto cached = cache.verify(nameless_template_token(clerk),
                                     std::string(ClerkInstance::issuer) + "/.well-known/jwks.json",
                                     ClerkInstance::issuer,
                                     "lake",
                                     "",
                                     false,
                                     serve(clerk),
                                     true,
                                     &reason);
    ASSERT_TRUE(cached) << reason;
    EXPECT_EQ(cached->display_name, "");

    // Null means absent; any other type is still refused, before and after the signature.
    const long now = now_unix();
    const auto numbered = clerk.token({{"iss", ClerkInstance::issuer},
                                       {"aud", "lake"},
                                       {"sub", "user_test"},
                                       {"name", 7},
                                       {"iat", now},
                                       {"exp", now + 300}});
    EXPECT_FALSE(verify(clerk, numbered));
    reason.clear();
    EXPECT_FALSE(cache.verify(numbered,
                              std::string(ClerkInstance::issuer) + "/.well-known/jwks.json",
                              ClerkInstance::issuer,
                              "lake",
                              "",
                              false,
                              serve(clerk),
                              true,
                              &reason));
    EXPECT_EQ(reason, "name is neither a string nor null");
}

TEST(Clerk, ARefusedTokenSaysWhyAndSummarizesWithoutPersonalClaims) {
    ClerkInstance clerk;
    const std::string jwks = std::string(ClerkInstance::issuer) + "/.well-known/jwks.json";
    const auto refusal = [&](const std::string& token) {
        lake::api::OidcJwks cache;
        std::string reason;
        EXPECT_FALSE(cache.verify(
            token, jwks, ClerkInstance::issuer, "lake", "", false, serve(clerk), true, &reason));
        return reason;
    };
    const long now = now_unix();
    EXPECT_EQ(refusal(clerk.template_token(-600)).rfind("expired (exp ", 0), 0U);
    EXPECT_EQ(refusal(clerk.token({{"iss", "https://clerk.example.test/"},
                                   {"aud", "lake"},
                                   {"sub", "user_test"},
                                   {"iat", now},
                                   {"exp", now + 300}})),
              "iss is not https://example-123.clerk.accounts.dev");
    EXPECT_EQ(refusal(clerk.token({{"iss", ClerkInstance::issuer},
                                   {"aud", "other"},
                                   {"sub", "user_test"},
                                   {"iat", now},
                                   {"exp", now + 300}})),
              "aud does not name lake");
    EXPECT_EQ(refusal(clerk.token({{"iss", ClerkInstance::issuer},
                                   {"aud", "lake"},
                                   {"sub", "user_test"},
                                   {"exp", now + 300}})),
              "no iat");
    EXPECT_EQ(refusal("not.a.jwt"), "header, claims or signature do not decode");
    ClerkInstance other;
    lake::api::OidcJwks cache;
    std::string reason;
    EXPECT_FALSE(cache.verify(clerk.template_token(),
                              jwks,
                              ClerkInstance::issuer,
                              "lake",
                              "",
                              false,
                              serve(other),
                              true,
                              &reason));
    EXPECT_EQ(reason, "signature or claims do not verify with key ins_test");

    const auto summary = lake::api::summarize_token(nameless_template_token(clerk));
    EXPECT_EQ(summary.algorithm, "RS256");
    EXPECT_EQ(summary.key_id, "ins_test");
    EXPECT_EQ(summary.issuer, ClerkInstance::issuer);
    EXPECT_EQ(summary.audience, "\"lake\"");
    EXPECT_EQ(summary.authorized_party, "https://us.lake.example.test");
    EXPECT_GT(summary.expires_at, summary.issued_at);
    EXPECT_EQ(summary.claim_types.at("name"), "null");
    EXPECT_EQ(summary.claim_types.at("email"), "string");
    EXPECT_TRUE(lake::api::summarize_token("garbage").issuer.empty());
}

// The exchange end to end: the token shape lake.live refused signs in, and a refused token is
// logged with its reason and registered claims, never the token or the person's claims.
TEST(HttpServer, ClerkExchangeAcceptsANamelessPersonAndLogsWhyATokenWasRefused) {
    ClerkInstance clerk;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ServerOptions options;
    // The origins the test tokens' azp name are this Lake's (lake#921).
    options.public_host = "lake.example.test";
    options.host_aliases = {"us.lake.example.test"};
    options.port = 0;
    options.store = store;
    options.instance_kek = std::string(test_kek);
    options.clerk_publishable_key = "pk_test_" + base64("example-123.clerk.accounts.dev$");
    options.clerk_secret_key = "dummy-clerk-key";
    options.clerk_http = [&](const std::string& url, const auto&) -> lake::api::ClerkResponse {
        if (url == std::string(ClerkInstance::issuer) + "/.well-known/jwks.json") {
            return {200, clerk.jwks(), ""};
        }
        return {200,
                json{{"id", "user_test"},
                     {"updated_at", 100},
                     {"banned", false},
                     {"locked", false},
                     {"deprovisioned", false}}
                    .dump(),
                ""};
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto exchange = [&](const std::string& token) {
        const auto body = "token=" + token;
        const auto* bytes = reinterpret_cast<const std::byte*>(body.data());
        return lake::test::http_request(
            {.url = "http://127.0.0.1:" + std::to_string(server.port()),
             .method = "POST",
             .suffix = "/identity/clerk-session",
             .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
             .body = std::vector<std::byte>(bytes, bytes + body.size())});
    };
    const auto active_sessions = [&] {
        const auto users = store->list_users(10);
        if (!users || users->empty())
            return std::ptrdiff_t{0};
        const auto sessions = store->list_sessions(users->front().id, "lake");
        return sessions ? std::ranges::count_if(*sessions,
                                                [](const auto& s) { return s.revoked_at.empty(); })
                        : std::ptrdiff_t{0};
    };

    const auto signed_in = exchange(nameless_template_token(clerk));
    ASSERT_TRUE(signed_in);
    EXPECT_EQ(signed_in->status, 302U);
    EXPECT_EQ(active_sessions(), 1) << "the nameless person is signed in (lake#1115)";

    // A template token issued to another product sharing the Clerk instance, or to no origin, is
    // not exchanged here (LSR-012, lake#921).
    for (const json& party : {json("https://app.other-product.example.test"), json(nullptr)}) {
        const long issued = now_unix();
        json claims{{"iss", ClerkInstance::issuer},
                    {"aud", "lake"},
                    {"sub", "user_test"},
                    {"email", "person@example.test"},
                    {"iat", issued},
                    {"exp", issued + 300}};
        if (!party.is_null()) {
            claims["azp"] = party;
        }
        std::ostringstream quiet;
        auto* kept = std::cerr.rdbuf(quiet.rdbuf());
        const auto other_product = exchange(clerk.token(claims));
        std::cerr.rdbuf(kept);
        ASSERT_TRUE(other_product);
        EXPECT_EQ(active_sessions(), 1) << party.dump();
        EXPECT_NE(quiet.str().find("azp is not one of this Lake's origins"), std::string::npos)
            << quiet.str();
    }

    const long now = now_unix();
    const std::string refused_token = clerk.token({{"iss", ClerkInstance::issuer},
                                                   {"aud", "other"},
                                                   {"sub", "user_test"},
                                                   {"email", "person@example.test"},
                                                   {"iat", now},
                                                   {"exp", now + 300}});
    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    const auto refused = exchange(refused_token);
    std::cerr.rdbuf(previous);
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 302U);
    EXPECT_EQ(active_sessions(), 1);
    const std::string log = captured.str();
    ASSERT_NE(log.find("\"event\":\"clerk_sign_in_refused\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"reason\":\"aud does not name lake\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"expected_audience\":\"lake\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"kid\":\"ins_test\""), std::string::npos) << log;
    EXPECT_EQ(log.find(refused_token.substr(refused_token.rfind('.') + 1)), std::string::npos)
        << "the signature is never logged";
    EXPECT_EQ(log.find(refused_token.substr(0, refused_token.rfind('.'))), std::string::npos)
        << "the token is never logged";
    EXPECT_EQ(log.find("person@example.test"), std::string::npos);
    EXPECT_EQ(log.find("user_test"), std::string::npos);
    server.stop();
}

// lake#1163: the page posts Clerk's default session token beside the template. Its signed `sid`
// binds the Lake browser session, so a session event ends that browser and nothing else. The
// session token must carry no `aud`, name the template's user and carry a sid; the template can
// never stand in for it.
TEST(Clerk, TheSessionTokenBindsTheBrowserToItsClerkSession) {
    ClerkInstance clerk;
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ServerOptions options;
    // The origins the test tokens' azp name are this Lake's (lake#921).
    options.public_host = "lake.example.test";
    options.host_aliases = {"us.lake.example.test"};
    options.port = 0;
    options.store = store;
    options.instance_kek = std::string(test_kek);
    options.clerk_publishable_key = "pk_test_" + base64("example-123.clerk.accounts.dev$");
    options.clerk_secret_key = "dummy-clerk-key";
    options.clerk_http = [&](const std::string& url, const auto&) -> lake::api::ClerkResponse {
        if (url == std::string(ClerkInstance::issuer) + "/.well-known/jwks.json") {
            return {200, clerk.jwks(), ""};
        }
        return {200,
                json{{"id", "user_test"},
                     {"updated_at", 100},
                     {"banned", false},
                     {"locked", false}}
                    .dump(),
                ""};
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const auto session_token = [&](json changes) {
        const long now = now_unix();
        json claims = {{"iss", ClerkInstance::issuer},
                       {"azp", "https://lake.example.test"},
                       {"sub", "user_test"},
                       {"sid", "sess_browser_a"},
                       {"iat", now},
                       {"nbf", now - 5},
                       {"exp", now + 60}};
        for (const auto& [name, value] : changes.items()) {
            if (value.is_null())
                claims.erase(name);
            else
                claims[name] = value;
        }
        return clerk.token(claims);
    };
    const auto exchange = [&](const std::string& session) {
        std::string body = "token=" + clerk.template_token();
        if (!session.empty())
            body += "&session_token=" + session;
        const auto* bytes = reinterpret_cast<const std::byte*>(body.data());
        const auto response = lake::test::http_request(
            {.url = "http://127.0.0.1:" + std::to_string(server.port()),
             .method = "POST",
             .suffix = "/identity/clerk-session",
             .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
             .body = std::vector<std::byte>(bytes, bytes + body.size())});
        EXPECT_TRUE(response);
    };
    const auto active = [&] {
        const auto users = store->list_users(10);
        if (!users || users->empty())
            return std::vector<lake::control::Session>{};
        auto sessions = store->list_sessions(users->front().id, "lake");
        std::vector<lake::control::Session> live;
        for (const auto& session : sessions ? *sessions : std::vector<lake::control::Session>{}) {
            if (session.revoked_at.empty())
                live.push_back(session);
        }
        return live;
    };

    // Refused: the template posing as the session token, another user, no sid.
    exchange(clerk.template_token());
    EXPECT_TRUE(active().empty()) << "a token with aud cannot be the session token";
    exchange(session_token({{"sub", "user_someone_else"}}));
    EXPECT_TRUE(active().empty()) << "the session token must name the template's user";
    exchange(session_token({{"sid", nullptr}}));
    EXPECT_TRUE(active().empty()) << "the session token must carry a sid";
    exchange(session_token({{"aud", "lake"}}));
    EXPECT_TRUE(active().empty());

    // Browser A and browser B, each bound; and an API key made from browser B.
    exchange(session_token({}));
    ASSERT_EQ(active().size(), 1U);
    const auto browser_a = active().front();
    exchange(session_token({{"sid", "sess_browser_b"}}));
    ASSERT_EQ(active().size(), 2U);
    const std::string user_id = browser_a.user_id;
    const auto both = active();
    const auto browser_b =
        std::ranges::find_if(both, [&](const auto& s) { return s.id != browser_a.id; })->id;
    const auto key =
        store->create_token({user_id, "lake", "agent key", "", {"*"}, 3600, false, browser_b});
    ASSERT_TRUE(key);

    // Clerk ends browser A's session: only browser A's Lake session ends.
    ASSERT_TRUE(store->revoke_clerk_sessions("user_test", "sess_browser_a", now_unix() * 1000));
    const auto left = active();
    EXPECT_TRUE(std::ranges::none_of(left, [&](const auto& s) { return s.id == browser_a.id; }));
    EXPECT_TRUE(std::ranges::any_of(left, [&](const auto& s) { return s.id == browser_b; }));
    EXPECT_TRUE(store->context_for(key->secret)) << "the API key keeps working";
    server.stop();
}

// lake#1163: a person's own account-wide revocation, now that signing out ends no key.
TEST(Clerk, SignOutEverywhereEndsEveryKeyAndDeviceOfOnlyThatPerson) {
    lake::control::InMemoryStore store;
    auto identity = minted("user_everywhere", "everywhere@example.test");
    const auto browser = store.sign_in_clerk(identity);
    const auto other = store.sign_in_clerk(minted("user_bystander", "bystander@example.test"));
    ASSERT_TRUE(browser);
    ASSERT_TRUE(other);
    const auto principal = store.context_for(browser->secret);
    ASSERT_TRUE(principal);
    const auto key = store.create_token(
        {principal->user_id, "lake", "MCP key", "created on Developer -> MCP", {"*"}, 0, true});
    ASSERT_TRUE(key);
    const auto derived = store.create_token(
        {principal->user_id, "lake", "derived", "", {"*"}, 600, false, key->session.id});
    ASSERT_TRUE(derived);
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.authorized = true;
    context.acting_user_id = principal->user_id;
    context.acting_identity = principal->taap_id;
    context.credential_session_id = key->session.id;
    context.request_id = lake::control::new_uuid_v4();
    const auto signed_out = registry.call("sign_out_everywhere", json::object(), context);
    ASSERT_TRUE(signed_out.ok) << signed_out.error.message;
    EXPECT_EQ(signed_out.value["sessions"], 3) << "the browser, the MCP key and its descendant";
    EXPECT_EQ(signed_out.value["devices"], 1);
    EXPECT_FALSE(store.context_for(browser->secret));
    EXPECT_FALSE(store.context_for(key->secret));
    EXPECT_FALSE(store.context_for(derived->secret));
    EXPECT_TRUE(store.context_for(other->secret)) << "another person is untouched";
    lake::api::ApiContext operator_context;
    operator_context.store = &store;
    operator_context.authorized = true;
    operator_context.operator_credential = true;
    operator_context.acting_identity = "admin";
    EXPECT_FALSE(registry.call("sign_out_everywhere", json::object(), operator_context).ok)
        << "an operator credential is not a person";
}
