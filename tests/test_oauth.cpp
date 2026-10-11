#include "audit_test_support.hpp"
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

#include "lake/api/server.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"

namespace {

using nlohmann::json;

std::string base64url(const unsigned char* bytes, std::size_t count) {
    std::string encoded(((count + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), bytes, static_cast<int>(count));
    if (size < 0) {
        return {};
    }
    encoded.resize(static_cast<std::size_t>(size));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::string base64url(std::string_view value) {
    return base64url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string url_decode(std::string_view value) {
    std::string decoded;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded += ' ';
        } else if (value[index] == '%' && index + 2 < value.size() &&
                   hex_value(value[index + 1]) >= 0 && hex_value(value[index + 2]) >= 0) {
            decoded +=
                static_cast<char>((hex_value(value[index + 1]) << 4) | hex_value(value[index + 2]));
            index += 2;
        } else {
            decoded += value[index];
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

std::string response_body(const std::string& response) {
    const std::size_t start = response.find("\r\n\r\n");
    return start == std::string::npos ? std::string() : response.substr(start + 4);
}

std::string response_header(const std::string& response, std::string_view name) {
    const std::string prefix = std::string(name) + ": ";
    std::size_t position = response.find("\r\n") + 2;
    while (position < response.size()) {
        const std::size_t end = response.find("\r\n", position);
        if (end == position || end == std::string::npos) {
            break;
        }
        const std::string_view line(response.data() + position, end - position);
        if (line.starts_with(prefix)) {
            return std::string(line.substr(prefix.size()));
        }
        position = end + 2;
    }
    return {};
}

std::string request(unsigned short port,
                    std::string_view method,
                    std::string_view target,
                    std::string_view body = {},
                    std::string_view content_type = {}) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return {};
    }
    std::string message = std::string(method) + " " + std::string(target) +
                          " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!content_type.empty()) {
        message += "Content-Type: " + std::string(content_type) + "\r\n";
    }
    message += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    message += body;
    std::size_t sent = 0;
    while (sent < message.size()) {
        const ssize_t count = ::send(descriptor, message.data() + sent, message.size() - sent, 0);
        if (count <= 0) {
            ::close(descriptor);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    return response;
}

unsigned short unused_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (descriptor < 0 ||
        ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        return 0;
    }
    socklen_t size = sizeof(address);
    ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size);
    const unsigned short port = ntohs(address.sin_port);
    ::close(descriptor);
    return port;
}

class StubProvider {
public:
    StubProvider() {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (context != nullptr && EVP_PKEY_keygen_init(context) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1) {
            EVP_PKEY_keygen(context, &key_);
        }
        EVP_PKEY_CTX_free(context);

        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (listener_ >= 0 &&
            ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            ::listen(listener_, 8) == 0) {
            socklen_t size = sizeof(address);
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
            port_ = ntohs(address.sin_port);
            worker_ = std::thread([this] { serve(); });
        }
    }

    ~StubProvider() {
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

    std::string issuer() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    int jwks_requests() const {
        return jwks_requests_.load();
    }
    bool pkce_verified() const {
        return pkce_verified_.load();
    }

private:
    std::string sign_token(std::string_view nonce) const {
        const long now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const std::string header =
            base64url(json{{"alg", "RS256"}, {"kid", "stub-key"}, {"typ", "JWT"}}.dump());
        const std::string payload = base64url(json{{"iss", issuer()},
                                                   {"aud", "lake-test"},
                                                   {"sub", "stub-subject"},
                                                   {"email", "oauth@example.com"},
                                                   {"name", "OAuth User"},
                                                   {"nonce", nonce},
                                                   {"iat", now},
                                                   {"exp", now + 300}}
                                                  .dump());
        const std::string input = header + "." + payload;
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        std::size_t size = 0;
        if (context == nullptr ||
            EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key_) != 1 ||
            EVP_DigestSignUpdate(context, input.data(), input.size()) != 1 ||
            EVP_DigestSignFinal(context, nullptr, &size) != 1) {
            EVP_MD_CTX_free(context);
            return {};
        }
        std::vector<unsigned char> signature(size);
        if (EVP_DigestSignFinal(context, signature.data(), &size) != 1) {
            EVP_MD_CTX_free(context);
            return {};
        }
        EVP_MD_CTX_free(context);
        signature.resize(size);
        return input + "." + base64url(signature.data(), signature.size());
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
                           {"kid", "stub-key"},
                           {"n", base64url(modulus_bytes.data(), modulus_bytes.size())},
                           {"e", base64url(exponent_bytes.data(), exponent_bytes.size())}}})}}
            .dump();
    }

    void send_response(int descriptor,
                       int status,
                       std::string_view body,
                       std::string_view location = {}) {
        const std::string reason = status == 200 ? "OK" : "Found";
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                               "\r\nConnection: close\r\nContent-Type: "
                               "application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\n";
        if (!location.empty()) {
            response += "Location: " + std::string(location) + "\r\n";
        }
        response += "\r\n";
        response += body;
        ::send(descriptor, response.data(), response.size(), 0);
    }

    void handle(int descriptor) {
        std::string request_text;
        char buffer[4096];
        std::size_t content_length = 0;
        for (;;) {
            const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return;
            }
            request_text.append(buffer, static_cast<std::size_t>(count));
            const std::size_t headers_end = request_text.find("\r\n\r\n");
            if (headers_end == std::string::npos) {
                continue;
            }
            const std::string length_header = "Content-Length: ";
            const std::size_t length_start = request_text.find(length_header);
            if (length_start != std::string::npos) {
                content_length = static_cast<std::size_t>(
                    std::stoul(request_text.substr(length_start + length_header.size())));
            }
            if (request_text.size() >= headers_end + 4 + content_length) {
                break;
            }
        }
        const std::size_t target_start = request_text.find(' ') + 1;
        const std::size_t target_end = request_text.find(' ', target_start);
        const std::string target = request_text.substr(target_start, target_end - target_start);
        const std::size_t headers_end = request_text.find("\r\n\r\n");
        const std::string body = request_text.substr(headers_end + 4);
        if (target.starts_with("/authorize?")) {
            const auto query = parameters(target.substr(target.find('?') + 1));
            {
                std::lock_guard lock(state_mutex_);
                code_challenge_ = query.at("code_challenge");
                nonce_ = query.at("nonce");
            }
            const std::string location =
                query.at("redirect_uri") + "?code=stub-code&state=" + query.at("state");
            send_response(descriptor, 302, "", location);
            return;
        }
        if (target == "/token") {
            const auto form = parameters(body);
            std::string challenge;
            std::string nonce;
            {
                std::lock_guard lock(state_mutex_);
                challenge = code_challenge_;
                nonce = nonce_;
            }
            const bool verified =
                form.contains("code_verifier") && form.at("code") == "stub-code" &&
                form.at("client_id") == "lake-test" && form.at("client_secret") == "stub-secret" &&
                lake::control::identity_crypto::sha256_base64url(form.at("code_verifier")) ==
                    challenge;
            pkce_verified_.store(verified);
            send_response(descriptor,
                          200,
                          json{{"token_type", "Bearer"}, {"id_token", sign_token(nonce)}}.dump());
            return;
        }
        if (target == "/jwks") {
            jwks_requests_.fetch_add(1);
            send_response(descriptor, 200, jwks());
            return;
        }
        send_response(descriptor, 404, "{}");
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

    EVP_PKEY* key_ = nullptr;
    int listener_ = -1;
    unsigned short port_ = 0;
    std::thread worker_;
    mutable std::mutex state_mutex_;
    std::string code_challenge_;
    std::string nonce_;
    std::atomic<int> jwks_requests_{0};
    std::atomic<bool> pkce_verified_{false};
};

std::string callback_target(const std::string& location) {
    const std::size_t scheme = location.find("://");
    const std::size_t path =
        scheme == std::string::npos ? location.find('/') : location.find('/', scheme + 3);
    return path == std::string::npos ? std::string("/") : location.substr(path);
}

std::string authorize_once(unsigned short lake_port, unsigned short provider_port) {
    const std::string started =
        request(lake_port,
                "POST",
                "/v1/start_oauth",
                R"({"provider":"stub","device_name":"software browser","identity_document":"{}"})",
                "application/json");
    EXPECT_NE(started.find(" 200 "), std::string::npos) << started;
    const json value = json::parse(response_body(started));
    const std::string authorization_url = value["value"]["authorization_url"].get<std::string>();
    const std::string target = callback_target(authorization_url);
    const std::string authorized = request(provider_port, "GET", target);
    EXPECT_NE(authorized.find(" 302 "), std::string::npos) << authorized;
    return response_header(authorized, "Location");
}

} // namespace

TEST(OAuth, AuthorizationCodePkceOidcAndCachedJwks) {
    StubProvider provider;
    ASSERT_NE(provider.issuer(), "http://127.0.0.1:0");
    const unsigned short lake_port = unused_port();
    ASSERT_NE(lake_port, 0);
    auto store = std::make_shared<lake::control::InMemoryStore>();
    ASSERT_TRUE(store->register_oauth_provider({"lake",
                                                "stub",
                                                provider.issuer(),
                                                "lake-test",
                                                "stub-secret",
                                                {"openid", "email", "profile"}}));

    lake::api::ServerOptions options;
    options.port = lake_port;
    options.public_host = "127.0.0.1:" + std::to_string(lake_port);
    options.store = store;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string first_location = authorize_once(
        lake_port, std::stoi(provider.issuer().substr(provider.issuer().rfind(':') + 1)));
    const std::string first_callback = request(lake_port, "GET", callback_target(first_location));
    ASSERT_NE(first_callback.find(" 302 "), std::string::npos) << first_callback;
    // Nothing named a page: the account lands on the home, Ask Orama (lake#600).
    EXPECT_EQ(response_header(first_callback, "Location"), "/");
    EXPECT_TRUE(provider.pkce_verified());
    EXPECT_EQ(provider.jwks_requests(), 1);
    const std::string cookie = response_header(first_callback, "Set-Cookie");
    ASSERT_TRUE(cookie.starts_with("lake_session="));
    const std::size_t secret_end = cookie.find(';');
    const std::string secret = cookie.substr(13, secret_end - 13);
    const auto principal = store->context_for(secret);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->pool_id, "lake");
    const auto audit_events = lake::test::audit_events(*store, lake::audit::Filter{.limit = 100});
    ASSERT_TRUE(audit_events) << audit_events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*audit_events, [](const lake::audit::Event& event) {
        return event.draft.kind == "authentication" && event.draft.action == "sign_in" &&
               event.draft.outcome == "success";
    }));

    const std::string replay = request(lake_port, "GET", callback_target(first_location));
    EXPECT_NE(replay.find(" 403 "), std::string::npos);

    const unsigned short provider_port = static_cast<unsigned short>(
        std::stoi(provider.issuer().substr(provider.issuer().rfind(':') + 1)));
    const std::string second_location = authorize_once(lake_port, provider_port);
    const std::string second_callback = request(lake_port, "GET", callback_target(second_location));
    EXPECT_NE(second_callback.find(" 302 "), std::string::npos) << second_callback;
    EXPECT_EQ(provider.jwks_requests(), 1);

    ASSERT_TRUE(store->register_oauth_provider({"lake",
                                                "insecure",
                                                "http://provider.example.test",
                                                "lake-test",
                                                "stub-secret",
                                                {"openid"}}));
    const std::string insecure = request(
        lake_port, "POST", "/v1/start_oauth", R"({"provider":"insecure"})", "application/json");
    EXPECT_NE(insecure.find(" 400 "), std::string::npos) << insecure;
    server.stop();
}

TEST(OAuth, EnterpriseOidcAuthorizationCodeCallback) {
    StubProvider provider;
    const unsigned short lake_port = unused_port();
    ASSERT_NE(lake_port, 0);
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto organization = store->create_organization({"", "Enterprise OIDC"});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto sso = store->register_sso_provider({organization->id,
                                                   "lake",
                                                   "stub-enterprise",
                                                   "oidc",
                                                   provider.issuer(),
                                                   "",
                                                   "",
                                                   "lake-test",
                                                   "stub-secret",
                                                   "scim-stub-token",
                                                   nlohmann::json::object(),
                                                   true});
    ASSERT_TRUE(sso) << sso.error().message;
    ASSERT_TRUE(store->create_federation_issuer({organization->id, provider.issuer(), "email"}));
    lake::api::ServerOptions options;
    options.port = lake_port;
    options.public_host = "127.0.0.1:" + std::to_string(lake_port);
    options.store = store;
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string started = request(
        lake_port,
        "POST",
        "/v1/start_sso_oidc",
        nlohmann::json{{"provider_id", sso->id}, {"taap_id", "taap:1:ed25519:enterprise"}}.dump(),
        "application/json");
    ASSERT_NE(started.find(" 200 "), std::string::npos) << started;
    const std::string authorization_url =
        nlohmann::json::parse(response_body(started))["value"]["authorization_url"];
    const std::string authorized =
        request(static_cast<unsigned short>(
                    std::stoi(provider.issuer().substr(provider.issuer().rfind(':') + 1))),
                "GET",
                callback_target(authorization_url));
    ASSERT_NE(authorized.find(" 302 "), std::string::npos) << authorized;
    const std::string callback =
        request(lake_port, "GET", callback_target(response_header(authorized, "Location")));
    EXPECT_NE(callback.find(" 302 "), std::string::npos) << callback;
    EXPECT_TRUE(provider.pkce_verified());
    server.stop();
}
