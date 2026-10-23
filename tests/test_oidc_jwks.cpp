#include "lake/api/oidc_jwks.hpp"

#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <thread>

namespace {
using Json = nlohmann::json;
using lake::api::OidcJwks;
using lake::control::identity_crypto::base64url_encode;

struct SigningKey {
    EVP_PKEY* key = nullptr;
    SigningKey() {
        auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (context && EVP_PKEY_keygen_init(context) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1) {
            EVP_PKEY_keygen(context, &key);
        }
        EVP_PKEY_CTX_free(context);
    }
    ~SigningKey() {
        EVP_PKEY_free(key);
    }
    std::string jwks(std::string kid = "known") const {
        const auto number = [&](const char* name) {
            BIGNUM* value = nullptr;
            EVP_PKEY_get_bn_param(key, name, &value);
            std::string bytes(BN_num_bytes(value), '\0');
            BN_bn2bin(value, reinterpret_cast<unsigned char*>(bytes.data()));
            BN_free(value);
            return base64url_encode(bytes);
        };
        return Json{{"keys",
                     Json::array({{{"kid", kid},
                                   {"kty", "RSA"},
                                   {"n", number(OSSL_PKEY_PARAM_RSA_N)},
                                   {"e", number(OSSL_PKEY_PARAM_RSA_E)}}})}}
            .dump();
    }
    std::string token(std::string kid = "known", bool forged = false) const {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto input =
            base64url_encode(Json{{"alg", "RS256"}, {"kid", kid}}.dump()) + "." +
            base64url_encode(
                Json{{"iss", "issuer"}, {"aud", "lake"}, {"sub", "user"}, {"exp", now + 300}}
                    .dump());
        auto* context = EVP_MD_CTX_new();
        EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key);
        EVP_DigestSignUpdate(context, input.data(), input.size());
        std::size_t size = 0;
        EVP_DigestSignFinal(context, nullptr, &size);
        std::string signature(size, '\0');
        EVP_DigestSignFinal(context, reinterpret_cast<unsigned char*>(signature.data()), &size);
        EVP_MD_CTX_free(context);
        signature.resize(size);
        if (forged) {
            signature[0] ^= 1;
        }
        return input + "." + base64url_encode(signature);
    }
};

TEST(OidcJwks, InvalidTokensCannotForceRefreshAndRotationRecoversAfterCooldown) {
    SigningKey key;
    ASSERT_NE(key.key, nullptr);
    auto now = OidcJwks::Clock::now();
    OidcJwks cache([&] { return now; });
    unsigned fetches = 0;
    std::string document = key.jwks();
    const OidcJwks::Fetch fetch = [&](const auto&, bool) {
        ++fetches;
        return std::optional(document);
    };
    const auto verify = [&](const std::string& token) {
        return cache.verify(token, "keys", "issuer", "lake", "", false, fetch);
    };
    const auto encoded = [&](const Json& header, const Json& claims) {
        return base64url_encode(header.dump()) + "." + base64url_encode(claims.dump()) + ".AA";
    };
    const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count() +
                         300;
    const Json header{{"alg", "RS256"}, {"kid", "known"}};
    const Json claims{{"iss", "issuer"}, {"aud", "lake"}, {"sub", "user"}, {"exp", expires}};
    for (unsigned i = 0; i < 100; ++i) {
        EXPECT_FALSE(verify(encoded({{"alg", "RS256"}}, claims)));
        EXPECT_FALSE(verify(encoded({{"alg", "none"}, {"kid", "known"}}, claims)));
        auto expired = claims;
        expired["exp"] = 1;
        EXPECT_FALSE(verify(encoded(header, expired)));
        auto malformed = claims;
        malformed["exp"] = "future";
        EXPECT_FALSE(verify(encoded(header, malformed)));
        malformed["exp"] = Json::array();
        EXPECT_FALSE(verify(encoded(header, malformed)));
        EXPECT_FALSE(verify(encoded(Json::array(), claims)));
        EXPECT_FALSE(verify("not.a.jwt"));
        EXPECT_FALSE(verify(base64url_encode("[]") + ".e30.AA"));
    }
    auto mixed_audience = claims;
    mixed_audience["aud"] = Json::array({"lake", 1});
    mixed_audience["azp"] = "lake";
    EXPECT_FALSE(verify(encoded(header, mixed_audience)));
    EXPECT_FALSE(
        cache.verify(encoded(header, claims), "keys", "issuer", "lake", "", false, fetch, true));
    EXPECT_EQ(fetches, 0U);
    ASSERT_TRUE(verify(key.token()));
    EXPECT_EQ(fetches, 1U);
    const auto forged = key.token("known", true);
    const auto unknown = key.token("rotated", true);
    for (unsigned i = 0; i < 100; ++i) {
        EXPECT_FALSE(verify(forged));
        EXPECT_FALSE(verify(unknown));
    }
    EXPECT_EQ(fetches, 1U);
    now += std::chrono::seconds(59);
    EXPECT_FALSE(verify(unknown));
    EXPECT_EQ(fetches, 1U);
    now += std::chrono::seconds(1);
    for (unsigned i = 0; i < 100; ++i) {
        EXPECT_FALSE(verify(key.token("unknown-" + std::to_string(i), true)));
    }
    EXPECT_EQ(fetches, 2U);
    document = key.jwks("rotated");
    now += std::chrono::seconds(60);
    ASSERT_TRUE(verify(key.token("rotated")));
    EXPECT_EQ(fetches, 3U);
    // A bad signature with a known key must not refresh even after the normal cache TTL.
    now += std::chrono::hours(2);
    EXPECT_FALSE(verify(key.token("rotated", true)));
    EXPECT_EQ(fetches, 3U);
    EXPECT_TRUE(verify(key.token("rotated")));
    EXPECT_EQ(fetches, 4U);
}

TEST(OidcJwks, FetchIsSingleFlightAndFailuresAreCooledDown) {
    SigningKey key;
    ASSERT_NE(key.key, nullptr);
    auto now = OidcJwks::Clock::now();
    OidcJwks cache([&] { return now; });
    std::atomic<unsigned> fetches = 0;
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    const OidcJwks::Fetch fetch = [&](const auto&, bool) -> std::optional<std::string> {
        ++fetches;
        entered.set_value();
        released.wait();
        return std::nullopt;
    };
    const auto token = key.token();
    auto first = std::async(std::launch::async, [&] {
        return cache.verify(token, "keys", "issuer", "lake", "", false, fetch);
    });
    entered.get_future().wait();
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(cache.verify(token, "keys", "issuer", "lake", "", false, fetch));
    }
    release.set_value();
    EXPECT_FALSE(first.get());
    EXPECT_FALSE(cache.verify(token, "keys", "issuer", "lake", "", false, fetch));
    EXPECT_EQ(fetches, 1U);
    now += std::chrono::seconds(60);
    EXPECT_FALSE(cache.verify(token,
                              "keys",
                              "issuer",
                              "lake",
                              "",
                              false,
                              [&](const auto&, bool) -> std::optional<std::string> {
                                  ++fetches;
                                  throw 1;
                              }));
    EXPECT_FALSE(cache.verify(token, "keys", "issuer", "lake", "", false, fetch));
    EXPECT_EQ(fetches, 2U);
    now += std::chrono::seconds(60);
    EXPECT_FALSE(cache.verify(token,
                              "keys",
                              "issuer",
                              "lake",
                              "",
                              false,
                              [&](const auto&, bool) -> std::optional<std::string> {
                                  ++fetches;
                                  return std::string(18, '[') + "0" + std::string(18, ']');
                              }));
    EXPECT_FALSE(cache.verify(token, "keys", "issuer", "lake", "", false, fetch));
    EXPECT_EQ(fetches, 3U);
    now += std::chrono::seconds(60);
    EXPECT_TRUE(cache.verify(token,
                             "keys",
                             "issuer",
                             "lake",
                             "",
                             false,
                             [&](const auto&, bool) -> std::optional<std::string> {
                                 ++fetches;
                                 return key.jwks();
                             }));
    EXPECT_EQ(fetches, 4U);
}
} // namespace
