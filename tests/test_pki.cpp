#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace {

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

std::string csr_for(std::string_view identifier) {
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
    X509_EXTENSION* san =
        X509V3_EXT_conf_nid(nullptr,
                            nullptr,
                            NID_subject_alt_name,
                            const_cast<char*>(("DNS:" + std::string(identifier)).c_str()));
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

TEST(Pki, IssuesAndVerifiesSpiffeX509AndJwtSvids) {
    auto authority = lake::control::pki::create_authority("customer-a");
    ASSERT_TRUE(authority) << authority.error().message;

    auto x509 = lake::control::pki::issue_x509_svid(*authority, "spiffe://lake.test/worker");
    ASSERT_TRUE(x509) << x509.error().message;
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

    context.request_id.clear();
    const auto ceremony =
        registry.call("begin_root_recovery",
                      {{"customer_id", customer->id}, {"threshold", 2}, {"share_count", 3}},
                      context);
    ASSERT_TRUE(ceremony.ok) << ceremony.error.message;
    const auto pending_ceremony = store.get_pki_recovery_ceremony(
        customer->id, ceremony.value.at("ceremony_id").get<std::string>());
    ASSERT_TRUE(pending_ceremony) << pending_ceremony.error().message;
    EXPECT_FALSE(pending_ceremony->recovered_at);
    const auto recovery = registry.call(
        "complete_root_recovery",
        {{"customer_id", customer->id},
         {"ceremony_id", ceremony.value.at("ceremony_id")},
         {"threshold", 2},
         {"shares", {ceremony.value.at("shares").at(0), ceremony.value.at("shares").at(1)}}},
        context);
    ASSERT_TRUE(recovery.ok) << recovery.error.message;
    const auto recovery_event = store.latest_audit_event();
    ASSERT_TRUE(recovery_event) << recovery_event.error().message;
    EXPECT_EQ(recovery_event->draft.kind, "recovery");
    EXPECT_EQ(recovery_event->draft.action, "complete_root_recovery");
    const auto completed_ceremony = store.get_pki_recovery_ceremony(
        customer->id, ceremony.value.at("ceremony_id").get<std::string>());
    ASSERT_TRUE(completed_ceremony) << completed_ceremony.error().message;
    EXPECT_TRUE(completed_ceremony->recovered_at);
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
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    AcmeClient client(server.port());
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string directory_url = origin + "/acme/" + customer->id + "/directory";
    const HttpResponse directory_response = client.get(directory_url);
    ASSERT_EQ(directory_response.status, 200);
    const nlohmann::json directory = nlohmann::json::parse(directory_response.body);

    const HttpResponse nonce_response = client.nonce(directory.at("newNonce").get<std::string>());
    ASSERT_EQ(nonce_response.status, 200);
    ASSERT_FALSE(header(nonce_response, "Replay-Nonce").empty());

    const HttpResponse account = client.post(
        directory.at("newAccount").get<std::string>(), {{"termsOfServiceAgreed", true}}, true);
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

    const HttpResponse challenged =
        client.post(challenge.at("url").get<std::string>(), nlohmann::json::object());
    ASSERT_EQ(challenged.status, 200) << challenged.body;
    EXPECT_EQ(nlohmann::json::parse(challenged.body).at("status"), "valid");

    const HttpResponse finalized = client.post(order_document.at("finalize").get<std::string>(),
                                               {{"csr", csr_for(identifier)}});
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

    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.action, "acme_certificate");
    server.stop();
}

TEST(PostgresPki, PersistsAndRewrapsCustomerAuthority) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
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

    auto ceremony = after_restart.begin_recovery(*store, customer->id, instance_kek, 2, 3);
    ASSERT_TRUE(ceremony) << ceremony.error().message;
    const auto pending = store->get_pki_recovery_ceremony(customer->id, ceremony->id);
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_FALSE(pending->recovered_at);
    const std::vector<lake::control::pki::ShamirShare> quorum{ceremony->shares.at(0),
                                                              ceremony->shares.at(2)};
    lake::control::pki::Service completing_process;
    ASSERT_TRUE(
        completing_process.recover_root(*store, customer->id, instance_kek, *ceremony, quorum));
    const auto completed = store->get_pki_recovery_ceremony(customer->id, ceremony->id);
    ASSERT_TRUE(completed) << completed.error().message;
    EXPECT_TRUE(completed->recovered_at);
}

} // namespace
