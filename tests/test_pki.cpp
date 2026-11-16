// SPDX-License-Identifier: MIT

#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/pki.hpp"
#include "lake/control/postgres_store.hpp"
#include "temporary_directory.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace {

using json = nlohmann::json;

struct PkeyDeleter {
    void operator()(EVP_PKEY* value) const {
        EVP_PKEY_free(value);
    }
};

struct RequestDeleter {
    void operator()(X509_REQ* value) const {
        X509_REQ_free(value);
    }
};

struct SignatureDeleter {
    void operator()(ECDSA_SIG* value) const {
        ECDSA_SIG_free(value);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Request = std::unique_ptr<X509_REQ, RequestDeleter>;
using Signature = std::unique_ptr<ECDSA_SIG, SignatureDeleter>;

std::string base64url(const unsigned char* data, std::size_t size) {
    std::string output(4 * ((size + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(output.data()), data, static_cast<int>(size));
    if (written < 0) {
        return {};
    }
    output.resize(static_cast<std::size_t>(written));
    while (!output.empty() && output.back() == '=') {
        output.pop_back();
    }
    for (char& character : output) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    return output;
}

std::string base64url(std::string_view value) {
    return base64url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

Pkey new_p256_key() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) != 1) {
        return {};
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw) != 1) {
        return {};
    }
    return Pkey(raw);
}

nlohmann::json jwk_of(EVP_PKEY* key) {
    BIGNUM* raw_x = nullptr;
    BIGNUM* raw_y = nullptr;
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_X, &raw_x) != 1 ||
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_Y, &raw_y) != 1) {
        BN_free(raw_x);
        BN_free(raw_y);
        return {};
    }
    std::unique_ptr<BIGNUM, decltype(&BN_free)> x(raw_x, BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> y(raw_y, BN_free);
    std::array<unsigned char, 32> x_bytes{};
    std::array<unsigned char, 32> y_bytes{};
    if (BN_bn2binpad(x.get(), x_bytes.data(), x_bytes.size()) != x_bytes.size() ||
        BN_bn2binpad(y.get(), y_bytes.data(), y_bytes.size()) != y_bytes.size()) {
        return {};
    }
    return {{"kty", "EC"},
            {"crv", "P-256"},
            {"x", base64url(x_bytes.data(), x_bytes.size())},
            {"y", base64url(y_bytes.data(), y_bytes.size())}};
}

std::string es256_signature(EVP_PKEY* key, std::string_view input) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        return {};
    }
    std::size_t der_size = 0;
    if (EVP_DigestSign(context.get(),
                       nullptr,
                       &der_size,
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size()) != 1) {
        return {};
    }
    std::vector<unsigned char> der(der_size);
    if (EVP_DigestSign(context.get(),
                       der.data(),
                       &der_size,
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size()) != 1) {
        return {};
    }
    der.resize(der_size);
    const unsigned char* cursor = der.data();
    Signature signature(d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size())));
    if (!signature) {
        return {};
    }
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(signature.get(), &r, &s);
    std::array<unsigned char, 64> raw{};
    if (BN_bn2binpad(r, raw.data(), 32) != 32 || BN_bn2binpad(s, raw.data() + 32, 32) != 32) {
        return {};
    }
    return base64url(raw.data(), raw.size());
}

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

HttpResponse request(unsigned short port,
                     std::string_view method,
                     std::string_view path,
                     std::string_view body = {}) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    std::string wire = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\n";
    wire += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    wire += "Connection: close\r\n";
    if (method == "POST") {
        wire += "Content-Type: application/jose+json\r\n";
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    wire += "\r\n";
    wire += body;
    std::size_t sent = 0;
    while (sent < wire.size()) {
        const ssize_t count = ::send(socket_fd, wire.data() + sent, wire.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
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
    HttpResponse parsed;
    const std::size_t first_space = response.find(' ');
    if (first_space != std::string::npos) {
        parsed.status = std::stoi(response.substr(first_space + 1, 3));
    }
    const std::size_t header_end = response.find("\r\n\r\n");
    std::size_t line = response.find("\r\n") + 2;
    while (line < header_end) {
        const std::size_t end = response.find("\r\n", line);
        const std::size_t colon = response.find(':', line);
        if (colon < end) {
            parsed.headers.emplace(response.substr(line, colon - line),
                                   response.substr(colon + 2, end - colon - 2));
        }
        line = end + 2;
    }
    if (header_end != std::string::npos) {
        parsed.body = response.substr(header_end + 4);
    }
    return parsed;
}

std::string path_of(std::string_view url) {
    const std::size_t scheme = url.find("://");
    const std::size_t path = url.find('/', scheme == std::string_view::npos ? 0 : scheme + 3);
    return path == std::string_view::npos ? "/" : std::string(url.substr(path));
}

std::string header(const HttpResponse& response, std::string_view name) {
    const auto value = response.headers.find(std::string(name));
    return value == response.headers.end() ? std::string{} : value->second;
}

class AcmeClient {
public:
    explicit AcmeClient(unsigned short port) : port_(port), account_key_(new_p256_key()) {}

    HttpResponse get(std::string_view url) const {
        return request(port_, "GET", path_of(url));
    }

    HttpResponse nonce(std::string_view url) {
        HttpResponse response = request(port_, "HEAD", path_of(url));
        nonce_ = header(response, "Replay-Nonce");
        return response;
    }

    HttpResponse post(std::string_view url,
                      const nlohmann::json& payload,
                      bool use_jwk = false,
                      bool post_as_get = false) {
        nlohmann::json protected_header{
            {"alg", "ES256"}, {"nonce", nonce_}, {"url", std::string(url)}};
        if (use_jwk) {
            protected_header["jwk"] = jwk_of(account_key_.get());
        } else {
            protected_header["kid"] = account_url_;
        }
        const std::string encoded_protected = base64url(protected_header.dump());
        const std::string encoded_payload = post_as_get ? "" : base64url(payload.dump());
        const std::string signing_input = encoded_protected + "." + encoded_payload;
        const nlohmann::json jws{{"protected", encoded_protected},
                                 {"payload", encoded_payload},
                                 {"signature", es256_signature(account_key_.get(), signing_input)}};
        HttpResponse response = request(port_, "POST", path_of(url), jws.dump());
        nonce_ = header(response, "Replay-Nonce");
        return response;
    }

    // RFC 8555 §7.3.4: the account key, signed HS256 with the binding an administrator issued.
    nlohmann::json external_account_binding(const lake::control::pki::ExternalAccountBinding& binding,
                                            std::string_view url) const {
        const std::string encoded_protected = base64url(
            nlohmann::json{{"alg", "HS256"}, {"kid", binding.key_id}, {"url", std::string(url)}}
                .dump());
        const std::string encoded_payload = base64url(jwk_of(account_key_.get()).dump());
        std::string key = binding.hmac_key;
        std::ranges::replace(key, '-', '+');
        std::ranges::replace(key, '_', '/');
        while (key.size() % 4 != 0) {
            key += '=';
        }
        std::string raw(key.size(), '\0');
        const int decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(raw.data()),
                                            reinterpret_cast<const unsigned char*>(key.data()),
                                            static_cast<int>(key.size()));
        raw.resize(static_cast<std::size_t>(decoded < 0 ? 0 : decoded) -
                   static_cast<std::size_t>(std::ranges::count(key, '=')));
        const std::string input = encoded_protected + "." + encoded_payload;
        std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
        unsigned size = 0;
        HMAC(EVP_sha256(),
             raw.data(),
             static_cast<int>(raw.size()),
             reinterpret_cast<const unsigned char*>(input.data()),
             input.size(),
             mac.data(),
             &size);
        return {{"protected", encoded_protected},
                {"payload", encoded_payload},
                {"signature", base64url(mac.data(), size)}};
    }

    void set_account_url(std::string account_url) {
        account_url_ = std::move(account_url);
    }

    std::string thumbprint() const {
        const nlohmann::json jwk = jwk_of(account_key_.get());
        const std::string canonical = nlohmann::json{
            {"crv", "P-256"},
            {"kty", "EC"},
            {"x", jwk.at("x")},
            {"y", jwk.at("y")}}.dump();
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned size = 0;
        if (EVP_Digest(
                canonical.data(), canonical.size(), digest.data(), &size, EVP_sha256(), nullptr) !=
            1) {
            return {};
        }
        return base64url(digest.data(), size);
    }

private:
    unsigned short port_;
    Pkey account_key_;
    std::string nonce_;
    std::string account_url_;
};

std::string csr_for(std::string_view identifier, std::string_view extra_sans = {}) {
    Pkey key = new_p256_key();
    Request request_value(X509_REQ_new());
    if (!key || !request_value || X509_REQ_set_version(request_value.get(), 0) != 1 ||
        X509_REQ_set_pubkey(request_value.get(), key.get()) != 1) {
        return {};
    }
    X509_NAME* subject = X509_REQ_get_subject_name(request_value.get());
    if (X509_NAME_add_entry_by_txt(subject,
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(identifier.data()),
                                   static_cast<int>(identifier.size()),
                                   -1,
                                   0) != 1) {
        return {};
    }
    STACK_OF(X509_EXTENSION)* extensions = sk_X509_EXTENSION_new_null();
    X509_EXTENSION* san = X509V3_EXT_conf_nid(
        nullptr,
        nullptr,
        NID_subject_alt_name,
        const_cast<char*>(("DNS:" + std::string(identifier) + std::string(extra_sans)).c_str()));
    if (!extensions || !san || sk_X509_EXTENSION_push(extensions, san) != 1 ||
        X509_REQ_add_extensions(request_value.get(), extensions) != 1) {
        sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
        return {};
    }
    sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
    if (X509_REQ_sign(request_value.get(), key.get(), EVP_sha256()) <= 0) {
        return {};
    }
    const int der_size = i2d_X509_REQ(request_value.get(), nullptr);
    if (der_size <= 0) {
        return {};
    }
    std::vector<unsigned char> der(static_cast<std::size_t>(der_size));
    unsigned char* cursor = der.data();
    if (i2d_X509_REQ(request_value.get(), &cursor) != der_size) {
        return {};
    }
    return base64url(der.data(), der.size());
}

void expect_single_san(std::string_view pem, int type, std::string_view expected) {
    std::unique_ptr<BIO, decltype(&BIO_free)> input(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    ASSERT_TRUE(input);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(
        PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
    ASSERT_TRUE(certificate);
    const int index = X509_get_ext_by_NID(certificate.get(), NID_subject_alt_name, -1);
    ASSERT_GE(index, 0);
    EXPECT_EQ(X509_get_ext_by_NID(certificate.get(), NID_subject_alt_name, index), -1);
    std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(
        static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, nullptr, nullptr)),
        GENERAL_NAMES_free);
    ASSERT_TRUE(names);
    ASSERT_EQ(sk_GENERAL_NAME_num(names.get()), 1);
    const auto* name = sk_GENERAL_NAME_value(names.get(), 0);
    ASSERT_EQ(name->type, type);
    const auto* value = type == GEN_URI ? name->d.uniformResourceIdentifier : name->d.dNSName;
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)),
                               static_cast<std::size_t>(ASN1_STRING_length(value))),
              expected);
}

TEST(Pki, X509SvidRejectsMalformedSpiffeIdentities) {
    auto authority = lake::control::pki::create_authority("customer-a");
    ASSERT_TRUE(authority) << authority.error().message;
    const std::vector<std::string> invalid{"spiffe://lake.test/a,DNS:extra.test",
                                           "spiffe://lake.test/a,IP:127.0.0.1",
                                           "spiffe://lake.test/a,URI:spiffe://other.test/a",
                                           "spiffe://lake.test/a,email:a@b.test",
                                           "spiffe://lake.test/a:b",
                                           "spiffe://lake.test:443/a",
                                           "spiffe://user@lake.test/a",
                                           "spiffe://",
                                           "spiffe:///a",
                                           "spiffe://LAKE.test/a",
                                           "spiffe://lake.test/",
                                           "spiffe://lake.test/a//b",
                                           "spiffe://lake.test/a/../b",
                                           "spiffe://lake.test/a/./b",
                                           "spiffe://lake.test/a?b",
                                           "spiffe://lake.test/a#b",
                                           "spiffe://lake.test/a%2cb",
                                           "spiffe://lake.test/a b",
                                           "spiffe://lake.test/a\tb",
                                           "spiffe://lake.test/a\nb",
                                           std::string("spiffe://lake.test/a") + '\0' +
                                               ",DNS:extra.test",
                                           "spiffe://lake.test/\xc3\xa9",
                                           "spiffe://" + std::string(256, 'a') + "/b",
                                           "spiffe://lake.test/" + std::string(2048, 'a')};
    for (const auto& identity : invalid) {
        SCOPED_TRACE(identity);
        const auto result = lake::control::pki::issue_x509_svid(*authority, identity);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "invalid_input");
    }
    for (const std::string identity : {"spiffe://lake.test/Workload_1/a-b.v2",
                                       "spiffe://trust_domain",
                                       "spiffe://127.0.0.1/a"}) {
        const auto result = lake::control::pki::issue_x509_svid(*authority, identity);
        ASSERT_TRUE(result) << result.error().message;
        expect_single_san(result->certificate_pem, GEN_URI, identity);
    }
}

TEST(Pki, AgentCertificateRejectsSanConfigurationDelimiters) {
    auto authority = lake::control::pki::create_authority("customer-a");
    ASSERT_TRUE(authority) << authority.error().message;
    const std::string public_key(43, 'A');
    for (const std::string subject : {"taap:agent,DNS:extra.test",
                                      "taap:agent,IP:127.0.0.1",
                                      "taap:agent,URI:spiffe://other.test/a",
                                      "taap:agent\n"}) {
        const auto result =
            lake::control::pki::issue_agent_certificate(*authority, subject, public_key);
        ASSERT_FALSE(result) << subject;
        EXPECT_EQ(result.error().code, "invalid_input");
    }
    const auto result =
        lake::control::pki::issue_agent_certificate(*authority, "taap:test-agent", public_key);
    ASSERT_TRUE(result) << result.error().message;
    expect_single_san(result->certificate_pem, GEN_URI, "taap:test-agent");
}

TEST(Pki, AcmeRejectsMalformedDnsIdentifiers) {
    lake::control::InMemoryStore store;
    const auto organization = store.create_organization({"taap:acme", "ACME", ""});
    ASSERT_TRUE(organization);
    const auto customer = store.create_customer({"organization", "", organization->id, "acme"});
    ASSERT_TRUE(customer);
    const std::string kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    lake::control::pki::Service pki;
    ASSERT_TRUE(pki.create_authority(store, customer->id, kek));
    const auto jwk = jwk_of(new_p256_key().get());
    const auto account = pki.create_acme_account(
        store, customer->id, {jwk.at("x").get<std::string>(), jwk.at("y").get<std::string>()});
    ASSERT_TRUE(account);
    const std::vector<std::string> invalid{"a.test,DNS:extra.test",
                                           "a.test,IP:127.0.0.1",
                                           "a.test,URI:spiffe://other.test/a",
                                           "a.test,email:a@b.test",
                                           "a.test:443",
                                           "a.test b",
                                           "a.test\n",
                                           "a.test\t",
                                           std::string("a.test") + '\0' + ",DNS:extra.test",
                                           "a%2ctest.test",
                                           "a_test.test",
                                           "*.test",
                                           "-a.test",
                                           "a-.test",
                                           "a..test",
                                           ".a.test",
                                           "a.test.",
                                           "a.test/path",
                                           "a.\xc3\xa9",
                                           std::string(64, 'a') + ".test",
                                           std::string(63, 'a') + "." + std::string(63, 'b') + "." +
                                               std::string(63, 'c') + "." + std::string(62, 'd')};
    for (const auto& identifier : invalid) {
        SCOPED_TRACE(identifier);
        const auto result =
            pki.create_acme_order(store, customer->id, account->id, identifier, kek);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, "invalid_input");
    }
    EXPECT_EQ(pki.acme_state_size().orders, 0U);
    for (const std::string identifier : {"Service-1.lake.test", "xn--bcher-kva.test"}) {
        const auto result =
            pki.create_acme_order(store, customer->id, account->id, identifier, kek);
        ASSERT_TRUE(result) << result.error().message;
    }
}

TEST(Pki, IssuesAndVerifiesSpiffeX509AndJwtSvids) {
    auto authority = lake::control::pki::create_authority("customer-a");
    ASSERT_TRUE(authority) << authority.error().message;

    auto x509 = lake::control::pki::issue_x509_svid(*authority, "spiffe://lake.test/worker");
    ASSERT_TRUE(x509) << x509.error().message;
    expect_single_san(x509->certificate_pem, GEN_URI, "spiffe://lake.test/worker");
    EXPECT_TRUE(lake::control::pki::verify_x509_svid(*authority, *x509))
        << lake::control::pki::verify_x509_svid(*authority, *x509).error().message;

    auto jwt = lake::control::pki::issue_jwt_svid(*authority, "spiffe://lake.test/worker", "lake");
    ASSERT_TRUE(jwt) << jwt.error().message;
    EXPECT_TRUE(lake::control::pki::verify_jwt_svid(*authority, *jwt))
        << lake::control::pki::verify_jwt_svid(*authority, *jwt).error().message;
}

TEST(Pki, ShamirRecoveryRequiresConfiguredQuorum) {
    const std::string root = "customer root material";
    std::vector<std::byte> secret;
    for (const char character : root) {
        secret.push_back(static_cast<std::byte>(character));
    }
    auto shares = lake::control::pki::split_secret(secret, 3, 5);
    ASSERT_TRUE(shares) << shares.error().message;

    std::vector<lake::control::pki::ShamirShare> short_quorum{shares->at(0), shares->at(1)};
    EXPECT_FALSE(lake::control::pki::recover_secret(short_quorum, 3));

    std::vector<lake::control::pki::ShamirShare> quorum{
        shares->at(0), shares->at(2), shares->at(4)};
    auto recovered = lake::control::pki::recover_secret(quorum, 3);
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(*recovered, secret);
}

TEST(Pki, RegistryIssuesSvidAndRecordsAuditEvent) {
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::control::InMemoryStore store;
    lake::control::pki::Service pki;
    lake::api::ApiContext context;
    context.store = &store;
    context.pki = &pki;
    context.authorized = true;
    context.acting_identity = "admin";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    const auto organization =
        store.create_organization({"taap:1:ed25519:organization", "Organization", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store.create_customer({"organization", "", organization->id, "pki-customer"});
    ASSERT_TRUE(customer) << customer.error().message;

    const auto authority =
        registry.call("create_pki_authority", {{"customer_id", customer->id}}, context);
    ASSERT_TRUE(authority.ok) << authority.error.message;
    context.request_id.clear();
    const auto svid =
        registry.call("issue_x509_svid",
                      {{"customer_id", customer->id}, {"spiffe_id", "spiffe://lake.test/api"}},
                      context);
    ASSERT_TRUE(svid.ok) << svid.error.message;
    context.request_id.clear();
    const auto client = registry.call("issue_client_certificate",
                                      {{"customer_id", customer->id}, {"subject", "client-1"}},
                                      context);
    ASSERT_TRUE(client.ok) << client.error.message;
    context.request_id.clear();
    const auto device = registry.call("issue_device_certificate",
                                      {{"customer_id", customer->id}, {"subject", "device-1"}},
                                      context);
    ASSERT_TRUE(device.ok) << device.error.message;
    const auto event = store.latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.kind, "certificate");
    EXPECT_EQ(event->draft.action, "issue_device_certificate");
}

// A customer with a CA and four people, the first three holding admin on it, and the rehearsal's
// contexts: each person signed in as themselves, and the operator.
struct RootRecoveryRig {
    static constexpr std::string_view kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    lake::api::Registry registry;
    lake::control::InMemoryStore store;
    lake::control::pki::Service pki;
    std::string customer_id;
    std::map<std::string, std::string> users; // TAAP id -> user id

    RootRecoveryRig() {
        lake::api::register_core_capabilities(registry, "test");
        const auto organization =
            store.create_organization({"taap:1:ed25519:recovery-org", "Recovery", ""});
        EXPECT_TRUE(organization) << organization.error().message;
        const auto customer =
            store.create_customer({"organization", "", organization->id, "recovery-customer"});
        EXPECT_TRUE(customer) << customer.error().message;
        customer_id = customer->id;
        const auto object = store.find_policy_object("customer", customer_id);
        EXPECT_TRUE(object) << object.error().message;
        for (const std::string name : {"alice", "bob", "carol", "dave"}) {
            const std::string taap = "taap:1:ed25519:recovery-" + name;
            const auto user = store.create_user({taap, name + "@example.test", name});
            EXPECT_TRUE(user) << user.error().message;
            users[taap] = user->id;
            if (name != "dave") {
                EXPECT_TRUE(store.create_grant({taap,
                                                object->id,
                                                "admin",
                                                "attestation-" + name,
                                                "admin",
                                                "request-" + name}));
            }
        }
        auto admin = operator_context();
        const auto authority =
            registry.call("create_pki_authority", {{"customer_id", customer_id}}, admin);
        EXPECT_TRUE(authority.ok) << authority.error.message;
    }

    static std::string taap(std::string_view name) {
        return "taap:1:ed25519:recovery-" + std::string(name);
    }

    lake::api::ApiContext operator_context() {
        lake::api::ApiContext context;
        context.store = &store;
        context.pki = &pki;
        context.authorized = true;
        context.acting_identity = "admin";
        context.operator_credential = true;
        context.instance_kek = std::string(kek);
        return context;
    }

    lake::api::ApiContext person(std::string_view name) {
        auto context = operator_context();
        context.operator_credential = false;
        context.acting_identity = taap(name);
        context.acting_user_id = users.at(taap(name));
        context.session_credential = true;
        context.credential_kind = "browser";
        context.scopes = {"*"};
        return context;
    }

    lake::api::ApiResult
    call(const std::string& name, const json& input, lake::api::ApiContext context) {
        return registry.call(name, input, context);
    }

    json begin(lake::api::ApiContext context, int threshold, json participants) {
        const auto begun = call("begin_root_recovery",
                                {{"customer_id", customer_id},
                                 {"threshold", threshold},
                                 {"participants", std::move(participants)}},
                                std::move(context));
        EXPECT_TRUE(begun.ok) << begun.error.message;
        return begun.value;
    }

    lake::api::ApiResult claim(std::string_view name, const std::string& ceremony_id) {
        return call("claim_root_recovery_share",
                    {{"customer_id", customer_id}, {"ceremony_id", ceremony_id}},
                    person(name));
    }

    lake::api::ApiResult
    present(lake::api::ApiContext context, const std::string& ceremony_id, const json& share) {
        return call("complete_root_recovery",
                    {{"customer_id", customer_id},
                     {"ceremony_id", ceremony_id},
                     {"share", {{"index", share.at("index")}, {"value", share.at("value")}}}},
                    std::move(context));
    }
};

TEST(Pki, RootRecoveryBeginsWithoutHandingOutAnyShare) {
    RootRecoveryRig rig;
    // The form that answered every share to its one caller (lake#934) is refused, share-less.
    const auto whole_quorum =
        rig.call("begin_root_recovery",
                 {{"customer_id", rig.customer_id}, {"threshold", 2}, {"share_count", 3}},
                 rig.person("alice"));
    EXPECT_FALSE(whole_quorum.ok);
    EXPECT_FALSE(whole_quorum.value.contains("shares")) << whole_quorum.value.dump();

    const json participants = json::array({RootRecoveryRig::taap("alice"),
                                           RootRecoveryRig::taap("bob"),
                                           RootRecoveryRig::taap("carol")});
    const json begun = rig.begin(rig.person("alice"), 2, participants);
    EXPECT_FALSE(begun.contains("shares"));
    EXPECT_EQ(begun.at("share_count"), 3);
    ASSERT_EQ(begun.at("participants").size(), 3U);
    for (const json& participant : begun.at("participants")) {
        EXPECT_EQ(participant.size(), 2U) << participant.dump(); // index and principal only
    }
    const std::string ceremony_id = begun.at("ceremony_id").get<std::string>();

    // Every share, claimed afterwards, is absent from the answer that began the ceremony.
    for (const std::string name : {"alice", "bob", "carol"}) {
        const auto share = rig.claim(name, ceremony_id);
        ASSERT_TRUE(share.ok) << share.error.message;
        EXPECT_EQ(begun.dump().find(share.value.at("value").get<std::string>()), std::string::npos);
    }

    // A quorum of one, more than there are participants, or the same person twice is refused.
    auto alice = rig.person("alice");
    for (const auto& [threshold, people] : std::vector<std::pair<int, json>>{
             {1, participants},
             {4, participants},
             {2, json::array({RootRecoveryRig::taap("alice"), RootRecoveryRig::taap("alice")})},
             {2, json::array({RootRecoveryRig::taap("alice")})}}) {
        const auto refused = rig.call(
            "begin_root_recovery",
            {{"customer_id", rig.customer_id}, {"threshold", threshold}, {"participants", people}},
            alice);
        EXPECT_FALSE(refused.ok) << threshold << " " << people.dump();
    }
    // A participant who does not hold admin on the customer, or who is not a TAAP identity, is
    // refused.
    for (const json& people :
         {json::array({RootRecoveryRig::taap("alice"), RootRecoveryRig::taap("dave")}),
          json::array({RootRecoveryRig::taap("alice"), "admin"})}) {
        const auto refused =
            rig.call("begin_root_recovery",
                     {{"customer_id", rig.customer_id}, {"threshold", 2}, {"participants", people}},
                     alice);
        EXPECT_FALSE(refused.ok) << people.dump();
    }
}

TEST(Pki, RootRecoveryNeedsSharesPresentedByDistinctParticipants) {
    RootRecoveryRig rig;
    const json begun = rig.begin(rig.operator_context(),
                                 2,
                                 json::array({RootRecoveryRig::taap("alice"),
                                              RootRecoveryRig::taap("bob"),
                                              RootRecoveryRig::taap("carol")}));
    const std::string ceremony_id = begun.at("ceremony_id").get<std::string>();

    // Only a participant, signed in as themselves, claims a share: their own, once.
    EXPECT_EQ(rig.call("claim_root_recovery_share",
                       {{"customer_id", rig.customer_id}, {"ceremony_id", ceremony_id}},
                       rig.operator_context())
                  .error.code,
              "forbidden");
    EXPECT_FALSE(rig.claim("dave", ceremony_id).ok); // not an admin, not a participant
    auto agent = rig.person("alice");
    agent.credential_kind = "agent_task";
    EXPECT_EQ(rig.call("claim_root_recovery_share",
                       {{"customer_id", rig.customer_id}, {"ceremony_id", ceremony_id}},
                       agent)
                  .error.code,
              "forbidden");
    const auto alice_share = rig.claim("alice", ceremony_id);
    ASSERT_TRUE(alice_share.ok) << alice_share.error.message;
    EXPECT_EQ(alice_share.value.at("index"), 1);
    EXPECT_EQ(rig.claim("alice", ceremony_id).error.code, "conflict");
    const auto bob_share = rig.claim("bob", ceremony_id);
    ASSERT_TRUE(bob_share.ok) << bob_share.error.message;
    EXPECT_EQ(bob_share.value.at("index"), 2);

    // One person holding two shares cannot present both: the second is not theirs.
    const auto first = rig.present(rig.person("alice"), ceremony_id, alice_share.value);
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_FALSE(first.value.at("recovered").get<bool>());
    EXPECT_EQ(first.value.at("submitted"), 1);
    EXPECT_EQ(rig.present(rig.person("alice"), ceremony_id, bob_share.value).error.code,
              "forbidden");
    EXPECT_EQ(rig.present(rig.person("alice"), ceremony_id, alice_share.value).error.code,
              "conflict");
    EXPECT_FALSE(rig.present(rig.operator_context(), ceremony_id, bob_share.value).ok);
    EXPECT_FALSE(rig.store.get_pki_recovery_ceremony(rig.customer_id, ceremony_id)->recovered_at);

    // A wrong value is refused without using up the participant's turn.
    json tampered = bob_share.value;
    std::string value = tampered.at("value").get<std::string>();
    value[0] = value[0] == '0' ? '1' : '0';
    tampered["value"] = value;
    EXPECT_EQ(rig.present(rig.person("bob"), ceremony_id, tampered).error.code, "unauthorized");

    const auto second = rig.present(rig.person("bob"), ceremony_id, bob_share.value);
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_TRUE(second.value.at("recovered").get<bool>());
    EXPECT_EQ(second.value.at("submitted"), 2);
    const auto event = rig.store.latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.kind, "recovery");
    EXPECT_EQ(event->draft.action, "complete_root_recovery");
    EXPECT_EQ(event->draft.principal, RootRecoveryRig::taap("bob"));
    EXPECT_EQ(event->draft.object, "root-recovery/" + ceremony_id);
    EXPECT_EQ(event->draft.details.dump().find(bob_share.value.at("value").get<std::string>()),
              std::string::npos);
    EXPECT_TRUE(rig.store.get_pki_recovery_ceremony(rig.customer_id, ceremony_id)->recovered_at);

    // A recovered ceremony issues no further share.
    EXPECT_EQ(rig.claim("carol", ceremony_id).error.code, "conflict");
}

TEST(Pki, PersistsAuthorityKeysThroughTheCustomerKeystore) {
    lake::control::InMemoryStore store;
    const auto organization =
        store.create_organization({"taap:1:ed25519:pki-persistence", "PKI persistence", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store.create_customer({"organization", "", organization->id, "pki-persistence"});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    lake::control::pki::Service creator;
    ASSERT_TRUE(creator.create_authority(store, customer->id, instance_kek));

    lake::control::pki::Service after_restart;
    auto issued = after_restart.issue_x509_svid(
        store, customer->id, "spiffe://lake.test/persisted", instance_kek);
    ASSERT_TRUE(issued) << issued.error().message;
    ASSERT_TRUE(store.rotate_customer_kek(customer->id, instance_kek));
    EXPECT_TRUE(after_restart.issue_jwt_svid(
        store, customer->id, "spiffe://lake.test/persisted", "lake", instance_kek));
}

TEST(Pki, IssuesOpenSshUserAndHostCertificates) {
    const std::filesystem::path directory = lake::test::fixture_path("ssh");
    std::filesystem::create_directories(directory);
    const std::filesystem::path key_path = directory / "subject";
    const std::string generate_command = "ssh-keygen -q -t ed25519 -N '' -f " + key_path.string();
    ASSERT_EQ(std::system(generate_command.c_str()), 0);
    std::ifstream public_key_file(key_path.string() + ".pub");
    std::string public_key;
    ASSERT_TRUE(std::getline(public_key_file, public_key));

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::control::InMemoryStore store;
    lake::control::pki::Service pki;
    lake::api::ApiContext context;
    context.store = &store;
    context.pki = &pki;
    context.authorized = true;
    context.acting_identity = "admin";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const auto organization =
        store.create_organization({"taap:1:ed25519:ssh", "SSH certificates", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store.create_customer({"organization", "", organization->id, "ssh-certificates"});
    ASSERT_TRUE(customer) << customer.error().message;
    ASSERT_TRUE(registry.call("create_pki_authority", {{"customer_id", customer->id}}, context).ok);

    context.request_id.clear();
    const auto user = registry.call("issue_ssh_user_certificate",
                                    {{"customer_id", customer->id},
                                     {"public_key", public_key},
                                     {"key_id", "operator"},
                                     {"principals", {"lake-admin"}}},
                                    context);
    ASSERT_TRUE(user.ok) << user.error.message;
    context.request_id.clear();
    const auto host = registry.call("issue_ssh_host_certificate",
                                    {{"customer_id", customer->id},
                                     {"public_key", public_key},
                                     {"key_id", "lake-node"},
                                     {"principals", {"lake.internal"}}},
                                    context);
    ASSERT_TRUE(host.ok) << host.error.message;

    const std::filesystem::path user_certificate_path = directory / "user-cert.pub";
    const std::filesystem::path host_certificate_path = directory / "host-cert.pub";
    const std::filesystem::path user_output_path = directory / "user.txt";
    const std::filesystem::path host_output_path = directory / "host.txt";
    std::ofstream(user_certificate_path) << user.value.at("certificate").get<std::string>() << '\n';
    std::ofstream(host_certificate_path) << host.value.at("certificate").get<std::string>() << '\n';
    const std::string inspect_user =
        "ssh-keygen -L -f " + user_certificate_path.string() + " > " + user_output_path.string();
    const std::string inspect_host =
        "ssh-keygen -L -f " + host_certificate_path.string() + " > " + host_output_path.string();
    ASSERT_EQ(std::system(inspect_user.c_str()), 0);
    ASSERT_EQ(std::system(inspect_host.c_str()), 0);
    std::ifstream user_output_file(user_output_path);
    std::ifstream host_output_file(host_output_path);
    const std::string user_output((std::istreambuf_iterator<char>(user_output_file)),
                                  std::istreambuf_iterator<char>());
    const std::string host_output((std::istreambuf_iterator<char>(host_output_file)),
                                  std::istreambuf_iterator<char>());
    EXPECT_NE(user_output.find("user certificate"), std::string::npos);
    EXPECT_NE(user_output.find("lake-admin"), std::string::npos);
    EXPECT_NE(host_output.find("host certificate"), std::string::npos);
    EXPECT_NE(host_output.find("lake.internal"), std::string::npos);
    const auto event = store.latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.action, "issue_ssh_host_certificate");
    std::filesystem::remove_all(directory);
}

TEST(Pki, AcmeClientCompletesHttp01OrderAndDownloadsCertificate) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto organization = store->create_organization({"taap:1:ed25519:acme", "ACME", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer = store->create_customer({"organization", "", organization->id, "acme"});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    lake::control::pki::Service authority;
    ASSERT_TRUE(authority.create_authority(*store, customer->id, instance_kek));

    lake::api::ServerOptions options;
    options.port = 0;
    options.store = store;
    options.instance_kek = instance_kek;
    // The HTTP-01 check fetches http://<identifier>/...; here the identifier's server answers what
    // `answer` holds for the challenge path (lake#919).
    auto answer = std::make_shared<std::map<std::string, std::string>>();
    std::vector<std::string> fetched;
    options.acme_http01_fetch = [answer, &fetched](const std::string& url) -> std::optional<std::string> {
        fetched.push_back(url);
        const auto found = answer->find(url);
        return found == answer->end() ? std::nullopt : std::optional(found->second);
    };
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    AcmeClient client(server.port());
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string directory_url = origin + "/acme/" + customer->id + "/directory";
    const HttpResponse directory_response = client.get(directory_url);
    ASSERT_EQ(directory_response.status, 200);
    const nlohmann::json directory = nlohmann::json::parse(directory_response.body);
    EXPECT_EQ(directory.at("meta").at("externalAccountRequired"), true);

    const HttpResponse nonce_response = client.nonce(directory.at("newNonce").get<std::string>());
    ASSERT_EQ(nonce_response.status, 200);
    ASSERT_FALSE(header(nonce_response, "Replay-Nonce").empty());

    // Without a binding from the customer's administrator, no account (lake#919); nor with one
    // issued for another customer.
    const std::string new_account = directory.at("newAccount");
    const HttpResponse unbound = client.post(new_account, {{"termsOfServiceAgreed", true}}, true);
    EXPECT_EQ(unbound.status, 403) << unbound.body;
    EXPECT_NE(unbound.body.find("externalAccountRequired"), std::string::npos) << unbound.body;
    const auto foreign = lake::control::pki::new_external_account_binding(instance_kek, "other");
    ASSERT_TRUE(foreign);
    const HttpResponse misbound = client.post(
        new_account,
        {{"termsOfServiceAgreed", true},
         {"externalAccountBinding", client.external_account_binding(*foreign, new_account)}},
        true);
    EXPECT_EQ(misbound.status, 403) << misbound.body;

    const auto binding =
        lake::control::pki::new_external_account_binding(instance_kek, customer->id);
    ASSERT_TRUE(binding) << binding.error().message;
    const HttpResponse account = client.post(
        new_account,
        {{"termsOfServiceAgreed", true},
         {"externalAccountBinding", client.external_account_binding(*binding, new_account)}},
        true);
    ASSERT_EQ(account.status, 201) << account.body;
    client.set_account_url(header(account, "Location"));
    ASSERT_FALSE(header(account, "Location").empty());

    const std::string identifier = "service.lake.test";
    const HttpResponse order = client.post(
        directory.at("newOrder").get<std::string>(),
        {{"identifiers", nlohmann::json::array({{{"type", "dns"}, {"value", identifier}}})}});
    ASSERT_EQ(order.status, 201) << order.body;
    const nlohmann::json order_document = nlohmann::json::parse(order.body);
    const std::string order_url = header(order, "Location");
    const std::string authorization_url = order_document.at("authorizations").at(0);

    const HttpResponse authorization =
        client.post(authorization_url, nlohmann::json::object(), false, true);
    ASSERT_EQ(authorization.status, 200) << authorization.body;
    const nlohmann::json authorization_document = nlohmann::json::parse(authorization.body);
    const nlohmann::json challenge = authorization_document.at("challenges").at(0);
    const std::string token = challenge.at("token");
    const HttpResponse key_authorization =
        client.get(origin + "/.well-known/acme-challenge/" + token);
    ASSERT_EQ(key_authorization.status, 200);
    EXPECT_EQ(key_authorization.body, token + "." + client.thumbprint());

    // The identifier does not answer the key authorization yet: the order stays pending and
    // finalize refuses (lake#919).
    const std::string challenge_url =
        "http://" + identifier + "/.well-known/acme-challenge/" + token;
    (*answer)[challenge_url] = token + ".not-this-account";
    const HttpResponse wrong =
        client.post(challenge.at("url").get<std::string>(), nlohmann::json::object());
    EXPECT_EQ(wrong.status, 403) << wrong.body;
    EXPECT_NE(wrong.body.find("incorrectResponse"), std::string::npos) << wrong.body;
    ASSERT_FALSE(fetched.empty());
    EXPECT_EQ(fetched.back(), challenge_url);
    const HttpResponse early = client.post(order_document.at("finalize").get<std::string>(),
                                           {{"csr", csr_for(identifier)}});
    EXPECT_NE(early.status, 200) << early.body;

    (*answer)[challenge_url] = key_authorization.body + "\n";
    const HttpResponse challenged =
        client.post(challenge.at("url").get<std::string>(), nlohmann::json::object());
    ASSERT_EQ(challenged.status, 200) << challenged.body;
    EXPECT_EQ(nlohmann::json::parse(challenged.body).at("status"), "valid");

    const HttpResponse finalized = client.post(order_document.at("finalize").get<std::string>(),
                                               {{"csr",
                                                 csr_for(identifier,
                                                         ",DNS:extra.test,IP:127.0.0.1,"
                                                         "URI:spiffe://other.test/a")}});
    ASSERT_EQ(finalized.status, 200) << finalized.body;
    const nlohmann::json finalized_order = nlohmann::json::parse(finalized.body);
    ASSERT_EQ(finalized_order.at("status"), "valid");

    const HttpResponse current_order =
        client.post(order_url, nlohmann::json::object(), false, true);
    ASSERT_EQ(current_order.status, 200) << current_order.body;
    const std::string certificate_url = nlohmann::json::parse(current_order.body).at("certificate");
    const HttpResponse certificate =
        client.post(certificate_url, nlohmann::json::object(), false, true);
    ASSERT_EQ(certificate.status, 200) << certificate.body;
    EXPECT_NE(certificate.body.find("-----BEGIN CERTIFICATE-----"), std::string::npos);
    expect_single_san(certificate.body, GEN_DNS, identifier);

    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.action, "acme_certificate");
    server.stop();
}

// ACME is anonymous, so its in-memory state is bounded and expires (lake#940). A test clock moves
// time; `now` starts well past the clock's epoch.
struct AcmeTestClock {
    std::shared_ptr<std::chrono::steady_clock::time_point> now =
        std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::hours(1000));

    lake::control::pki::AcmeLimits limits() const {
        lake::control::pki::AcmeLimits limits;
        limits.clock = [now = now] { return *now; };
        return limits;
    }
    void advance(std::chrono::steady_clock::duration by) const {
        *now += by;
    }
};

lake::control::pki::AcmeJwk new_acme_jwk() {
    const Pkey key = new_p256_key();
    const nlohmann::json jwk = jwk_of(key.get());
    return {jwk.at("x").get<std::string>(), jwk.at("y").get<std::string>()};
}

TEST(Pki, AcmeNoncesAreStatelessOneUseAndExpire) {
    const AcmeTestClock clock;
    auto limits = clock.limits();
    limits.nonce_lifetime = std::chrono::seconds(60);
    limits.used_nonces = 4;
    lake::control::pki::Service pki(limits);

    // Issuing nonces keeps nothing, however many are asked for.
    for (int i = 0; i < 10000; ++i) {
        ASSERT_TRUE(pki.new_acme_nonce());
    }
    EXPECT_EQ(pki.acme_state_size().used_nonces, 0U);

    auto nonce = pki.new_acme_nonce();
    ASSERT_TRUE(nonce);
    EXPECT_TRUE(pki.consume_acme_nonce(*nonce));
    EXPECT_FALSE(pki.consume_acme_nonce(*nonce));

    // A nonce past its lifetime, altered, foreign or empty is refused.
    auto stale = pki.new_acme_nonce();
    ASSERT_TRUE(stale);
    clock.advance(std::chrono::seconds(60));
    EXPECT_FALSE(pki.consume_acme_nonce(*stale));
    auto fresh = pki.new_acme_nonce();
    ASSERT_TRUE(fresh);
    std::string altered = *fresh;
    altered[20] = altered[20] == 'A' ? 'B' : 'A';
    EXPECT_FALSE(pki.consume_acme_nonce(altered));
    lake::control::pki::Service other(clock.limits());
    auto foreign = other.new_acme_nonce();
    ASSERT_TRUE(foreign);
    EXPECT_FALSE(pki.consume_acme_nonce(*foreign));
    EXPECT_FALSE(pki.consume_acme_nonce(""));
    EXPECT_TRUE(pki.consume_acme_nonce(*fresh));

    // Consumed nonces are remembered up to the cap; past it every older nonce is refused, so a
    // forgotten one is never accepted again.
    std::vector<std::string> nonces;
    for (int i = 0; i < 10; ++i) {
        auto issued = pki.new_acme_nonce();
        ASSERT_TRUE(issued);
        nonces.push_back(*issued);
    }
    for (std::size_t i = 1; i < nonces.size(); ++i) {
        EXPECT_TRUE(pki.consume_acme_nonce(nonces[i])) << i;
        EXPECT_LE(pki.acme_state_size().used_nonces, 4U);
    }
    EXPECT_FALSE(pki.consume_acme_nonce(nonces[0]));
    for (const std::string& used : nonces) {
        EXPECT_FALSE(pki.consume_acme_nonce(used));
    }
    auto after = pki.new_acme_nonce();
    ASSERT_TRUE(after);
    EXPECT_TRUE(pki.consume_acme_nonce(*after));

    // Remembered nonces are dropped once they could no longer be accepted anyway.
    clock.advance(std::chrono::seconds(60));
    EXPECT_EQ(pki.acme_state_size().used_nonces, 0U);
}

TEST(Pki, AcmeAccountsAndOrdersAreBoundedAndOrdersExpire) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    std::vector<std::string> customers;
    for (const std::string name : {"acme-one", "acme-two"}) {
        const auto organization = store->create_organization({"taap:1:ed25519:" + name, name, ""});
        ASSERT_TRUE(organization) << organization.error().message;
        const auto customer = store->create_customer({"organization", "", organization->id, name});
        ASSERT_TRUE(customer) << customer.error().message;
        customers.push_back(customer->id);
    }
    const AcmeTestClock clock;
    auto limits = clock.limits();
    limits.accounts_per_customer = 3;
    limits.accounts = 5;
    limits.orders_per_account = 2;
    limits.order_lifetime = std::chrono::seconds(60);
    limits.account_idle_lifetime = std::chrono::hours(1);
    lake::control::pki::Service pki(limits);
    ASSERT_TRUE(pki.create_authority(*store, customers[0], instance_kek));

    // Only an existing customer gets an account.
    const auto unknown =
        pki.create_acme_account(*store, lake::control::new_uuid_v4(), new_acme_jwk());
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, "not_found");
    EXPECT_EQ(pki.acme_state_size().accounts, 0U);

    // Past a customer's cap its least recently used account goes, with its orders.
    std::vector<std::string> accounts;
    for (int i = 0; i < 10; ++i) {
        const auto account = pki.create_acme_account(*store, customers[0], new_acme_jwk());
        ASSERT_TRUE(account) << account.error().message;
        accounts.push_back(account->id);
        clock.advance(std::chrono::seconds(1));
    }
    EXPECT_EQ(pki.acme_state_size().accounts, 3U);
    EXPECT_FALSE(
        pki.create_acme_order(*store, customers[0], accounts[0], "a.lake.test", instance_kek));

    // Past the global cap the least recently used account of any customer goes.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(pki.create_acme_account(*store, customers[1], new_acme_jwk()));
        clock.advance(std::chrono::seconds(1));
    }
    EXPECT_EQ(pki.acme_state_size().accounts, 5U);
    EXPECT_FALSE(
        pki.create_acme_order(*store, customers[0], accounts[7], "a.lake.test", instance_kek));

    // Past an account's cap its oldest order goes, and with it the HTTP-01 token.
    const std::string& account = accounts[9];
    std::vector<lake::control::pki::AcmeOrder> orders;
    std::vector<std::string> tokens;
    for (int i = 0; i < 5; ++i) {
        const auto order =
            pki.create_acme_order(*store, customers[0], account, "a.lake.test", instance_kek);
        ASSERT_TRUE(order) << order.error().message;
        const auto authorization =
            pki.acme_authorization(customers[0], account, order->authorization_id);
        ASSERT_TRUE(authorization) << authorization.error().message;
        orders.push_back(*order);
        tokens.push_back(authorization->token);
    }
    EXPECT_EQ(pki.acme_state_size().orders, 2U);
    EXPECT_EQ(pki.acme_state_size().authorizations, 2U);
    EXPECT_FALSE(pki.acme_order(customers[0], account, orders[0].id));
    EXPECT_FALSE(pki.acme_http01_response(tokens[0]));
    EXPECT_TRUE(pki.acme_http01_response(tokens[4]));

    // An order expires with its authorization and token.
    clock.advance(std::chrono::seconds(60));
    EXPECT_EQ(pki.acme_state_size().orders, 0U);
    EXPECT_EQ(pki.acme_state_size().authorizations, 0U);
    EXPECT_FALSE(pki.acme_order(customers[0], account, orders[4].id));
    EXPECT_FALSE(pki.acme_http01_response(tokens[4]));
    EXPECT_FALSE(pki.authorize_acme_order(customers[0], account, orders[4].authorization_id));

    // An idle account expires.
    clock.advance(std::chrono::hours(1));
    EXPECT_EQ(pki.acme_state_size().accounts, 0U);
    EXPECT_FALSE(pki.create_acme_order(*store, customers[0], account, "a.lake.test", instance_kek));
}

TEST(Pki, AcmeNewAccountForAnUnknownCustomerIsRefused) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ServerOptions options;
    options.port = 0;
    options.store = store;
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    AcmeClient client(server.port());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port()) + "/acme/" +
                             lake::control::new_uuid_v4();
    ASSERT_EQ(client.nonce(base + "/new-nonce").status, 200);
    const HttpResponse account =
        client.post(base + "/new-account", {{"termsOfServiceAgreed", true}}, true);
    EXPECT_EQ(account.status, 404) << account.body;
    EXPECT_TRUE(header(account, "Location").empty());
    server.stop();
}

TEST(PostgresPki, PersistsAndRewrapsCustomerAuthority) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto organization =
        store->create_organization({"taap:1:ed25519:pki-postgres-" + suffix, "PKI PostgreSQL", ""});
    ASSERT_TRUE(organization) << organization.error().message;
    const auto customer =
        store->create_customer({"organization", "", organization->id, "pki-pg-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

    lake::control::pki::Service creator;
    ASSERT_TRUE(creator.create_authority(*store, customer->id, instance_kek));
    lake::control::pki::Service after_restart;
    auto svid = after_restart.issue_x509_svid(
        *store, customer->id, "spiffe://lake.test/postgres", instance_kek);
    ASSERT_TRUE(svid) << svid.error().message;
    const auto authority = store->get_pki_authority(customer->id, instance_kek);
    ASSERT_TRUE(authority) << authority.error().message;
    EXPECT_TRUE(lake::control::pki::verify_x509_svid({authority->root_certificate_pem,
                                                      authority->intermediate_chain_pem,
                                                      authority->root_private_key_pem,
                                                      authority->intermediate_private_key_pem},
                                                     *svid));
    ASSERT_TRUE(store->rotate_customer_kek(customer->id, instance_kek));
    EXPECT_TRUE(after_restart.issue_jwt_svid(
        *store, customer->id, "spiffe://lake.test/postgres", "lake", instance_kek));

    const std::vector<std::string> participants{"taap:1:ed25519:pki-pg-a-" + suffix,
                                                "taap:1:ed25519:pki-pg-b-" + suffix,
                                                "taap:1:ed25519:pki-pg-c-" + suffix};
    auto ceremony =
        after_restart.begin_recovery(*store, customer->id, instance_kek, 2, participants);
    ASSERT_TRUE(ceremony) << ceremony.error().message;
    const auto pending = store->get_pki_recovery_ceremony(customer->id, ceremony->id);
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_FALSE(pending->recovered_at);
    ASSERT_EQ(pending->participants.size(), 3U);
    EXPECT_EQ(pending->participants[2].principal, participants[2]);

    // Each share is issued once, to its participant, by any process.
    lake::control::pki::Service completing_process;
    auto first = completing_process.claim_recovery_share(
        *store, customer->id, instance_kek, ceremony->id, participants[0]);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->index, 1);
    EXPECT_EQ(
        completing_process
            .claim_recovery_share(*store, customer->id, instance_kek, ceremony->id, participants[0])
            .error()
            .code,
        "conflict");
    EXPECT_EQ(completing_process
                  .claim_recovery_share(*store,
                                        customer->id,
                                        instance_kek,
                                        ceremony->id,
                                        "taap:1:ed25519:pki-pg-outsider-" + suffix)
                  .error()
                  .code,
              "forbidden");
    auto third = after_restart.claim_recovery_share(
        *store, customer->id, instance_kek, ceremony->id, participants[2]);
    ASSERT_TRUE(third) << third.error().message;
    EXPECT_EQ(third->index, 3);

    // One participant cannot present another's share, nor their own twice.
    EXPECT_EQ(completing_process
                  .submit_recovery_share(
                      *store, customer->id, instance_kek, ceremony->id, participants[0], *third)
                  .error()
                  .code,
              "forbidden");
    auto one = completing_process.submit_recovery_share(
        *store, customer->id, instance_kek, ceremony->id, participants[0], *first);
    ASSERT_TRUE(one) << one.error().message;
    EXPECT_FALSE(one->recovered);
    EXPECT_EQ(completing_process
                  .submit_recovery_share(
                      *store, customer->id, instance_kek, ceremony->id, participants[0], *first)
                  .error()
                  .code,
              "conflict");
    auto two = after_restart.submit_recovery_share(
        *store, customer->id, instance_kek, ceremony->id, participants[2], *third);
    ASSERT_TRUE(two) << two.error().message;
    EXPECT_TRUE(two->recovered);
    EXPECT_EQ(two->submitted, 2);
    const auto completed = store->get_pki_recovery_ceremony(customer->id, ceremony->id);
    ASSERT_TRUE(completed) << completed.error().message;
    EXPECT_TRUE(completed->recovered_at);
}

} // namespace
