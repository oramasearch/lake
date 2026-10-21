#include "lake/control/pki.hpp"

#include "lake/control/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <nlohmann/json.hpp>

namespace lake::control::pki {
namespace {

using json = nlohmann::json;

struct BioDeleter {
    void operator()(BIO* value) const {
        BIO_free(value);
    }
};
struct PkeyDeleter {
    void operator()(EVP_PKEY* value) const {
        EVP_PKEY_free(value);
    }
};
struct X509Deleter {
    void operator()(X509* value) const {
        X509_free(value);
    }
};
struct StoreDeleter {
    void operator()(X509_STORE* value) const {
        X509_STORE_free(value);
    }
};
struct StoreContextDeleter {
    void operator()(X509_STORE_CTX* value) const {
        X509_STORE_CTX_free(value);
    }
};
struct StackDeleter {
    void operator()(STACK_OF(X509) * value) const {
        sk_X509_pop_free(value, X509_free);
    }
};
struct MdContextDeleter {
    void operator()(EVP_MD_CTX* value) const {
        EVP_MD_CTX_free(value);
    }
};
struct EcdsaSignatureDeleter {
    void operator()(ECDSA_SIG* value) const {
        ECDSA_SIG_free(value);
    }
};
struct RequestDeleter {
    void operator()(X509_REQ* value) const {
        X509_REQ_free(value);
    }
};

using Bio = std::unique_ptr<BIO, BioDeleter>;
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Certificate = std::unique_ptr<X509, X509Deleter>;
using Store = std::unique_ptr<X509_STORE, StoreDeleter>;
using StoreContext = std::unique_ptr<X509_STORE_CTX, StoreContextDeleter>;
using CertificateStack = std::unique_ptr<STACK_OF(X509), StackDeleter>;
using MdContext = std::unique_ptr<EVP_MD_CTX, MdContextDeleter>;
using EcdsaSignature = std::unique_ptr<ECDSA_SIG, EcdsaSignatureDeleter>;
using Request = std::unique_ptr<X509_REQ, RequestDeleter>;

Error crypto_error(std::string message) {
    return {"crypto_error", std::move(message)};
}

Result<std::string> base64url_encode(const unsigned char* value, std::size_t size) {
    if (size == 0) {
        return std::string{};
    }
    std::string encoded(4 * ((size + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), value, static_cast<int>(size));
    if (written < 0) {
        return std::unexpected(crypto_error("could not encode base64url"));
    }
    encoded.resize(static_cast<std::size_t>(written));
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    for (char& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    return encoded;
}

Result<std::vector<unsigned char>> base64url_decode(std::string value) {
    for (char& character : value) {
        if (character == '-') {
            character = '+';
        } else if (character == '_') {
            character = '/';
        }
    }
    while (value.size() % 4 != 0) {
        value += '=';
    }
    std::vector<unsigned char> decoded((value.size() / 4) * 3);
    const int written = EVP_DecodeBlock(decoded.data(),
                                        reinterpret_cast<const unsigned char*>(value.data()),
                                        static_cast<int>(value.size()));
    if (written < 0) {
        return std::unexpected(Error{"invalid_input", "invalid base64url value"});
    }
    std::size_t padding = 0;
    if (!value.empty() && value.ends_with('=')) {
        ++padding;
    }
    if (value.size() > 1 && value.ends_with("==")) {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(written) - padding);
    return decoded;
}

Result<std::string> base64_encode(const std::vector<unsigned char>& value) {
    if (value.empty()) {
        return std::string{};
    }
    std::string encoded(4 * ((value.size() + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                        value.data(),
                                        static_cast<int>(value.size()));
    if (written < 0) {
        return std::unexpected(crypto_error("could not encode OpenSSH value"));
    }
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

void append_u32(std::vector<unsigned char>& output, std::uint32_t value) {
    output.push_back(static_cast<unsigned char>(value >> 24));
    output.push_back(static_cast<unsigned char>(value >> 16));
    output.push_back(static_cast<unsigned char>(value >> 8));
    output.push_back(static_cast<unsigned char>(value));
}

void append_u64(std::vector<unsigned char>& output, std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value >> 32));
    append_u32(output, static_cast<std::uint32_t>(value));
}

void append_ssh_string(std::vector<unsigned char>& output,
                       const unsigned char* value,
                       std::size_t size) {
    append_u32(output, static_cast<std::uint32_t>(size));
    output.insert(output.end(), value, value + size);
}

void append_ssh_string(std::vector<unsigned char>& output, std::string_view value) {
    append_ssh_string(output, reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

void append_ssh_string(std::vector<unsigned char>& output,
                       const std::vector<unsigned char>& value) {
    append_ssh_string(output, value.data(), value.size());
}

Result<std::string_view> read_ssh_string(std::string_view& input) {
    if (input.size() < 4) {
        return std::unexpected(Error{"invalid_input", "OpenSSH public key is truncated"});
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    const std::uint32_t size = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                               (static_cast<std::uint32_t>(bytes[1]) << 16) |
                               (static_cast<std::uint32_t>(bytes[2]) << 8) |
                               static_cast<std::uint32_t>(bytes[3]);
    input.remove_prefix(4);
    if (size > input.size()) {
        return std::unexpected(Error{"invalid_input", "OpenSSH public key is truncated"});
    }
    const std::string_view value = input.substr(0, size);
    input.remove_prefix(size);
    return value;
}

std::vector<unsigned char> ssh_mpint(const BIGNUM* value) {
    std::vector<unsigned char> output(static_cast<std::size_t>(BN_num_bytes(value)));
    if (!output.empty()) {
        BN_bn2bin(value, output.data());
    }
    if (!output.empty() && (output.front() & 0x80) != 0) {
        output.insert(output.begin(), 0);
    }
    return output;
}

Result<Pkey> new_p256_key() {
    Pkey context_key(EVP_PKEY_new());
    EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!raw_context) {
        return std::unexpected(crypto_error("could not allocate EC key context"));
    }
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(raw_context,
                                                                        EVP_PKEY_CTX_free);
    if (EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) != 1) {
        return std::unexpected(crypto_error("could not initialize P-256 key generation"));
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw_key) != 1) {
        return std::unexpected(crypto_error("could not generate P-256 key"));
    }
    return Pkey(raw_key);
}

Result<std::string> pem_of_private_key(EVP_PKEY* key) {
    Bio output(BIO_new(BIO_s_mem()));
    if (!output ||
        PEM_write_bio_PrivateKey(output.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return std::unexpected(crypto_error("could not encode private key"));
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(output.get(), &data);
    if (length <= 0) {
        return std::unexpected(crypto_error("private key encoding was empty"));
    }
    return std::string(data, static_cast<std::size_t>(length));
}

Result<Pkey> private_key_from_pem(std::string_view pem) {
    Bio input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!input) {
        return std::unexpected(crypto_error("could not allocate private key input"));
    }
    EVP_PKEY* raw_key = PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr);
    if (!raw_key) {
        return std::unexpected(Error{"invalid_input", "invalid private key PEM"});
    }
    return Pkey(raw_key);
}

Result<Certificate> certificate_from_pem(std::string_view pem) {
    Bio input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!input) {
        return std::unexpected(crypto_error("could not allocate certificate input"));
    }
    X509* raw_certificate = PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr);
    if (!raw_certificate) {
        return std::unexpected(Error{"invalid_input", "invalid certificate PEM"});
    }
    return Certificate(raw_certificate);
}

Result<std::string> pem_of_certificate(X509* certificate) {
    Bio output(BIO_new(BIO_s_mem()));
    if (!output || PEM_write_bio_X509(output.get(), certificate) != 1) {
        return std::unexpected(crypto_error("could not encode certificate"));
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(output.get(), &data);
    if (length <= 0) {
        return std::unexpected(crypto_error("certificate encoding was empty"));
    }
    return std::string(data, static_cast<std::size_t>(length));
}

Result<void> add_extension(X509* certificate, int nid, std::string_view value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension =
        X509V3_EXT_conf_nid(nullptr, &context, nid, const_cast<char*>(value.data()));
    if (!extension) {
        return std::unexpected(crypto_error("could not add certificate extension"));
    }
    const int added = X509_add_ext(certificate, extension, -1);
    X509_EXTENSION_free(extension);
    if (added != 1) {
        return std::unexpected(crypto_error("could not retain certificate extension"));
    }
    return {};
}

bool has_tunnel_identity(X509* certificate) {
    int index = -1;
    while ((index = X509_get_ext_by_NID(certificate, NID_subject_alt_name, index)) >= 0) {
        auto* extension = X509_get_ext(certificate, index);
        std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(
            static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(extension)), GENERAL_NAMES_free);
        if (!names) {
            continue;
        }
        for (int item = 0; item < sk_GENERAL_NAME_num(names.get()); ++item) {
            const auto* name = sk_GENERAL_NAME_value(names.get(), item);
            if (name->type != GEN_URI) {
                continue;
            }
            const auto* uri = name->d.uniformResourceIdentifier;
            const std::string_view value(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),
                                         static_cast<std::size_t>(ASN1_STRING_length(uri)));
            if (value.starts_with("urn:orama:tunnel-grant:")) {
                return true;
            }
        }
    }
    return false;
}

Result<Certificate> make_certificate(EVP_PKEY* subject_key,
                                     EVP_PKEY* issuer_key,
                                     X509* issuer_certificate,
                                     std::string_view common_name,
                                     int lifetime_seconds,
                                     bool is_authority,
                                     std::string_view uri_san = {},
                                     std::string_view dns_san = {},
                                     bool tunnel_identity = false) {
    Certificate certificate(X509_new());
    if (!certificate) {
        return std::unexpected(crypto_error("could not allocate certificate"));
    }
    if (X509_set_version(certificate.get(), 2) != 1) {
        return std::unexpected(crypto_error("could not set certificate version"));
    }
    std::array<unsigned char, 8> serial_bytes{};
    if (RAND_bytes(serial_bytes.data(), static_cast<int>(serial_bytes.size())) != 1) {
        return std::unexpected(crypto_error("could not generate certificate serial"));
    }
    BIGNUM* serial_number =
        BN_bin2bn(serial_bytes.data(), static_cast<int>(serial_bytes.size()), nullptr);
    if (!serial_number ||
        BN_to_ASN1_INTEGER(serial_number, X509_get_serialNumber(certificate.get())) == nullptr) {
        BN_free(serial_number);
        return std::unexpected(crypto_error("could not assign certificate serial"));
    }
    BN_free(serial_number);
    if (!X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), lifetime_seconds) ||
        X509_set_pubkey(certificate.get(), subject_key) != 1) {
        return std::unexpected(crypto_error("could not set certificate validity"));
    }
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (X509_NAME_add_entry_by_txt(subject,
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(common_name.data()),
                                   static_cast<int>(common_name.size()),
                                   -1,
                                   0) != 1) {
        return std::unexpected(crypto_error("could not set certificate subject"));
    }
    if (issuer_certificate) {
        if (X509_set_issuer_name(certificate.get(), X509_get_subject_name(issuer_certificate)) !=
            1) {
            return std::unexpected(crypto_error("could not set certificate issuer"));
        }
    } else if (X509_set_issuer_name(certificate.get(), subject) != 1) {
        return std::unexpected(crypto_error("could not self-issue root certificate"));
    }
    const auto basic = add_extension(certificate.get(),
                                     NID_basic_constraints,
                                     is_authority ? "critical,CA:TRUE" : "critical,CA:FALSE");
    if (!basic) {
        return std::unexpected(basic.error());
    }
    const auto usage = add_extension(certificate.get(),
                                     NID_key_usage,
                                     is_authority ? "critical,keyCertSign,cRLSign"
                                                  : "critical,digitalSignature,keyEncipherment");
    if (!usage) {
        return std::unexpected(usage.error());
    }
    if (!uri_san.empty()) {
        const std::string san = "URI:" + std::string(uri_san);
        const auto extension = add_extension(certificate.get(), NID_subject_alt_name, san);
        if (!extension) {
            return std::unexpected(extension.error());
        }
    }
    if (!dns_san.empty()) {
        const std::string san = "DNS:" + std::string(dns_san);
        const auto extension = add_extension(certificate.get(), NID_subject_alt_name, san);
        if (!extension) {
            return std::unexpected(extension.error());
        }
    }
    // The customer CA is shared by PKI profiles. Neither an agent identity nor another SAN
    // profile may mint a grant identity outside the recipient-only tunnel signer. Inspect the
    // encoded names, including extra names accepted by OpenSSL's extension configuration parser.
    if (!tunnel_identity && has_tunnel_identity(certificate.get())) {
        return std::unexpected(Error{
            "invalid_input", "tunnel grant identities require the recipient-only CSR capability"});
    }
    if (X509_sign(certificate.get(), issuer_key ? issuer_key : subject_key, EVP_sha256()) <= 0) {
        return std::unexpected(crypto_error("could not sign certificate"));
    }
    return certificate;
}

std::uint8_t gf_multiply(std::uint8_t left, std::uint8_t right) {
    std::uint8_t product = 0;
    while (right != 0) {
        if ((right & 1) != 0) {
            product ^= left;
        }
        const bool high = (left & 0x80) != 0;
        left <<= 1;
        if (high) {
            left ^= 0x1b;
        }
        right >>= 1;
    }
    return product;
}

std::uint8_t gf_power(std::uint8_t value, int exponent) {
    std::uint8_t result = 1;
    while (exponent > 0) {
        if ((exponent & 1) != 0) {
            result = gf_multiply(result, value);
        }
        value = gf_multiply(value, value);
        exponent >>= 1;
    }
    return result;
}

std::uint8_t gf_inverse(std::uint8_t value) {
    return gf_power(value, 254);
}

Result<std::vector<unsigned char>> sign_jws(EVP_PKEY* key, std::string_view input) {
    MdContext context(EVP_MD_CTX_new());
    if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        return std::unexpected(crypto_error("could not initialize JWT signature"));
    }
    std::size_t der_size = 0;
    if (EVP_DigestSign(context.get(),
                       nullptr,
                       &der_size,
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size()) != 1) {
        return std::unexpected(crypto_error("could not determine JWT signature size"));
    }
    std::vector<unsigned char> der(der_size);
    if (EVP_DigestSign(context.get(),
                       der.data(),
                       &der_size,
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size()) != 1) {
        return std::unexpected(crypto_error("could not sign JWT"));
    }
    der.resize(der_size);
    const unsigned char* cursor = der.data();
    EcdsaSignature signature(d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size())));
    if (!signature) {
        return std::unexpected(crypto_error("could not decode JWT signature"));
    }
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(signature.get(), &r, &s);
    std::vector<unsigned char> raw(64);
    if (BN_bn2binpad(r, raw.data(), 32) != 32 || BN_bn2binpad(s, raw.data() + 32, 32) != 32) {
        return std::unexpected(crypto_error("could not encode JWT signature"));
    }
    return raw;
}

Result<void>
verify_jws(EVP_PKEY* key, std::string_view input, const std::vector<unsigned char>& raw) {
    if (raw.size() != 64) {
        return std::unexpected(Error{"invalid_input", "JWT signature must be 64 bytes"});
    }
    BIGNUM* r = BN_bin2bn(raw.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(raw.data() + 32, 32, nullptr);
    EcdsaSignature signature(ECDSA_SIG_new());
    if (!r || !s || !signature || ECDSA_SIG_set0(signature.get(), r, s) != 1) {
        BN_free(r);
        BN_free(s);
        return std::unexpected(crypto_error("could not decode JWT signature"));
    }
    r = nullptr;
    s = nullptr;
    const int der_size = i2d_ECDSA_SIG(signature.get(), nullptr);
    if (der_size <= 0) {
        return std::unexpected(crypto_error("could not encode JWT signature"));
    }
    std::vector<unsigned char> der(static_cast<std::size_t>(der_size));
    unsigned char* cursor = der.data();
    if (i2d_ECDSA_SIG(signature.get(), &cursor) != der_size) {
        return std::unexpected(crypto_error("could not encode JWT signature"));
    }
    MdContext context(EVP_MD_CTX_new());
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        return std::unexpected(crypto_error("could not initialize JWT verification"));
    }
    if (EVP_DigestVerify(context.get(),
                         der.data(),
                         der.size(),
                         reinterpret_cast<const unsigned char*>(input.data()),
                         input.size()) != 1) {
        return std::unexpected(Error{"unauthorized", "JWT signature does not verify"});
    }
    return {};
}

Result<Pkey> key_from_acme_jwk(const AcmeJwk& jwk) {
    auto x = base64url_decode(jwk.x);
    auto y = base64url_decode(jwk.y);
    if (!x || !y || x->size() != 32 || y->size() != 32) {
        return std::unexpected(Error{"invalid_input", "ACME account JWK must be P-256"});
    }
    EC_KEY* raw_ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!raw_ec) {
        return std::unexpected(crypto_error("could not allocate ACME account key"));
    }
    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ec(raw_ec, EC_KEY_free);
    const EC_GROUP* group = EC_KEY_get0_group(ec.get());
    EC_POINT* raw_point = EC_POINT_new(group);
    if (!raw_point) {
        return std::unexpected(crypto_error("could not allocate ACME account point"));
    }
    std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)> point(raw_point, EC_POINT_free);
    std::array<unsigned char, 65> encoded{};
    encoded[0] = POINT_CONVERSION_UNCOMPRESSED;
    std::copy(x->begin(), x->end(), encoded.begin() + 1);
    std::copy(y->begin(), y->end(), encoded.begin() + 33);
    if (EC_POINT_oct2point(group, point.get(), encoded.data(), encoded.size(), nullptr) != 1 ||
        EC_KEY_set_public_key(ec.get(), point.get()) != 1 || EC_KEY_check_key(ec.get()) != 1) {
        return std::unexpected(Error{"invalid_input", "ACME account JWK is invalid"});
    }
    Pkey key(EVP_PKEY_new());
    if (!key || EVP_PKEY_assign_EC_KEY(key.get(), ec.release()) != 1) {
        return std::unexpected(crypto_error("could not retain ACME account key"));
    }
    return key;
}

Result<std::string> acme_thumbprint(const AcmeJwk& jwk) {
    const std::string canonical =
        json{{"crv", "P-256"}, {"kty", "EC"}, {"x", jwk.x}, {"y", jwk.y}}.dump();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_size = 0;
    if (EVP_Digest(canonical.data(),
                   canonical.size(),
                   digest.data(),
                   &digest_size,
                   EVP_sha256(),
                   nullptr) != 1) {
        return std::unexpected(crypto_error("could not hash ACME account key"));
    }
    return base64url_encode(digest.data(), digest_size);
}

void put_big_endian(char* out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

std::uint64_t get_big_endian(const unsigned char* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | in[i];
    }
    return value;
}

Result<std::string> random_acme_value(std::size_t size) {
    std::vector<unsigned char> bytes(size);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return std::unexpected(crypto_error("could not create ACME random value"));
    }
    return base64url_encode(bytes.data(), bytes.size());
}

Result<X509Svid> issue_acme_certificate(const Authority& authority,
                                        std::string_view identifier,
                                        std::string_view encoded_csr) {
    auto csr_bytes = base64url_decode(std::string(encoded_csr));
    if (!csr_bytes || csr_bytes->empty()) {
        return std::unexpected(Error{"invalid_input", "ACME finalize requires a DER CSR"});
    }
    const unsigned char* cursor = csr_bytes->data();
    Request request(d2i_X509_REQ(nullptr, &cursor, static_cast<long>(csr_bytes->size())));
    if (!request || cursor != csr_bytes->data() + csr_bytes->size()) {
        return std::unexpected(Error{"invalid_input", "ACME CSR is invalid"});
    }
    Pkey subject_key(X509_REQ_get_pubkey(request.get()));
    if (!subject_key || X509_REQ_verify(request.get(), subject_key.get()) != 1) {
        return std::unexpected(Error{"unauthorized", "ACME CSR signature does not verify"});
    }
    bool names_identifier = false;
    STACK_OF(X509_EXTENSION)* extensions = X509_REQ_get_extensions(request.get());
    if (extensions) {
        const int count = sk_X509_EXTENSION_num(extensions);
        for (int index = 0; index < count; ++index) {
            X509_EXTENSION* extension = sk_X509_EXTENSION_value(extensions, index);
            if (OBJ_obj2nid(X509_EXTENSION_get_object(extension)) != NID_subject_alt_name) {
                continue;
            }
            GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(extension));
            if (!names) {
                continue;
            }
            const int name_count = sk_GENERAL_NAME_num(names);
            for (int name_index = 0; name_index < name_count; ++name_index) {
                const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, name_index);
                if (name->type != GEN_DNS) {
                    continue;
                }
                const unsigned char* data = ASN1_STRING_get0_data(name->d.dNSName);
                const int length = ASN1_STRING_length(name->d.dNSName);
                names_identifier = std::string_view(reinterpret_cast<const char*>(data),
                                                    static_cast<std::size_t>(length)) == identifier;
                if (names_identifier) {
                    break;
                }
            }
            GENERAL_NAMES_free(names);
        }
        sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
    }
    if (!names_identifier) {
        return std::unexpected(
            Error{"unauthorized", "ACME CSR does not contain the ordered DNS identifier"});
    }
    auto intermediate_key = private_key_from_pem(authority.intermediate_private_key_pem);
    auto intermediate_certificate = certificate_from_pem(authority.intermediate_certificate_pem);
    if (!intermediate_key || !intermediate_certificate) {
        return std::unexpected(Error{"invalid_input", "authority material is invalid"});
    }
    auto certificate = make_certificate(subject_key.get(),
                                        intermediate_key->get(),
                                        intermediate_certificate->get(),
                                        identifier,
                                        3600,
                                        false,
                                        {},
                                        identifier);
    if (!certificate) {
        return std::unexpected(certificate.error());
    }
    auto pem = pem_of_certificate(certificate->get());
    if (!pem) {
        return std::unexpected(pem.error());
    }
    return X509Svid{
        *pem, {}, authority.intermediate_certificate_pem + authority.root_certificate_pem};
}

} // namespace

Result<Authority> create_authority(std::string_view customer_id) {
    if (customer_id.empty()) {
        return std::unexpected(Error{"invalid_input", "customer id is required"});
    }
    auto root_key = new_p256_key();
    if (!root_key) {
        return std::unexpected(root_key.error());
    }
    auto root_certificate = make_certificate(root_key->get(),
                                             nullptr,
                                             nullptr,
                                             "lake root " + std::string(customer_id),
                                             10 * 365 * 24 * 60 * 60,
                                             true);
    if (!root_certificate) {
        return std::unexpected(root_certificate.error());
    }
    auto intermediate_key = new_p256_key();
    if (!intermediate_key) {
        return std::unexpected(intermediate_key.error());
    }
    auto intermediate_certificate =
        make_certificate(intermediate_key->get(),
                         root_key->get(),
                         root_certificate->get(),
                         "lake intermediate " + std::string(customer_id),
                         365 * 24 * 60 * 60,
                         true);
    if (!intermediate_certificate) {
        return std::unexpected(intermediate_certificate.error());
    }
    auto root_pem = pem_of_certificate(root_certificate->get());
    auto intermediate_pem = pem_of_certificate(intermediate_certificate->get());
    auto root_key_pem = pem_of_private_key(root_key->get());
    auto intermediate_key_pem = pem_of_private_key(intermediate_key->get());
    if (!root_pem || !intermediate_pem || !root_key_pem || !intermediate_key_pem) {
        return std::unexpected(crypto_error("could not encode authority material"));
    }
    return Authority{*root_pem, *intermediate_pem, *root_key_pem, *intermediate_key_pem};
}

Result<X509Svid>
issue_x509_svid(const Authority& authority, std::string_view spiffe_id, int lifetime_seconds) {
    if (!spiffe_id.starts_with("spiffe://") || lifetime_seconds <= 0 || lifetime_seconds > 3600) {
        return std::unexpected(
            Error{"invalid_input", "SVID must use SPIFFE and a lifetime up to one hour"});
    }
    auto intermediate_key = private_key_from_pem(authority.intermediate_private_key_pem);
    auto intermediate_certificate = certificate_from_pem(authority.intermediate_certificate_pem);
    auto leaf_key = new_p256_key();
    if (!intermediate_key || !intermediate_certificate || !leaf_key) {
        return std::unexpected(Error{"invalid_input", "authority material is invalid"});
    }
    auto leaf_certificate = make_certificate(leaf_key->get(),
                                             intermediate_key->get(),
                                             intermediate_certificate->get(),
                                             spiffe_id,
                                             lifetime_seconds,
                                             false,
                                             spiffe_id);
    if (!leaf_certificate) {
        return std::unexpected(leaf_certificate.error());
    }
    auto certificate_pem = pem_of_certificate(leaf_certificate->get());
    auto private_key_pem = pem_of_private_key(leaf_key->get());
    if (!certificate_pem || !private_key_pem) {
        return std::unexpected(crypto_error("could not encode SVID"));
    }
    return X509Svid{*certificate_pem,
                    *private_key_pem,
                    authority.intermediate_certificate_pem + authority.root_certificate_pem};
}

Result<X509Svid>
issue_x509_certificate(const Authority& authority, std::string_view subject, int lifetime_seconds) {
    if (subject.empty() || lifetime_seconds <= 0 || lifetime_seconds > 3600) {
        return std::unexpected(Error{
            "invalid_input", "certificate subject and a lifetime up to one hour are required"});
    }
    auto intermediate_key = private_key_from_pem(authority.intermediate_private_key_pem);
    auto intermediate_certificate = certificate_from_pem(authority.intermediate_certificate_pem);
    auto leaf_key = new_p256_key();
    if (!intermediate_key || !intermediate_certificate || !leaf_key) {
        return std::unexpected(Error{"invalid_input", "authority material is invalid"});
    }
    auto leaf_certificate = make_certificate(leaf_key->get(),
                                             intermediate_key->get(),
                                             intermediate_certificate->get(),
                                             subject,
                                             lifetime_seconds,
                                             false);
    if (!leaf_certificate) {
        return std::unexpected(leaf_certificate.error());
    }
    auto certificate_pem = pem_of_certificate(leaf_certificate->get());
    auto private_key_pem = pem_of_private_key(leaf_key->get());
    if (!certificate_pem || !private_key_pem) {
        return std::unexpected(crypto_error("could not encode certificate"));
    }
    return X509Svid{*certificate_pem,
                    *private_key_pem,
                    authority.intermediate_certificate_pem + authority.root_certificate_pem};
}

Result<X509Svid> issue_agent_certificate(const Authority& authority,
                                         std::string_view subject,
                                         std::string_view public_key,
                                         int lifetime_seconds) {
    if (subject.empty() || lifetime_seconds <= 0 || lifetime_seconds > 3600) {
        return std::unexpected(Error{
            "invalid_input", "certificate subject and a lifetime up to one hour are required"});
    }
    auto public_key_bytes = base64url_decode(std::string(public_key));
    if (!public_key_bytes || public_key_bytes->size() != 32) {
        return std::unexpected(
            Error{"invalid_input", "agent TAAP public key must be 32-byte base64url Ed25519"});
    }
    Pkey leaf_key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key_bytes->data(), public_key_bytes->size()));
    auto intermediate_key = private_key_from_pem(authority.intermediate_private_key_pem);
    auto intermediate_certificate = certificate_from_pem(authority.intermediate_certificate_pem);
    if (!leaf_key || !intermediate_key || !intermediate_certificate) {
        return std::unexpected(
            Error{"invalid_input", "authority or agent key material is invalid"});
    }
    auto leaf_certificate = make_certificate(leaf_key.get(),
                                             intermediate_key->get(),
                                             intermediate_certificate->get(),
                                             "lake agent",
                                             lifetime_seconds,
                                             false,
                                             subject);
    if (!leaf_certificate) {
        return std::unexpected(leaf_certificate.error());
    }
    auto certificate_pem = pem_of_certificate(leaf_certificate->get());
    if (!certificate_pem) {
        return std::unexpected(certificate_pem.error());
    }
    return X509Svid{*certificate_pem,
                    {},
                    authority.intermediate_certificate_pem + authority.root_certificate_pem};
}

Result<void> verify_x509_svid(const Authority& authority, const X509Svid& svid) {
    auto root = certificate_from_pem(authority.root_certificate_pem);
    auto intermediate = certificate_from_pem(authority.intermediate_certificate_pem);
    auto leaf = certificate_from_pem(svid.certificate_pem);
    if (!root || !intermediate || !leaf) {
        return std::unexpected(Error{"invalid_input", "certificate chain is invalid"});
    }
    Store store(X509_STORE_new());
    StoreContext context(X509_STORE_CTX_new());
    CertificateStack chain(sk_X509_new_null());
    if (!store || !context || !chain || X509_STORE_add_cert(store.get(), root->get()) != 1 ||
        sk_X509_push(chain.get(), intermediate->release()) == 0 ||
        X509_STORE_CTX_init(context.get(), store.get(), leaf->get(), chain.get()) != 1) {
        return std::unexpected(crypto_error("could not initialize certificate verification"));
    }
    if (X509_verify_cert(context.get()) != 1) {
        return std::unexpected(Error{"unauthorized", "SVID does not verify against authority"});
    }
    return {};
}

Result<JwtSvid> issue_jwt_svid(const Authority& authority,
                               std::string_view spiffe_id,
                               std::string_view audience,
                               int lifetime_seconds) {
    if (!spiffe_id.starts_with("spiffe://") || audience.empty() || lifetime_seconds <= 0 ||
        lifetime_seconds > 900) {
        return std::unexpected(Error{"invalid_input", "JWT SVID input is invalid"});
    }
    auto key = private_key_from_pem(authority.intermediate_private_key_pem);
    if (!key) {
        return std::unexpected(key.error());
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    auto header = base64url_encode(
        reinterpret_cast<const unsigned char*>("{\"alg\":\"ES256\",\"typ\":\"JWT\"}"), 27);
    const std::string claims = json{
        {"aud", audience},
        {"exp", now + lifetime_seconds},
        {"iat", now},
        {"sub", spiffe_id}}.dump();
    auto payload =
        base64url_encode(reinterpret_cast<const unsigned char*>(claims.data()), claims.size());
    if (!header || !payload) {
        return std::unexpected(crypto_error("could not encode JWT SVID"));
    }
    const std::string signing_input = *header + "." + *payload;
    auto signature = sign_jws(key->get(), signing_input);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    auto encoded_signature = base64url_encode(signature->data(), signature->size());
    if (!encoded_signature) {
        return std::unexpected(encoded_signature.error());
    }
    return JwtSvid{signing_input + "." + *encoded_signature, "lake", std::string(spiffe_id)};
}

Result<void> verify_jwt_svid(const Authority& authority, const JwtSvid& svid) {
    const std::size_t first = svid.token.find('.');
    const std::size_t second = svid.token.find('.', first == std::string::npos ? first : first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        svid.token.find('.', second + 1) != std::string::npos) {
        return std::unexpected(Error{"invalid_input", "JWT SVID has invalid compact form"});
    }
    auto intermediate = certificate_from_pem(authority.intermediate_certificate_pem);
    if (!intermediate) {
        return std::unexpected(intermediate.error());
    }
    Pkey public_key(X509_get_pubkey(intermediate->get()));
    auto signature = base64url_decode(svid.token.substr(second + 1));
    if (!public_key || !signature) {
        return std::unexpected(Error{"invalid_input", "JWT SVID has invalid signing material"});
    }
    auto verified = verify_jws(public_key.get(), svid.token.substr(0, second), *signature);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    auto payload = base64url_decode(svid.token.substr(first + 1, second - first - 1));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    json claims = json::parse(payload->begin(), payload->end(), nullptr, false);
    if (claims.is_discarded() || claims.value("sub", "") != svid.subject ||
        claims.value("exp", std::int64_t{0}) <=
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()) {
        return std::unexpected(Error{"unauthorized", "JWT SVID claims are invalid or expired"});
    }
    return {};
}

Result<SshCertificate> issue_ssh_certificate(const Authority& authority,
                                             std::string_view public_key,
                                             std::string_view key_id,
                                             const std::vector<std::string>& principals,
                                             bool host,
                                             int lifetime_seconds) {
    if (key_id.empty() || principals.empty() || lifetime_seconds <= 0 || lifetime_seconds > 3600) {
        return std::unexpected(Error{
            "invalid_input", "SSH key id, principals, and a lifetime up to one hour are required"});
    }
    if (std::ranges::any_of(principals, [](const std::string& value) { return value.empty(); })) {
        return std::unexpected(Error{"invalid_input", "SSH principals must not be empty"});
    }
    const std::size_t algorithm_end = public_key.find(' ');
    const std::size_t encoded_end =
        public_key.find(' ', algorithm_end == std::string_view::npos ? 0 : algorithm_end + 1);
    if (algorithm_end == std::string_view::npos ||
        public_key.substr(0, algorithm_end) != "ssh-ed25519") {
        return std::unexpected(
            Error{"invalid_input", "SSH certificate subjects must use an Ed25519 public key"});
    }
    const std::string encoded_key(public_key.substr(algorithm_end + 1,
                                                    encoded_end == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : encoded_end - algorithm_end - 1));
    auto decoded_key = base64url_decode(encoded_key);
    if (!decoded_key) {
        return std::unexpected(decoded_key.error());
    }
    std::string_view key_blob(reinterpret_cast<const char*>(decoded_key->data()),
                              decoded_key->size());
    auto key_algorithm = read_ssh_string(key_blob);
    auto key_value = read_ssh_string(key_blob);
    if (!key_algorithm || !key_value || *key_algorithm != "ssh-ed25519" ||
        key_value->size() != 32 || !key_blob.empty()) {
        return std::unexpected(Error{"invalid_input", "Ed25519 public key is malformed"});
    }

    auto signing_key = private_key_from_pem(authority.intermediate_private_key_pem);
    if (!signing_key) {
        return std::unexpected(signing_key.error());
    }
    std::array<unsigned char, 65> ca_point{};
    std::size_t ca_point_size = ca_point.size();
    if (EVP_PKEY_get_octet_string_param(signing_key->get(),
                                        OSSL_PKEY_PARAM_PUB_KEY,
                                        ca_point.data(),
                                        ca_point.size(),
                                        &ca_point_size) != 1 ||
        ca_point_size != ca_point.size()) {
        return std::unexpected(crypto_error("could not encode SSH CA public key"));
    }
    constexpr std::string_view ca_algorithm = "ecdsa-sha2-nistp256";
    std::vector<unsigned char> ca_blob;
    append_ssh_string(ca_blob, ca_algorithm);
    append_ssh_string(ca_blob, "nistp256");
    append_ssh_string(ca_blob, ca_point.data(), ca_point.size());

    constexpr std::string_view certificate_algorithm = "ssh-ed25519-cert-v01@openssh.com";
    std::array<unsigned char, 32> nonce{};
    std::uint64_t serial = 0;
    if (RAND_bytes(nonce.data(), nonce.size()) != 1 ||
        RAND_bytes(reinterpret_cast<unsigned char*>(&serial), sizeof(serial)) != 1) {
        return std::unexpected(crypto_error("could not create SSH certificate identifiers"));
    }
    std::vector<unsigned char> principal_blob;
    for (const std::string& principal : principals) {
        append_ssh_string(principal_blob, principal);
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::vector<unsigned char> certificate_blob;
    append_ssh_string(certificate_blob, certificate_algorithm);
    append_ssh_string(certificate_blob, nonce.data(), nonce.size());
    append_ssh_string(certificate_blob,
                      reinterpret_cast<const unsigned char*>(key_value->data()),
                      key_value->size());
    append_u64(certificate_blob, serial);
    append_u32(certificate_blob, host ? 2 : 1);
    append_ssh_string(certificate_blob, key_id);
    append_ssh_string(certificate_blob, principal_blob);
    append_u64(certificate_blob, static_cast<std::uint64_t>(now - 60));
    append_u64(certificate_blob, static_cast<std::uint64_t>(now + lifetime_seconds));
    append_ssh_string(certificate_blob, "");
    append_ssh_string(certificate_blob, "");
    append_ssh_string(certificate_blob, "");
    append_ssh_string(certificate_blob, ca_blob);

    auto raw_signature =
        sign_jws(signing_key->get(),
                 std::string_view(reinterpret_cast<const char*>(certificate_blob.data()),
                                  certificate_blob.size()));
    if (!raw_signature) {
        return std::unexpected(raw_signature.error());
    }
    std::unique_ptr<BIGNUM, decltype(&BN_free)> r(BN_bin2bn(raw_signature->data(), 32, nullptr),
                                                  BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> s(
        BN_bin2bn(raw_signature->data() + 32, 32, nullptr), BN_free);
    if (!r || !s) {
        return std::unexpected(crypto_error("could not encode SSH certificate signature"));
    }
    std::vector<unsigned char> ecdsa_signature;
    append_ssh_string(ecdsa_signature, ssh_mpint(r.get()));
    append_ssh_string(ecdsa_signature, ssh_mpint(s.get()));
    std::vector<unsigned char> signature_blob;
    append_ssh_string(signature_blob, ca_algorithm);
    append_ssh_string(signature_blob, ecdsa_signature);
    append_ssh_string(certificate_blob, signature_blob);

    auto encoded_certificate = base64_encode(certificate_blob);
    auto encoded_ca = base64_encode(ca_blob);
    if (!encoded_certificate || !encoded_ca) {
        return std::unexpected(crypto_error("could not encode SSH certificate"));
    }
    return SshCertificate{std::string(certificate_algorithm) + " " + *encoded_certificate + " " +
                              std::string(key_id),
                          std::string(ca_algorithm) + " " + *encoded_ca + " lake-ssh-ca",
                          serial};
}

Result<std::vector<ShamirShare>>
split_secret(const std::vector<std::byte>& secret, int threshold, int share_count) {
    if (secret.empty() || threshold < 2 || share_count < threshold || share_count > 255) {
        return std::unexpected(Error{"invalid_input", "invalid Shamir threshold or share count"});
    }
    std::vector<std::vector<std::uint8_t>> coefficients(secret.size(),
                                                        std::vector<std::uint8_t>(threshold));
    for (std::size_t offset = 0; offset < secret.size(); ++offset) {
        coefficients[offset][0] = std::to_integer<std::uint8_t>(secret[offset]);
        if (RAND_bytes(coefficients[offset].data() + 1, threshold - 1) != 1) {
            return std::unexpected(crypto_error("could not create recovery shares"));
        }
    }
    std::vector<ShamirShare> shares;
    shares.reserve(static_cast<std::size_t>(share_count));
    for (int index = 1; index <= share_count; ++index) {
        ShamirShare share;
        share.index = index;
        share.value.resize(secret.size());
        for (std::size_t offset = 0; offset < secret.size(); ++offset) {
            std::uint8_t value = 0;
            std::uint8_t x_power = 1;
            for (const std::uint8_t coefficient : coefficients[offset]) {
                value ^= gf_multiply(coefficient, x_power);
                x_power = gf_multiply(x_power, static_cast<std::uint8_t>(index));
            }
            share.value[offset] = static_cast<std::byte>(value);
        }
        shares.push_back(std::move(share));
    }
    return shares;
}

Result<std::vector<std::byte>> recover_secret(const std::vector<ShamirShare>& shares,
                                              int threshold) {
    if (threshold < 2 || static_cast<int>(shares.size()) < threshold) {
        return std::unexpected(Error{"invalid_input", "recovery quorum is not present"});
    }
    const std::size_t length = shares.front().value.size();
    std::array<bool, 256> seen{};
    for (const ShamirShare& share : shares) {
        if (share.index < 1 || share.index > 255 || share.value.size() != length ||
            seen[share.index]) {
            return std::unexpected(Error{"invalid_input", "recovery shares are malformed"});
        }
        seen[share.index] = true;
    }
    std::vector<std::byte> secret(length);
    for (std::size_t offset = 0; offset < length; ++offset) {
        std::uint8_t value = 0;
        for (const ShamirShare& current : shares) {
            std::uint8_t basis = 1;
            for (const ShamirShare& other : shares) {
                if (current.index == other.index) {
                    continue;
                }
                basis = gf_multiply(basis, static_cast<std::uint8_t>(other.index));
                basis = gf_multiply(
                    basis, gf_inverse(static_cast<std::uint8_t>(other.index ^ current.index)));
            }
            value ^= gf_multiply(std::to_integer<std::uint8_t>(current.value[offset]), basis);
        }
        secret[offset] = static_cast<std::byte>(value);
    }
    return secret;
}

Result<RecoveryCeremony>
recovery_ceremony_for(const Authority& authority, int threshold, int share_count) {
    std::vector<std::byte> root;
    root.reserve(authority.root_private_key_pem.size());
    for (const char character : authority.root_private_key_pem) {
        root.push_back(static_cast<std::byte>(character));
    }
    auto shares = split_secret(root, threshold, share_count);
    if (!shares) {
        return std::unexpected(shares.error());
    }
    return RecoveryCeremony{new_uuid_v4(), threshold, std::move(*shares)};
}

Result<void> verify_recovered_root(const Authority& authority,
                                   const RecoveryCeremony& ceremony,
                                   const std::vector<ShamirShare>& shares) {
    auto recovered = recover_secret(shares, ceremony.threshold);
    if (!recovered) {
        return std::unexpected(recovered.error());
    }
    std::string root;
    root.reserve(recovered->size());
    for (const std::byte byte : *recovered) {
        root.push_back(static_cast<char>(byte));
    }
    if (root != authority.root_private_key_pem) {
        return std::unexpected(
            Error{"unauthorized", "recovery shares do not restore the customer root"});
    }
    return {};
}

Result<PkiRecoveryShareEvidence> recovery_evidence(const ShamirShare& share) {
    std::vector<unsigned char> input;
    append_u32(input, static_cast<std::uint32_t>(share.index));
    for (const std::byte value : share.value) {
        input.push_back(std::to_integer<unsigned char>(value));
    }
    std::array<unsigned char, 32> digest{};
    unsigned int digest_size = 0;
    if (EVP_Digest(
            input.data(), input.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != digest.size()) {
        return std::unexpected(crypto_error("could not digest recovery evidence"));
    }
    std::string encoded;
    encoded.reserve(digest.size() * 2);
    for (const unsigned char value : digest) {
        encoded += std::format("{:02x}", value);
    }
    return PkiRecoveryShareEvidence{share.index, std::move(encoded)};
}

Result<AuthorityPublic> Service::create_authority(std::string_view customer_id) {
    std::lock_guard lock(mutex_);
    if (authorities_.contains(std::string(customer_id))) {
        return std::unexpected(Error{"conflict", "customer already has a certificate authority"});
    }
    auto authority = pki::create_authority(customer_id);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    AuthorityPublic output{authority->root_certificate_pem,
                           authority->intermediate_certificate_pem +
                               authority->root_certificate_pem};
    authorities_.emplace(std::string(customer_id), std::move(*authority));
    return output;
}

Result<AuthorityPublic> Service::create_authority(::lake::control::Store& store,
                                                  std::string_view customer_id,
                                                  std::string_view instance_kek) {
    auto generated = pki::create_authority(customer_id);
    if (!generated) {
        return std::unexpected(generated.error());
    }
    auto persisted =
        store.create_pki_authority(NewPkiAuthorityRecord{std::string(customer_id),
                                                         generated->root_certificate_pem,
                                                         generated->intermediate_certificate_pem,
                                                         generated->root_private_key_pem,
                                                         generated->intermediate_private_key_pem},
                                   std::string(instance_kek));
    if (!persisted) {
        return std::unexpected(Error{persisted.error().code, persisted.error().message});
    }
    return AuthorityPublic{persisted->root_certificate_pem, persisted->intermediate_chain_pem};
}

Result<Authority> Service::load_authority(::lake::control::Store& store,
                                          std::string_view customer_id,
                                          std::string_view instance_kek) {
    auto persisted = store.get_pki_authority(std::string(customer_id), std::string(instance_kek));
    if (!persisted) {
        return std::unexpected(Error{persisted.error().code, persisted.error().message});
    }
    return Authority{persisted->root_certificate_pem,
                     persisted->intermediate_chain_pem,
                     persisted->root_private_key_pem,
                     persisted->intermediate_private_key_pem};
}

Result<AuthorityPublic> Service::ensure_authority(::lake::control::Store& store,
                                                  std::string_view customer_id,
                                                  std::string_view instance_kek) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority && authority.error().code == "not_found") {
        auto created = create_authority(store, customer_id, instance_kek);
        if (created || created.error().code != "conflict") {
            return created;
        }
        authority = load_authority(store, customer_id, instance_kek);
    }
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return AuthorityPublic{authority->root_certificate_pem,
                           authority->intermediate_certificate_pem};
}

namespace {

Result<Pkey> verified_csr_key(std::string_view csr_pem) {
    if (csr_pem.empty() || csr_pem.size() > 16384 ||
        !csr_pem.starts_with("-----BEGIN CERTIFICATE REQUEST-----")) {
        return std::unexpected(Error{"invalid_input", "send one PEM PKCS#10 CSR (at most 16 KiB)"});
    }
    Bio input(BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size())));
    Request request(input ? PEM_read_bio_X509_REQ(input.get(), nullptr, nullptr, nullptr)
                          : nullptr);
    if (!request || X509_REQ_get_version(request.get()) != 0) {
        return std::unexpected(Error{"invalid_input", "invalid PKCS#10 CSR"});
    }
    char* remainder = nullptr;
    const auto remaining = BIO_get_mem_data(input.get(), &remainder);
    for (long i = 0; i < remaining; ++i) {
        if (remainder[i] != ' ' && remainder[i] != '\r' && remainder[i] != '\n' &&
            remainder[i] != '\t') {
            return std::unexpected(Error{"invalid_input", "send exactly one CSR"});
        }
    }
    Pkey key(X509_REQ_get_pubkey(request.get()));
    if (!key || EVP_PKEY_get_security_bits(key.get()) < 112 ||
        X509_REQ_verify(request.get(), key.get()) != 1) {
        return std::unexpected(Error{"invalid_input", "CSR signature or public key is invalid"});
    }
    return key;
}

Result<SignedClientCertificate>
sign_tunnel_csr(const Authority& authority, const NetworkTunnelGrant& grant, EVP_PKEY* key) {
    auto issuer_key = private_key_from_pem(authority.intermediate_private_key_pem);
    auto issuer = certificate_from_pem(authority.intermediate_certificate_pem);
    auto root = certificate_from_pem(authority.root_certificate_pem);
    if (!issuer_key || !issuer || !root ||
        X509_check_private_key(issuer->get(), issuer_key->get()) != 1) {
        return std::unexpected(Error{"crypto_error", "customer authority material is invalid"});
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto expiry = network_tunnel_grant_expiry(grant);
    if (!expiry || grant.revoked_at || *expiry <= now) {
        return std::unexpected(Error{"conflict", "the tunnel grant has ended"});
    }
    const auto ends =
        std::min(std::chrono::floor<std::chrono::seconds>(*expiry), now + std::chrono::hours(12));
    if (ends <= now) {
        return std::unexpected(
            Error{"conflict", "the tunnel grant expires before a certificate can be used"});
    }
    const std::string uri = "urn:orama:tunnel-grant:" + grant.id;
    auto certificate = make_certificate(
        key, issuer_key->get(), issuer->get(), "Lake tunnel client", 1, false, uri, {}, true);
    if (!certificate) {
        return std::unexpected(certificate.error());
    }
    const auto start_time = std::chrono::system_clock::to_time_t(now);
    const auto end_time = std::chrono::system_clock::to_time_t(ends);
    if (!ASN1_TIME_set(X509_getm_notBefore(certificate->get()), start_time) ||
        !ASN1_TIME_set(X509_getm_notAfter(certificate->get()), end_time)) {
        return std::unexpected(crypto_error("could not set tunnel certificate validity"));
    }
    // A certificate cannot outlive either CA in its chain.
    for (X509* ca : {issuer->get(), root->get()}) {
        if (ASN1_TIME_compare(X509_get0_notBefore(ca), X509_get0_notBefore(certificate->get())) >
                0 ||
            ASN1_TIME_compare(X509_get0_notAfter(ca), X509_get0_notBefore(certificate->get())) <=
                0) {
            return std::unexpected(
                Error{"conflict", "customer certificate authority is not valid"});
        }
        if (ASN1_TIME_compare(X509_get0_notAfter(ca), X509_get0_notAfter(certificate->get())) < 0 &&
            X509_set1_notAfter(certificate->get(), X509_get0_notAfter(ca)) != 1) {
            return std::unexpected(crypto_error("could not bound tunnel certificate validity"));
        }
    }
    auto usage = add_extension(certificate->get(), NID_ext_key_usage, "critical,clientAuth");
    if (!usage) {
        return std::unexpected(usage.error());
    }
    if (X509_sign(certificate->get(), issuer_key->get(), EVP_sha256()) <= 0) {
        return std::unexpected(crypto_error("could not sign tunnel certificate"));
    }
    auto pem = pem_of_certificate(certificate->get());
    std::tm actual_expiry{};
    if (!pem || ASN1_TIME_to_tm(X509_get0_notAfter(certificate->get()), &actual_expiry) != 1) {
        return std::unexpected(crypto_error("could not encode tunnel certificate"));
    }
    return SignedClientCertificate{*pem,
                                   authority.intermediate_certificate_pem +
                                       authority.root_certificate_pem,
                                   std::chrono::system_clock::from_time_t(timegm(&actual_expiry))};
}

} // namespace

Result<SignedClientCertificate> Service::sign_tunnel_certificate(::lake::control::Store& store,
                                                                 const NetworkTunnelGrant& grant,
                                                                 std::string_view csr_pem,
                                                                 std::string_view instance_kek) {
    auto key = verified_csr_key(csr_pem);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto authority = load_authority(store, grant.customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    if (grant.spec.client_ca_pem.empty() ||
        grant.spec.client_ca_pem != authority->root_certificate_pem) {
        return std::unexpected(
            Error{"conflict", "the grant's client CA does not match the customer authority"});
    }
    return sign_tunnel_csr(*authority, grant, key->get());
}

Result<X509Svid> Service::issue_x509_svid(std::string_view customer_id,
                                          std::string_view spiffe_id,
                                          int lifetime_seconds) {
    std::lock_guard lock(mutex_);
    const auto authority = authorities_.find(std::string(customer_id));
    if (authority == authorities_.end()) {
        return std::unexpected(Error{"not_found", "customer certificate authority does not exist"});
    }
    return pki::issue_x509_svid(authority->second, spiffe_id, lifetime_seconds);
}

Result<X509Svid> Service::issue_x509_svid(::lake::control::Store& store,
                                          std::string_view customer_id,
                                          std::string_view spiffe_id,
                                          std::string_view instance_kek,
                                          int lifetime_seconds) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return pki::issue_x509_svid(*authority, spiffe_id, lifetime_seconds);
}

Result<X509Svid> Service::issue_x509_certificate(::lake::control::Store& store,
                                                 std::string_view customer_id,
                                                 std::string_view subject,
                                                 std::string_view instance_kek,
                                                 int lifetime_seconds) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return pki::issue_x509_certificate(*authority, subject, lifetime_seconds);
}

Result<X509Svid> Service::issue_agent_certificate(::lake::control::Store& store,
                                                  std::string_view customer_id,
                                                  std::string_view subject,
                                                  std::string_view public_key,
                                                  std::string_view instance_kek,
                                                  int lifetime_seconds) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return pki::issue_agent_certificate(*authority, subject, public_key, lifetime_seconds);
}

Result<JwtSvid> Service::issue_jwt_svid(std::string_view customer_id,
                                        std::string_view spiffe_id,
                                        std::string_view audience,
                                        int lifetime_seconds) {
    std::lock_guard lock(mutex_);
    const auto authority = authorities_.find(std::string(customer_id));
    if (authority == authorities_.end()) {
        return std::unexpected(Error{"not_found", "customer certificate authority does not exist"});
    }
    return pki::issue_jwt_svid(authority->second, spiffe_id, audience, lifetime_seconds);
}

Result<JwtSvid> Service::issue_jwt_svid(::lake::control::Store& store,
                                        std::string_view customer_id,
                                        std::string_view spiffe_id,
                                        std::string_view audience,
                                        std::string_view instance_kek,
                                        int lifetime_seconds) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return pki::issue_jwt_svid(*authority, spiffe_id, audience, lifetime_seconds);
}

Result<SshCertificate> Service::issue_ssh_certificate(::lake::control::Store& store,
                                                      std::string_view customer_id,
                                                      std::string_view public_key,
                                                      std::string_view key_id,
                                                      const std::vector<std::string>& principals,
                                                      bool host,
                                                      std::string_view instance_kek,
                                                      int lifetime_seconds) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return pki::issue_ssh_certificate(
        *authority, public_key, key_id, principals, host, lifetime_seconds);
}

Result<RecoveryCeremony>
Service::begin_recovery(std::string_view customer_id, int threshold, int share_count) {
    std::lock_guard lock(mutex_);
    const auto authority = authorities_.find(std::string(customer_id));
    if (authority == authorities_.end()) {
        return std::unexpected(Error{"not_found", "customer certificate authority does not exist"});
    }
    return recovery_ceremony_for(authority->second, threshold, share_count);
}

Result<RecoveryCeremony> Service::begin_recovery(::lake::control::Store& store,
                                                 std::string_view customer_id,
                                                 std::string_view instance_kek,
                                                 int threshold,
                                                 int share_count) {
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    auto ceremony = recovery_ceremony_for(*authority, threshold, share_count);
    if (!ceremony) {
        return std::unexpected(ceremony.error());
    }
    const auto expires_at = std::chrono::system_clock::now() + std::chrono::minutes(15);
    auto record = store.create_pki_recovery_ceremony(NewPkiRecoveryCeremonyRecord{
        ceremony->id,
        std::string(customer_id),
        threshold,
        share_count,
        std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(expires_at))});
    if (!record) {
        return std::unexpected(Error{record.error().code, record.error().message});
    }
    return ceremony;
}

Result<void> Service::recover_root(std::string_view customer_id,
                                   const RecoveryCeremony& ceremony,
                                   const std::vector<ShamirShare>& shares) {
    std::lock_guard lock(mutex_);
    const auto authority = authorities_.find(std::string(customer_id));
    if (authority == authorities_.end()) {
        return std::unexpected(Error{"not_found", "customer certificate authority does not exist"});
    }
    return verify_recovered_root(authority->second, ceremony, shares);
}

Result<void> Service::recover_root(::lake::control::Store& store,
                                   std::string_view customer_id,
                                   std::string_view instance_kek,
                                   const RecoveryCeremony& ceremony,
                                   const std::vector<ShamirShare>& shares) {
    auto record = store.get_pki_recovery_ceremony(std::string(customer_id), ceremony.id);
    if (!record) {
        return std::unexpected(Error{record.error().code, record.error().message});
    }
    if (record->threshold != ceremony.threshold) {
        return std::unexpected(
            Error{"invalid_input", "recovery threshold does not match ceremony"});
    }
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    RecoveryCeremony persisted{record->id, record->threshold, {}};
    auto verified = verify_recovered_root(*authority, persisted, shares);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    std::vector<PkiRecoveryShareEvidence> evidence;
    evidence.reserve(shares.size());
    for (const ShamirShare& share : shares) {
        auto digest = recovery_evidence(share);
        if (!digest) {
            return std::unexpected(digest.error());
        }
        evidence.push_back(std::move(*digest));
    }
    auto completed =
        store.complete_pki_recovery_ceremony(std::string(customer_id), ceremony.id, evidence);
    if (!completed) {
        return std::unexpected(Error{completed.error().code, completed.error().message});
    }
    return {};
}

Result<std::string> Service::new_acme_nonce() {
    if (!acme_nonce_key_ready_) {
        return std::unexpected(crypto_error("ACME nonce key is unavailable"));
    }
    const std::uint64_t counter = acme_nonce_counter_.fetch_add(1) + 1;
    const std::int64_t issued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       acme_limits_.clock().time_since_epoch())
                                       .count();
    auto mac = acme_nonce_mac(counter, issued_ms);
    if (!mac) {
        return std::unexpected(mac.error());
    }
    std::string nonce(16, '\0');
    put_big_endian(nonce.data(), counter);
    put_big_endian(nonce.data() + 8, static_cast<std::uint64_t>(issued_ms));
    nonce += *mac;
    return base64url_encode(reinterpret_cast<const unsigned char*>(nonce.data()), nonce.size());
}

Result<void> Service::consume_acme_nonce(std::string_view nonce) {
    const Error refused{"unauthorized", "ACME nonce is missing or was already used"};
    // A nonce is exactly the 32 bytes new_acme_nonce encodes, in that one encoding.
    if (!acme_nonce_key_ready_ || nonce.size() != 43) {
        return std::unexpected(refused);
    }
    auto decoded = base64url_decode(std::string(nonce));
    if (!decoded || decoded->size() != 32) {
        return std::unexpected(refused);
    }
    auto canonical = base64url_encode(decoded->data(), decoded->size());
    if (!canonical || *canonical != nonce) {
        return std::unexpected(refused);
    }
    const std::uint64_t counter = get_big_endian(decoded->data());
    const auto issued_ms = static_cast<std::int64_t>(get_big_endian(decoded->data() + 8));
    auto mac = acme_nonce_mac(counter, issued_ms);
    if (!mac || CRYPTO_memcmp(mac->data(), decoded->data() + 16, mac->size()) != 0) {
        return std::unexpected(refused);
    }
    const AcmeTime issued{
        std::chrono::duration_cast<AcmeTime::duration>(std::chrono::milliseconds(issued_ms))};
    std::lock_guard lock(mutex_);
    const AcmeTime now = acme_limits_.clock();
    prune_acme_locked(now);
    if (issued > now || now - issued >= acme_limits_.nonce_lifetime ||
        counter < acme_nonce_floor_ || !acme_used_nonces_.emplace(counter, issued).second) {
        return std::unexpected(refused);
    }
    // Forgetting the oldest consumed nonce refuses every nonce issued before it, so none can be
    // replayed once it is forgotten.
    while (acme_used_nonces_.size() > acme_limits_.used_nonces) {
        acme_nonce_floor_ = acme_used_nonces_.begin()->first + 1;
        acme_used_nonces_.erase(acme_used_nonces_.begin());
    }
    return {};
}

Result<AcmeAccount> Service::create_acme_account(::lake::control::Store& store,
                                                 std::string_view customer_id,
                                                 const AcmeJwk& jwk) {
    if (customer_id.empty()) {
        return std::unexpected(Error{"invalid_input", "ACME customer is required"});
    }
    auto key = key_from_acme_jwk(jwk);
    if (!key) {
        return std::unexpected(key.error());
    }
    if (auto customer = store.get_customer(std::string(customer_id)); !customer) {
        return std::unexpected(customer.error().code == "not_found"
                                   ? Error{"not_found", "ACME customer does not exist"}
                                   : Error{customer.error().code, customer.error().message});
    }
    AcmeAccount account{new_uuid_v4(), std::string(customer_id), jwk};
    std::lock_guard lock(mutex_);
    const AcmeTime now = acme_limits_.clock();
    prune_acme_locked(now);
    for (auto owned = acme_customer_accounts_.find(account.customer_id);
         owned != acme_customer_accounts_.end() &&
         owned->second.size() >= acme_limits_.accounts_per_customer;
         owned = acme_customer_accounts_.find(account.customer_id)) {
        const std::string least_recent =
            *std::ranges::min_element(owned->second, {}, [&](const std::string& id) {
                return acme_accounts_.at(id).last_used;
            });
        remove_acme_account_locked(least_recent);
    }
    while (!acme_account_use_.empty() && acme_accounts_.size() >= acme_limits_.accounts) {
        remove_acme_account_locked(std::string(acme_account_use_.begin()->second));
    }
    acme_customer_accounts_[account.customer_id].push_back(account.id);
    acme_accounts_.emplace(account.id, AcmeAccountEntry{account, now, {}});
    acme_account_use_.emplace(now, account.id);
    return account;
}

Result<void> Service::verify_acme_signature(std::string_view account_id,
                                            const std::optional<AcmeJwk>& jwk,
                                            std::string_view signing_input,
                                            std::string_view signature) {
    AcmeJwk selected;
    if (jwk) {
        selected = *jwk;
    } else {
        std::lock_guard lock(mutex_);
        prune_acme_locked(acme_limits_.clock());
        const auto account = acme_accounts_.find(std::string(account_id));
        if (account == acme_accounts_.end()) {
            return std::unexpected(Error{"unauthorized", "ACME account does not exist"});
        }
        selected = account->second.account.jwk;
    }
    auto key = key_from_acme_jwk(selected);
    auto raw_signature = base64url_decode(std::string(signature));
    if (!key || !raw_signature) {
        return std::unexpected(Error{"unauthorized", "ACME JWS is invalid"});
    }
    auto verified = verify_jws(key->get(), signing_input, *raw_signature);
    if (verified && !jwk) {
        std::lock_guard lock(mutex_);
        const auto account = acme_accounts_.find(std::string(account_id));
        if (account != acme_accounts_.end()) {
            touch_acme_account_locked(account->second, acme_limits_.clock());
        }
    }
    return verified;
}

Result<AcmeOrder> Service::create_acme_order(::lake::control::Store& store,
                                             std::string_view customer_id,
                                             std::string_view account_id,
                                             std::string_view identifier,
                                             std::string_view instance_kek) {
    if (identifier.empty() || identifier.find('.') == std::string_view::npos) {
        return std::unexpected(Error{"invalid_input", "an ACME DNS identifier is required"});
    }
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    auto token = random_acme_value(24);
    if (!token) {
        return std::unexpected(token.error());
    }
    std::lock_guard lock(mutex_);
    const AcmeTime now = acme_limits_.clock();
    prune_acme_locked(now);
    const auto account = acme_accounts_.find(std::string(account_id));
    if (account == acme_accounts_.end() || account->second.account.customer_id != customer_id) {
        return std::unexpected(Error{"unauthorized", "ACME account does not own this customer"});
    }
    auto thumbprint = acme_thumbprint(account->second.account.jwk);
    if (!thumbprint) {
        return std::unexpected(thumbprint.error());
    }
    touch_acme_account_locked(account->second, now);
    while (!account->second.orders.empty() &&
           account->second.orders.size() >= acme_limits_.orders_per_account) {
        remove_acme_order_locked(std::string(account->second.orders.front()));
    }
    const std::string order_id = new_uuid_v4();
    const std::string authorization_id = new_uuid_v4();
    AcmeOrder order{order_id,
                    std::string(customer_id),
                    std::string(account_id),
                    std::string(identifier),
                    authorization_id,
                    "pending",
                    {}};
    AcmeAuthorization authorization{authorization_id,
                                    order_id,
                                    std::string(identifier),
                                    *token,
                                    *token + "." + *thumbprint,
                                    "pending"};
    const AcmeTime expires_at = now + acme_limits_.order_lifetime;
    acme_orders_.emplace(order.id, AcmeOrderEntry{order, expires_at});
    acme_order_expiry_.emplace(expires_at, order.id);
    acme_authorizations_.emplace(authorization.id, authorization);
    acme_tokens_.emplace(authorization.token, authorization.id);
    account->second.orders.push_back(order.id);
    return order;
}

Result<AcmeOrder> Service::acme_order(std::string_view customer_id,
                                      std::string_view account_id,
                                      std::string_view order_id) {
    std::lock_guard lock(mutex_);
    prune_acme_locked(acme_limits_.clock());
    const AcmeOrderEntry* order = find_acme_order_locked(customer_id, account_id, order_id);
    if (order == nullptr) {
        return std::unexpected(Error{"not_found", "ACME order does not exist"});
    }
    return order->order;
}

Result<AcmeAuthorization> Service::acme_authorization(std::string_view customer_id,
                                                      std::string_view account_id,
                                                      std::string_view authorization_id) {
    std::lock_guard lock(mutex_);
    prune_acme_locked(acme_limits_.clock());
    const auto authorization = acme_authorizations_.find(std::string(authorization_id));
    if (authorization == acme_authorizations_.end() ||
        find_acme_order_locked(customer_id, account_id, authorization->second.order_id) ==
            nullptr) {
        return std::unexpected(Error{"not_found", "ACME authorization does not exist"});
    }
    return authorization->second;
}

Result<AcmeAuthorization> Service::authorize_acme_order(std::string_view customer_id,
                                                        std::string_view account_id,
                                                        std::string_view authorization_id) {
    std::lock_guard lock(mutex_);
    prune_acme_locked(acme_limits_.clock());
    const auto authorization = acme_authorizations_.find(std::string(authorization_id));
    AcmeOrderEntry* order =
        authorization == acme_authorizations_.end()
            ? nullptr
            : find_acme_order_locked(customer_id, account_id, authorization->second.order_id);
    if (order == nullptr) {
        return std::unexpected(Error{"not_found", "ACME authorization does not exist"});
    }
    authorization->second.status = "valid";
    order->order.status = "ready";
    return authorization->second;
}

Result<std::string> Service::acme_http01_response(std::string_view token) {
    std::lock_guard lock(mutex_);
    prune_acme_locked(acme_limits_.clock());
    const auto authorization = acme_tokens_.find(std::string(token));
    if (authorization == acme_tokens_.end()) {
        return std::unexpected(Error{"not_found", "ACME HTTP-01 token does not exist"});
    }
    return acme_authorizations_.at(authorization->second).key_authorization;
}

Result<AcmeOrder> Service::finalize_acme_order(::lake::control::Store& store,
                                               std::string_view customer_id,
                                               std::string_view account_id,
                                               std::string_view order_id,
                                               std::string_view csr,
                                               std::string_view instance_kek) {
    AcmeOrder pending;
    {
        std::lock_guard lock(mutex_);
        prune_acme_locked(acme_limits_.clock());
        const AcmeOrderEntry* order = find_acme_order_locked(customer_id, account_id, order_id);
        if (order == nullptr || order->order.status != "ready") {
            return std::unexpected(Error{"invalid_input", "ACME order is not ready to finalize"});
        }
        pending = order->order;
    }
    auto authority = load_authority(store, pending.customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    auto certificate = issue_acme_certificate(*authority, pending.identifier, csr);
    if (!certificate) {
        return std::unexpected(certificate.error());
    }
    std::lock_guard lock(mutex_);
    prune_acme_locked(acme_limits_.clock());
    AcmeOrderEntry* order = find_acme_order_locked(customer_id, account_id, order_id);
    if (order == nullptr || order->order.status != "ready") {
        return std::unexpected(Error{"conflict", "ACME order changed during finalization"});
    }
    order->order.status = "valid";
    order->order.certificate_pem = certificate->certificate_pem + certificate->chain_pem;
    return order->order;
}

AcmeStateSize Service::acme_state_size() {
    std::lock_guard lock(mutex_);
    prune_acme_locked(acme_limits_.clock());
    return {acme_used_nonces_.size(),
            acme_accounts_.size(),
            acme_orders_.size(),
            acme_authorizations_.size()};
}

Service::Service(AcmeLimits acme_limits) : acme_limits_(std::move(acme_limits)) {
    if (!acme_limits_.clock) {
        acme_limits_.clock = std::chrono::steady_clock::now;
    }
    acme_nonce_key_ready_ =
        RAND_bytes(acme_nonce_key_.data(), static_cast<int>(acme_nonce_key_.size())) == 1;
}

// The first 16 bytes of HMAC-SHA-256(key, counter || issued_ms), both big-endian.
Result<std::string> Service::acme_nonce_mac(std::uint64_t counter, std::int64_t issued_ms) const {
    std::array<unsigned char, 16> message{};
    put_big_endian(reinterpret_cast<char*>(message.data()), counter);
    put_big_endian(reinterpret_cast<char*>(message.data() + 8),
                   static_cast<std::uint64_t>(issued_ms));
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_size = 0;
    if (HMAC(EVP_sha256(),
             acme_nonce_key_.data(),
             static_cast<int>(acme_nonce_key_.size()),
             message.data(),
             message.size(),
             digest.data(),
             &digest_size) == nullptr ||
        digest_size < 16) {
        return std::unexpected(crypto_error("could not authenticate ACME nonce"));
    }
    return std::string(reinterpret_cast<const char*>(digest.data()), 16);
}

// Drop consumed nonces past their lifetime, idle accounts and expired orders. Each index is
// ordered by time, so the cost is proportional to what is dropped.
void Service::prune_acme_locked(AcmeTime now) {
    while (!acme_used_nonces_.empty() &&
           acme_used_nonces_.begin()->second + acme_limits_.nonce_lifetime <= now) {
        acme_used_nonces_.erase(acme_used_nonces_.begin());
    }
    while (!acme_order_expiry_.empty() && acme_order_expiry_.begin()->first <= now) {
        remove_acme_order_locked(std::string(acme_order_expiry_.begin()->second));
    }
    while (!acme_account_use_.empty() &&
           acme_account_use_.begin()->first + acme_limits_.account_idle_lifetime <= now) {
        remove_acme_account_locked(std::string(acme_account_use_.begin()->second));
    }
}

void Service::touch_acme_account_locked(AcmeAccountEntry& entry, AcmeTime now) {
    acme_account_use_.erase({entry.last_used, entry.account.id});
    entry.last_used = now;
    acme_account_use_.emplace(now, entry.account.id);
}

void Service::remove_acme_account_locked(const std::string& account_id) {
    const auto account = acme_accounts_.find(account_id);
    if (account == acme_accounts_.end()) {
        return;
    }
    for (const std::string& order_id : std::vector<std::string>(account->second.orders)) {
        remove_acme_order_locked(order_id);
    }
    const auto owned = acme_customer_accounts_.find(account->second.account.customer_id);
    if (owned != acme_customer_accounts_.end()) {
        std::erase(owned->second, account_id);
        if (owned->second.empty()) {
            acme_customer_accounts_.erase(owned);
        }
    }
    acme_account_use_.erase({account->second.last_used, account_id});
    acme_accounts_.erase(account);
}

void Service::remove_acme_order_locked(const std::string& order_id) {
    const auto order = acme_orders_.find(order_id);
    if (order == acme_orders_.end()) {
        return;
    }
    const auto authorization = acme_authorizations_.find(order->second.order.authorization_id);
    if (authorization != acme_authorizations_.end()) {
        acme_tokens_.erase(authorization->second.token);
        acme_authorizations_.erase(authorization);
    }
    const auto account = acme_accounts_.find(order->second.order.account_id);
    if (account != acme_accounts_.end()) {
        std::erase(account->second.orders, order_id);
    }
    acme_order_expiry_.erase({order->second.expires_at, order_id});
    acme_orders_.erase(order);
}

Service::AcmeOrderEntry* Service::find_acme_order_locked(std::string_view customer_id,
                                                         std::string_view account_id,
                                                         std::string_view order_id) {
    const auto order = acme_orders_.find(std::string(order_id));
    if (order == acme_orders_.end() || order->second.order.customer_id != customer_id ||
        order->second.order.account_id != account_id) {
        return nullptr;
    }
    return &order->second;
}

} // namespace lake::control::pki
