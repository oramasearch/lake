// SPDX-License-Identifier: MIT

// OpenID Connect discovery for Lake OAuth and enterprise OIDC (lake#837,
// docs/api/identity.md#openid-connect-discovery): the document's checks, its cache, and sign-in end
// to end against local identity providers shaped like Microsoft Entra and Okta, whose endpoints are
// not under `<issuer>/`, with valid, tampered, replayed, wrong-audience and expired ID tokens.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "lake/api/oauth.hpp"
#include "lake/api/server.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"

namespace {

using nlohmann::json;

std::string base64url(const unsigned char* bytes, std::size_t count) {
    std::string encoded(((count + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), bytes, static_cast<int>(count));
    encoded.resize(static_cast<std::size_t>(std::max(size, 0)));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::ranges::replace(encoded, '+', '-');
    std::ranges::replace(encoded, '/', '_');
    return encoded;
}

std::string base64url(std::string_view value) {
    return base64url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::string url_decode(std::string_view value) {
    std::string decoded;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            decoded +=
                static_cast<char>(std::stoi(std::string(value.substr(index + 1, 2)), nullptr, 16));
            index += 2;
        } else {
            decoded += value[index] == '+' ? ' ' : value[index];
        }
    }
    return decoded;
}

std::map<std::string, std::string> parameters(std::string_view encoded) {
    std::map<std::string, std::string> result;
    std::size_t position = 0;
    while (position <= encoded.size()) {
        const std::size_t ampersand = encoded.find('&', position);
        const std::string_view pair = encoded.substr(
            position,
            ampersand == std::string_view::npos ? std::string_view::npos : ampersand - position);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos) {
            result[url_decode(pair.substr(0, equals))] = url_decode(pair.substr(equals + 1));
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        position = ampersand + 1;
    }
    return result;
}

std::string http(unsigned short port,
                 std::string_view method,
                 std::string_view target,
                 std::string_view body = {},
                 std::string_view content_type = {},
                 std::string_view cookie = {}) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (descriptor < 0 ||
        ::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        return {};
    }
    std::string message = std::string(method) + " " + std::string(target) +
                          " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!content_type.empty()) {
        message += "Content-Type: " + std::string(content_type) + "\r\n";
    }
    if (!cookie.empty()) {
        message += "Cookie: " + std::string(cookie) + "\r\n";
    }
    message += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    message += body;
    for (std::size_t sent = 0; sent < message.size();) {
        const ssize_t count = ::send(descriptor, message.data() + sent, message.size() - sent, 0);
        if (count <= 0) {
            ::close(descriptor);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (ssize_t count; (count = ::recv(descriptor, buffer, sizeof(buffer), 0)) > 0;) {
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    return response;
}

std::string header(const std::string& response, std::string_view name) {
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const std::size_t at = response.find(prefix);
    if (at == std::string::npos || at > response.find("\r\n\r\n")) {
        return {};
    }
    const std::size_t start = at + prefix.size();
    return response.substr(start, response.find("\r\n", start) - start);
}

std::string body_of(const std::string& response) {
    const std::size_t at = response.find("\r\n\r\n");
    return at == std::string::npos ? std::string() : response.substr(at + 4);
}

std::string path_of(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    return path == std::string::npos ? std::string("/") : url.substr(path);
}

unsigned short free_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    unsigned short port = 0;
    if (descriptor >= 0 &&
        ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
        port = ntohs(address.sin_port);
    }
    if (descriptor >= 0) {
        ::close(descriptor);
    }
    return port;
}

// How the identity provider lays out its endpoints.
enum class Shape { entra, okta };

// What the next ID token is.
enum class Token { valid, tampered, wrong_audience, expired, wrong_nonce };

// A local identity provider shaped like Entra (issuer `<base>/<tenant>/v2.0`, endpoints under
// `<base>/<tenant>/oauth2/v2.0/` and keys under `<base>/<tenant>/discovery/v2.0/keys`) or Okta
// (issuer
// `<base>/oauth2/default`, endpoints `/v1/authorize`, `/v1/token`, `/v1/keys` beneath it).
class Provider {
public:
    explicit Provider(Shape shape) : shape_(shape) {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(context);
        EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048);
        EVP_PKEY_keygen(context, &key_);
        EVP_PKEY_CTX_free(context);
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (listener_ >= 0 &&
            ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            ::listen(listener_, 16) == 0 &&
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
            port_ = ntohs(address.sin_port);
            worker_ = std::thread([this] { serve(); });
        }
    }
    ~Provider() {
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
            listener_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        EVP_PKEY_free(key_);
    }

    std::string base() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    std::string issuer() const {
        return shape_ == Shape::entra ? base() + "/7d2c-tenant/v2.0" : base() + "/oauth2/default";
    }
    std::string authorize_path() const {
        return shape_ == Shape::entra ? "/7d2c-tenant/oauth2/v2.0/authorize"
                                      : "/oauth2/default/v1/authorize";
    }
    std::string token_path() const {
        return shape_ == Shape::entra ? "/7d2c-tenant/oauth2/v2.0/token"
                                      : "/oauth2/default/v1/token";
    }
    std::string keys_path() const {
        return shape_ == Shape::entra ? "/7d2c-tenant/discovery/v2.0/keys"
                                      : "/oauth2/default/v1/keys";
    }
    std::string discovery_path() const {
        return path_of(issuer()) + "/.well-known/openid-configuration";
    }
    unsigned short port() const {
        return port_;
    }
    void set_token(Token token) {
        token_.store(token);
    }
    void set_discovery_issuer(std::string issuer) {
        std::lock_guard lock(mutex_);
        discovery_issuer_ = std::move(issuer);
    }
    int requests(const std::string& path) const {
        std::lock_guard lock(mutex_);
        const auto found = requests_.find(path);
        return found == requests_.end() ? 0 : found->second;
    }
    int total_requests() const {
        std::lock_guard lock(mutex_);
        int total = 0;
        for (const auto& [path, count] : requests_) {
            total += count;
        }
        return total;
    }

private:
    std::string jwks() const {
        BIGNUM* modulus = nullptr;
        BIGNUM* exponent = nullptr;
        EVP_PKEY_get_bn_param(key_, OSSL_PKEY_PARAM_RSA_N, &modulus);
        EVP_PKEY_get_bn_param(key_, OSSL_PKEY_PARAM_RSA_E, &exponent);
        std::vector<unsigned char> n(BN_num_bytes(modulus));
        std::vector<unsigned char> e(BN_num_bytes(exponent));
        BN_bn2bin(modulus, n.data());
        BN_bn2bin(exponent, e.data());
        BN_free(modulus);
        BN_free(exponent);
        return json{{"keys",
                     json::array({{{"kty", "RSA"},
                                   {"use", "sig"},
                                   {"kid", "provider-key"},
                                   {"n", base64url(n.data(), n.size())},
                                   {"e", base64url(e.data(), e.size())}}})}}
            .dump();
    }

    std::string id_token(const std::string& nonce) const {
        const long now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const Token token = token_.load();
        const std::string header =
            base64url(json{{"alg", "RS256"}, {"kid", "provider-key"}, {"typ", "JWT"}}.dump());
        const std::string payload =
            base64url(json{{"iss", issuer()},
                           {"aud", token == Token::wrong_audience ? "someone-else" : "lake-client"},
                           {"sub", shape_ == Shape::entra ? "entra-oid-1" : "okta-uid-1"},
                           {"email", "person@example.com"},
                           {"name", "Person"},
                           {"nonce", token == Token::wrong_nonce ? "another-nonce" : nonce},
                           {"iat", now - (token == Token::expired ? 7200 : 0)},
                           {"exp", token == Token::expired ? now - 3600 : now + 300}}
                          .dump());
        const std::string input = header + "." + payload;
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        std::size_t size = 0;
        EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key_);
        EVP_DigestSignUpdate(context, input.data(), input.size());
        EVP_DigestSignFinal(context, nullptr, &size);
        std::vector<unsigned char> signature(size);
        EVP_DigestSignFinal(context, signature.data(), &size);
        EVP_MD_CTX_free(context);
        signature.resize(size);
        if (token == Token::tampered) {
            signature[signature.size() / 2] ^= 0x01;
        }
        return input + "." + base64url(signature.data(), signature.size());
    }

    void send(int descriptor, int status, std::string_view body, std::string_view location = {}) {
        std::string response = "HTTP/1.1 " + std::to_string(status) +
                               (status == 200   ? " OK"
                                : status == 302 ? " Found"
                                                : " Not Found") +
                               "\r\nConnection: close\r\nContent-Type: application/json\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) + "\r\n";
        if (!location.empty()) {
            response += "Location: " + std::string(location) + "\r\n";
        }
        response += "\r\n";
        response += body;
        ::send(descriptor, response.data(), response.size(), 0);
    }

    void handle(int descriptor) {
        std::string text;
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return;
            }
            text.append(buffer, static_cast<std::size_t>(count));
            const std::size_t end = text.find("\r\n\r\n");
            if (end == std::string::npos) {
                continue;
            }
            std::size_t length = 0;
            if (const std::size_t at = text.find("Content-Length: ");
                at != std::string::npos && at < end) {
                length = std::stoul(text.substr(at + 16));
            }
            if (text.size() >= end + 4 + length) {
                break;
            }
        }
        const std::size_t target_start = text.find(' ') + 1;
        const std::string target =
            text.substr(target_start, text.find(' ', target_start) - target_start);
        const std::string path = target.substr(0, target.find('?'));
        const std::string body = text.substr(text.find("\r\n\r\n") + 4);
        {
            std::lock_guard lock(mutex_);
            ++requests_[path];
        }
        if (path == discovery_path()) {
            std::string document_issuer;
            {
                std::lock_guard lock(mutex_);
                document_issuer = discovery_issuer_.empty() ? issuer() : discovery_issuer_;
            }
            send(descriptor,
                 200,
                 json{{"issuer", document_issuer},
                      {"authorization_endpoint", base() + authorize_path()},
                      {"token_endpoint", base() + token_path()},
                      {"jwks_uri", base() + keys_path()},
                      {"response_types_supported", json::array({"code"})},
                      {"subject_types_supported", json::array({"pairwise"})},
                      {"id_token_signing_alg_values_supported", json::array({"RS256"})}}
                     .dump());
        } else if (path == authorize_path()) {
            const auto query = parameters(target.substr(target.find('?') + 1));
            {
                std::lock_guard lock(mutex_);
                nonce_ = query.contains("nonce") ? query.at("nonce") : std::string();
            }
            send(descriptor,
                 302,
                 "",
                 query.at("redirect_uri") + "?code=provider-code&state=" + query.at("state"));
        } else if (path == token_path()) {
            const auto form = parameters(body);
            std::string nonce;
            {
                std::lock_guard lock(mutex_);
                nonce = nonce_;
            }
            if (form.contains("code_verifier") && form.at("client_id") == "lake-client") {
                send(descriptor,
                     200,
                     json{{"token_type", "Bearer"}, {"id_token", id_token(nonce)}}.dump());
            } else {
                send(descriptor, 404, "{}");
            }
        } else if (path == keys_path()) {
            send(descriptor, 200, jwks());
        } else {
            send(descriptor, 404, "{}");
        }
    }

    void serve() {
        while (listener_ >= 0) {
            const int descriptor = ::accept(listener_, nullptr, nullptr);
            if (descriptor < 0) {
                return;
            }
            handle(descriptor);
            ::close(descriptor);
        }
    }

    Shape shape_;
    EVP_PKEY* key_ = nullptr;
    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::map<std::string, int> requests_;
    std::string nonce_;
    std::string discovery_issuer_;
    std::atomic<Token> token_{Token::valid};
};

// A Lake with one enterprise OIDC provider for `provider`.
struct OidcLake {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    unsigned short port = free_port();
    std::string provider_id;
    std::string browser_cookie;
    std::unique_ptr<lake::api::ApiServer> server;

    OidcLake(const Provider& provider, bool allow_private_addresses = true) {
        const auto organization = store->create_organization({"", "OIDC organization"});
        EXPECT_TRUE(organization) << organization.error().message;
        const auto sso = store->register_sso_provider({organization->id,
                                                       "lake",
                                                       "corporate-oidc",
                                                       "oidc",
                                                       provider.issuer(),
                                                       "",
                                                       "",
                                                       "lake-client",
                                                       "client-secret",
                                                       "scim-oidc-token",
                                                       json::object(),
                                                       true});
        EXPECT_TRUE(sso) << sso.error().message;
        provider_id = sso->id;
        EXPECT_TRUE(
            store->create_federation_issuer({organization->id, provider.issuer(), "email"}));
        lake::api::ServerOptions options;
        options.port = port;
        options.public_host = "127.0.0.1:" + std::to_string(port);
        options.store = store;
        options.oidc_allow_private_addresses = allow_private_addresses;
        server = std::make_unique<lake::api::ApiServer>(options);
        EXPECT_TRUE(server->start()) << server->reason();
    }
    ~OidcLake() {
        server->stop();
    }

    std::string start() {
        return http(
            port,
            "POST",
            "/v1/start_sso_oidc",
            json{{"provider_id", provider_id}, {"taap_id", "taap:1:ed25519:oidc-person"}}.dump(),
            "application/json");
    }

    std::string callback(const std::string& target) {
        return http(port, "GET", target, {}, {}, browser_cookie);
    }

    // Starts sign-in and follows the provider's redirect: answers Lake's callback target.
    std::string authorize(const Provider& provider) {
        const std::string started = start();
        browser_cookie = header(started, "Set-Cookie");
        browser_cookie = browser_cookie.substr(0, browser_cookie.find(';'));
        EXPECT_NE(started.find(" 200 "), std::string::npos) << started;
        const std::string url = json::parse(body_of(started))["value"]["authorization_url"];
        EXPECT_TRUE(url.starts_with(provider.base() + provider.authorize_path() + "?")) << url;
        const std::string redirected = http(provider.port(), "GET", path_of(url));
        EXPECT_NE(redirected.find(" 302 "), std::string::npos) << redirected;
        return path_of(header(redirected, "Location"));
    }
};

} // namespace

TEST(OidcDiscovery, ReadsEntraAndOktaShapedDocumentsAndRefusesBadOnes) {
    const std::string entra_issuer = "https://login.microsoftonline.com/7d2c/v2.0";
    const json entra{
        {"issuer", entra_issuer},
        {"authorization_endpoint", "https://login.microsoftonline.com/7d2c/oauth2/v2.0/authorize"},
        {"token_endpoint", "https://login.microsoftonline.com/7d2c/oauth2/v2.0/token"},
        {"jwks_uri", "https://login.microsoftonline.com/7d2c/discovery/v2.0/keys"},
        {"id_token_signing_alg_values_supported", json::array({"RS256"})}};
    const auto read = lake::api::parse_oidc_discovery(entra.dump(), entra_issuer, false);
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_EQ(read->authorization, "https://login.microsoftonline.com/7d2c/oauth2/v2.0/authorize");
    EXPECT_EQ(read->token, "https://login.microsoftonline.com/7d2c/oauth2/v2.0/token");
    EXPECT_EQ(read->jwks, "https://login.microsoftonline.com/7d2c/discovery/v2.0/keys");
    EXPECT_EQ(lake::api::oidc_discovery_url(entra_issuer),
              "https://login.microsoftonline.com/7d2c/v2.0/.well-known/openid-configuration");
    EXPECT_EQ(lake::api::oidc_discovery_url("https://acme.okta.com/oauth2/default/"),
              "https://acme.okta.com/oauth2/default/.well-known/openid-configuration");

    const json okta{{"issuer", "https://acme.okta.com"},
                    {"authorization_endpoint", "https://acme.okta.com/oauth2/v1/authorize"},
                    {"token_endpoint", "https://acme.okta.com/oauth2/v1/token"},
                    {"jwks_uri", "https://acme.okta.com/oauth2/v1/keys"}};
    EXPECT_TRUE(
        lake::api::parse_oidc_discovery(okta.dump(), "https://acme.okta.com", false).has_value());

    // The issuer must be the configured one exactly, trailing slash included.
    EXPECT_FALSE(
        lake::api::parse_oidc_discovery(okta.dump(), "https://acme.okta.com/", false).has_value());
    EXPECT_FALSE(
        lake::api::parse_oidc_discovery(okta.dump(), "https://evil.example", false).has_value());
    for (const char* field : {"authorization_endpoint", "token_endpoint", "jwks_uri"}) {
        json missing = okta;
        missing.erase(field);
        EXPECT_FALSE(lake::api::parse_oidc_discovery(missing.dump(), "https://acme.okta.com", false)
                         .has_value())
            << field;
        json plain = okta;
        plain[field] = "http://acme.okta.com/x";
        EXPECT_FALSE(lake::api::parse_oidc_discovery(plain.dump(), "https://acme.okta.com", false)
                         .has_value())
            << field;
    }
    json loopback = okta;
    loopback["issuer"] = "http://127.0.0.1:9";
    loopback["jwks_uri"] = "http://127.0.0.1:9/keys";
    loopback["token_endpoint"] = "http://127.0.0.1:9/token";
    loopback["authorization_endpoint"] = "http://127.0.0.1:9/authorize";
    EXPECT_FALSE(
        lake::api::parse_oidc_discovery(loopback.dump(), "http://127.0.0.1:9", false).has_value());
    EXPECT_TRUE(
        lake::api::parse_oidc_discovery(loopback.dump(), "http://127.0.0.1:9", true).has_value());
    json no_rs256 = okta;
    no_rs256["id_token_signing_alg_values_supported"] = json::array({"ES256"});
    EXPECT_FALSE(lake::api::parse_oidc_discovery(no_rs256.dump(), "https://acme.okta.com", false)
                     .has_value());
    EXPECT_FALSE(
        lake::api::parse_oidc_discovery("not json", "https://acme.okta.com", false).has_value());
}

TEST(OidcDiscovery, CachesEachIssuerUntilItExpires) {
    std::atomic<int> fetches{0};
    const std::string issuer = "https://idp.example";
    const auto fetch = [&](const std::string& url) {
        EXPECT_EQ(url, issuer + "/.well-known/openid-configuration");
        fetches.fetch_add(1);
        return lake::api::OidcFetchResponse{200,
                                            json{{"issuer", issuer},
                                                 {"authorization_endpoint", issuer + "/a"},
                                                 {"token_endpoint", issuer + "/t"},
                                                 {"jwks_uri", issuer + "/k"}}
                                                .dump(),
                                            ""};
    };
    lake::api::OidcDiscovery cached(fetch, false);
    ASSERT_TRUE(cached.endpoints(issuer).has_value());
    ASSERT_TRUE(cached.endpoints(issuer).has_value());
    EXPECT_EQ(fetches.load(), 1);
    ASSERT_TRUE(cached.endpoints(issuer, true).has_value());
    EXPECT_EQ(fetches.load(), 2);
    lake::api::OidcDiscovery expiring(fetch, false, std::chrono::seconds(0));
    ASSERT_TRUE(expiring.endpoints(issuer).has_value());
    ASSERT_TRUE(expiring.endpoints(issuer).has_value());
    EXPECT_EQ(fetches.load(), 4);
    // A failed fetch is not cached, and says why.
    lake::api::OidcDiscovery failing(
        [](const std::string&) { return lake::api::OidcFetchResponse{503, "", ""}; }, false);
    const auto failed = failing.endpoints(issuer);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, "engine_error");
    // A plain-HTTP issuer is refused before anything is fetched.
    const auto plain = cached.endpoints("http://idp.example");
    ASSERT_FALSE(plain.has_value());
    EXPECT_EQ(plain.error().code, "invalid_input");
}

class OidcSignIn : public ::testing::TestWithParam<Shape> {};

TEST_P(OidcSignIn, AValidTokenSignsInOnceThroughTheDiscoveredEndpoints) {
    Provider provider(GetParam());
    OidcLake lake(provider);
    const std::string callback = lake.authorize(provider);
    const std::string signed_in = lake.callback(callback);
    ASSERT_NE(signed_in.find(" 302 "), std::string::npos) << signed_in;
    const std::string cookie = header(signed_in, "Set-Cookie");
    ASSERT_TRUE(cookie.starts_with("lake_session=")) << cookie;
    EXPECT_TRUE(lake.store->context_for(cookie.substr(13, cookie.find(';') - 13)).has_value());
    // Discovery, the token endpoint and the keys, each at the provider's own path.
    EXPECT_EQ(provider.requests(provider.discovery_path()), 1);
    EXPECT_EQ(provider.requests(provider.token_path()), 1);
    EXPECT_EQ(provider.requests(provider.keys_path()), 1);
    // Nothing was asked of `<issuer>/authorize`, `<issuer>/token` or `<issuer>/jwks`.
    const std::string under_issuer = path_of(provider.issuer());
    EXPECT_EQ(provider.requests(under_issuer + "/token"), 0);
    EXPECT_EQ(provider.requests(under_issuer + "/jwks"), 0);

    // Replayed: the state is used.
    const std::string replayed = lake.callback(callback);
    EXPECT_NE(replayed.find(" 403 "), std::string::npos) << replayed;

    // A second sign-in reads the cached document.
    const std::string again = lake.callback(lake.authorize(provider));
    EXPECT_NE(again.find(" 302 "), std::string::npos) << again;
    EXPECT_EQ(provider.requests(provider.discovery_path()), 1);
}

TEST_P(OidcSignIn, TamperedWrongAudienceExpiredAndWrongNonceTokensAreRefused) {
    Provider provider(GetParam());
    OidcLake lake(provider);
    for (const Token token :
         {Token::tampered, Token::wrong_audience, Token::expired, Token::wrong_nonce}) {
        provider.set_token(token);
        const std::string refused = lake.callback(lake.authorize(provider));
        EXPECT_NE(refused.find(" 403 "), std::string::npos) << static_cast<int>(token) << refused;
        EXPECT_EQ(header(refused, "Set-Cookie"), "") << static_cast<int>(token);
    }
}

TEST_P(OidcSignIn, ADocumentNamingAnotherIssuerIsRefused) {
    Provider provider(GetParam());
    provider.set_discovery_issuer("https://login.attacker.example/v2.0");
    OidcLake lake(provider);
    const std::string started = lake.start();
    EXPECT_NE(started.find(" 400 "), std::string::npos) << started;
    EXPECT_NE(body_of(started).find("is not the configured issuer"), std::string::npos) << started;
    EXPECT_EQ(provider.requests(provider.authorize_path()), 0);
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, OidcSignIn, ::testing::Values(Shape::entra, Shape::okta), [](const auto& info) {
        return info.param == Shape::entra ? std::string("Entra") : std::string("Okta");
    });

TEST(OidcSignInNetwork, PrivateAddressesAreRefusedUnlessAllowed) {
    // Without LAKE_OIDC_ALLOW_PRIVATE_ADDRESSES, a loopback issuer is never contacted: plain HTTP
    // is refused, and HTTPS to a private address is refused at connect time (lake#876).
    Provider provider(Shape::okta);
    OidcLake lake(provider, false);
    const std::string started = lake.start();
    EXPECT_NE(started.find(" 400 "), std::string::npos) << started;
    EXPECT_EQ(provider.total_requests(), 0);

    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto organization = store->create_organization({"", "Private OIDC organization"});
    ASSERT_TRUE(organization);
    const auto sso =
        store->register_sso_provider({organization->id,
                                      "lake",
                                      "private-oidc",
                                      "oidc",
                                      "https://127.0.0.1:" + std::to_string(provider.port()),
                                      "",
                                      "",
                                      "lake-client",
                                      "client-secret",
                                      "scim-private-token",
                                      json::object(),
                                      true});
    ASSERT_TRUE(sso) << sso.error().message;
    const unsigned short port = free_port();
    lake::api::ServerOptions options;
    options.port = port;
    options.public_host = "127.0.0.1:" + std::to_string(port);
    options.store = store;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string private_https = http(port,
                                           "POST",
                                           "/v1/start_sso_oidc",
                                           json{{"provider_id", sso->id}}.dump(),
                                           "application/json");
    EXPECT_EQ(private_https.find(" 200 "), std::string::npos) << private_https;
    server.stop();
    EXPECT_EQ(provider.total_requests(), 0);
}

TEST(OidcSignInLakeOAuth, AGenericLakeOAuthProviderUsesDiscoveryToo) {
    Provider provider(Shape::entra);
    auto store = std::make_shared<lake::control::InMemoryStore>();
    ASSERT_TRUE(store->register_oauth_provider({"lake",
                                                "entra",
                                                provider.issuer(),
                                                "lake-client",
                                                "client-secret",
                                                {"openid", "email", "profile"}}));
    const unsigned short port = free_port();
    lake::api::ServerOptions options;
    options.port = port;
    options.public_host = "127.0.0.1:" + std::to_string(port);
    options.store = store;
    options.oidc_allow_private_addresses = true;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string started = http(port,
                                     "POST",
                                     "/v1/start_oauth",
                                     R"({"provider":"entra","identity_document":"{}"})",
                                     "application/json");
    ASSERT_NE(started.find(" 200 "), std::string::npos) << started;
    const std::string url = json::parse(body_of(started))["value"]["authorization_url"];
    EXPECT_TRUE(url.starts_with(provider.base() + provider.authorize_path() + "?")) << url;
    const std::string redirected = http(provider.port(), "GET", path_of(url));
    ASSERT_NE(redirected.find(" 302 "), std::string::npos) << redirected;
    // The token exchange and the key fetch go to the discovered endpoints; the identity document
    // here is not a TAAP one, so the sign-in itself is refused after verification, as for any
    // first OAuth sign-in without one (docs/api/clerk.md).
    const std::string cookie = header(started, "Set-Cookie");
    const std::string callback = http(port,
                                      "GET",
                                      path_of(header(redirected, "Location")),
                                      {},
                                      {},
                                      cookie.substr(0, cookie.find(';')));
    EXPECT_EQ(provider.requests(provider.token_path()), 1) << callback;
    EXPECT_EQ(provider.requests(provider.keys_path()), 1) << callback;
    server.stop();
}
