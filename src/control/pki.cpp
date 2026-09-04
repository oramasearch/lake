#include "lake/control/pki.hpp"

#include "lake/control/store.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
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

using Bio = std::unique_ptr<BIO, BioDeleter>;
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Certificate = std::unique_ptr<X509, X509Deleter>;
using Store = std::unique_ptr<X509_STORE, StoreDeleter>;
using StoreContext = std::unique_ptr<X509_STORE_CTX, StoreContextDeleter>;
using CertificateStack = std::unique_ptr<STACK_OF(X509), StackDeleter>;
using MdContext = std::unique_ptr<EVP_MD_CTX, MdContextDeleter>;
using EcdsaSignature = std::unique_ptr<ECDSA_SIG, EcdsaSignatureDeleter>;

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

Result<Certificate> make_certificate(EVP_PKEY* subject_key,
                                     EVP_PKEY* issuer_key,
                                     X509* issuer_certificate,
                                     std::string_view common_name,
                                     int lifetime_seconds,
                                     bool is_authority,
                                     std::string_view uri_san = {}) {
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
    return recovery_ceremony_for(*authority, threshold, share_count);
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
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    return verify_recovered_root(*authority, ceremony, shares);
}

Result<AcmeOrder> Service::create_acme_order(::lake::control::Store& store,
                                             std::string_view customer_id,
                                             std::string_view identifier,
                                             std::string_view instance_kek) {
    if (identifier.empty() || identifier.find('.') == std::string_view::npos) {
        return std::unexpected(Error{"invalid_input", "an ACME DNS identifier is required"});
    }
    auto authority = load_authority(store, customer_id, instance_kek);
    if (!authority) {
        return std::unexpected(authority.error());
    }
    std::lock_guard lock(mutex_);
    AcmeOrder order{
        new_uuid_v4(), std::string(customer_id), std::string(identifier), "pending", {}};
    acme_orders_.emplace(order.id, order);
    return order;
}

Result<AcmeOrder> Service::authorize_acme_order(std::string_view order_id) {
    std::lock_guard lock(mutex_);
    const auto order = acme_orders_.find(std::string(order_id));
    if (order == acme_orders_.end()) {
        return std::unexpected(Error{"not_found", "ACME order does not exist"});
    }
    order->second.status = "ready";
    return order->second;
}

Result<AcmeOrder> Service::finalize_acme_order(::lake::control::Store& store,
                                               std::string_view order_id,
                                               std::string_view instance_kek) {
    std::lock_guard lock(mutex_);
    const auto order = acme_orders_.find(std::string(order_id));
    if (order == acme_orders_.end() || order->second.status != "ready") {
        return std::unexpected(Error{"invalid_input", "ACME order is not ready to finalize"});
    }
    auto certificate = issue_x509_certificate(
        store, order->second.customer_id, order->second.identifier, instance_kek);
    if (!certificate) {
        return std::unexpected(certificate.error());
    }
    order->second.status = "valid";
    order->second.certificate_pem = certificate->certificate_pem + certificate->chain_pem;
    return order->second;
}

} // namespace lake::control::pki
