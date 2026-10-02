#include "postgres_fixture.hpp"

#include <chrono>
#include <memory>
#include <optional>
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
#include "lake/api/registry.hpp"
#include "lake/api/web_ui.hpp"
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
            identity ? identity->wrapped_seed : ""};
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

TEST(Clerk, SignInPageMountsClerkAndExchangesTheLakeTemplateToken) {
    const auto config =
        lake::api::clerk_config("pk_test_" + base64("example-123.clerk.accounts.dev$"));
    ASSERT_TRUE(config);
    const std::string page = lake::api::web_ui_clerk_sign_in(*config, "sign-in", "", false);
    EXPECT_NE(page.find("data-clerk-publishable-key=\"" + config->publishable_key + "\""),
              std::string::npos);
    EXPECT_NE(page.find("src=\"https://example-123.clerk.accounts.dev/npm/@clerk/clerk-js@5/dist/"
                        "clerk.browser.js\""),
              std::string::npos);
    EXPECT_NE(page.find("getToken({template:'lake'})"), std::string::npos);
    EXPECT_NE(page.find("f.action='/identity/clerk-session'"), std::string::npos);
    EXPECT_NE(page.find("mountSignIn"), std::string::npos);
    EXPECT_NE(page.find("if(false)"), std::string::npos);
    EXPECT_EQ(page.find("sk_"), std::string::npos);

    const std::string signed_out =
        lake::api::web_ui_clerk_sign_in(*config, "sign-in", "<bad>", true);
    EXPECT_NE(signed_out.find("if(true){await window.Clerk.signOut()"), std::string::npos);
    EXPECT_NE(signed_out.find("&lt;bad&gt;"), std::string::npos);
    EXPECT_EQ(signed_out.find("<bad>"), std::string::npos);

    const std::string sign_up = lake::api::web_ui_clerk_sign_in(*config, "sign-up", "", false);
    EXPECT_NE(sign_up.find("mountSignUp"), std::string::npos);
    EXPECT_NE(sign_up.find("Create Your Account"), std::string::npos);
}

// Under Clerk, sign-up is restricted: the visitor home page must not offer an account the
// sign-up page then refuses (lake#448). Both say the same sentence about invitations.
TEST(Clerk, VisitorsAreNotOfferedAnAccountTheSignUpPageRefuses) {
    const auto config = lake::api::clerk_config("pk_test_" + base64("example-123.clerk.accounts.dev$"));
    ASSERT_TRUE(config);
    const std::string note(lake::api::web_ui_invitation_note);
    const std::string sign_up = lake::api::web_ui_clerk_sign_in(*config, "sign-up", "", false);
    EXPECT_NE(sign_up.find(note), std::string::npos);

    lake::api::WebUiViewer visitor;
    visitor.clerk = true;
    const std::string home = lake::api::web_ui_home(visitor);
    EXPECT_EQ(home.find("href=\"/sign-up\""), std::string::npos)
        << "neither the actions nor the top bar link sign-up";
    EXPECT_EQ(home.find("Create account"), std::string::npos);
    EXPECT_NE(home.find(note), std::string::npos);
    EXPECT_NE(home.find("href=\"/sign-in\""), std::string::npos);
    EXPECT_NE(lake::api::web_ui_docs("concepts", visitor)->find("href=\"/sign-in\""),
              std::string::npos);
    EXPECT_EQ(lake::api::web_ui_docs("concepts", visitor)->find("href=\"/sign-up\""),
              std::string::npos)
        << "every page's top bar follows the same rule";

    visitor.clerk = false;
    EXPECT_NE(lake::api::web_ui_home(visitor).find("href=\"/sign-up\">Create account"),
              std::string::npos)
        << "without Clerk, password sign-up is open";
}

TEST(Clerk, AccountMenuOmitsLakePasskeysUnderClerk) {
    lake::api::WebUiViewer viewer{true};
    EXPECT_NE(lake::api::web_ui_home(viewer).find("href=\"/passkeys\""), std::string::npos);
    viewer.clerk = true;
    const std::string home = lake::api::web_ui_home(viewer);
    EXPECT_EQ(home.find("href=\"/passkeys\""), std::string::npos);
    EXPECT_NE(home.find("href=\"/tokens\""), std::string::npos);
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

TEST(Clerk, AccountsSeeTheirOrganizationSectionAndOnlyTheirOwnActivity) {
    const std::string account = lake::api::web_ui_home(true);
    for (const std::string_view link : {"<p class=\"nav-label\">Organization</p>",
                                        "href=\"/organizations\"",
                                        "href=\"/secret-mounts\"",
                                        "href=\"/metering\"",
                                        "href=\"/activity\""}) {
        EXPECT_NE(account.find(link), std::string::npos) << link;
    }
    EXPECT_EQ(lake::api::web_ui_home({true, true}).find("href=\"/activity\""), std::string::npos)
        << "operators keep the instance audit under Administration";

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
    const auto mine = registry.call("my_audit", {{"limit", 100}}, a_again);
    ASSERT_TRUE(mine.ok) << mine.error.message;
    ASSERT_FALSE(mine.value["items"].empty());
    for (const json& event : mine.value["items"]) {
        EXPECT_EQ(event.value("principal", ""), a->taap_id);
    }
    auto b_context = as(*b);
    const auto theirs =
        registry.call("my_audit", {{"principal", a->taap_id}, {"limit", 100}}, b_context);
    ASSERT_TRUE(theirs.ok);
    for (const json& event : theirs.value["items"]) {
        EXPECT_EQ(event.value("principal", ""), b->taap_id) << "a principal filter cannot widen it";
    }
    auto b_audit = as(*b);
    EXPECT_FALSE(registry.call("audit", {{"limit", 10}}, b_audit).ok)
        << "the instance audit log stays operator-only";
}
