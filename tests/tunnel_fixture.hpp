#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace lake::test {

using TunnelKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using TunnelCertificate = std::unique_ptr<X509, decltype(&X509_free)>;

inline void require_crypto(bool ok) {
    if (!ok) {
        throw std::runtime_error("tunnel test crypto fixture failed");
    }
}

struct TunnelCsr {
    TunnelKey key{nullptr, EVP_PKEY_free};
    std::string pem;
};

// The request deliberately asks for privileges and names that Lake must not copy.
inline TunnelCsr tunnel_csr(bool bad_signature = false, bool weak = false) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(weak ? EVP_PKEY_RSA : EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    require_crypto(context && EVP_PKEY_keygen_init(context.get()) == 1);
    if (weak) {
        require_crypto(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 1024) == 1);
    } else {
        require_crypto(
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1);
    }
    EVP_PKEY* raw = nullptr;
    require_crypto(EVP_PKEY_keygen(context.get(), &raw) == 1);
    TunnelCsr result{TunnelKey(raw, EVP_PKEY_free), {}};
    std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)> request(X509_REQ_new(), X509_REQ_free);
    require_crypto(request && X509_REQ_set_version(request.get(), 0) == 1);
    require_crypto(X509_REQ_set_pubkey(request.get(), result.key.get()) == 1);
    require_crypto(
        X509_NAME_add_entry_by_txt(X509_REQ_get_subject_name(request.get()),
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("untrusted requester"),
                                   -1,
                                   -1,
                                   0) == 1);
    auto* extensions = sk_X509_EXTENSION_new_null();
    require_crypto(extensions != nullptr);
    for (const auto& [nid, value] :
         {std::pair{NID_subject_alt_name, "URI:urn:attacker,DNS:attacker.test"},
          std::pair{NID_basic_constraints, "critical,CA:TRUE"},
          std::pair{NID_ext_key_usage, "serverAuth"}}) {
        auto* extension = X509V3_EXT_conf_nid(nullptr, nullptr, nid, value);
        require_crypto(extension && sk_X509_EXTENSION_push(extensions, extension) > 0);
    }
    const auto added = X509_REQ_add_extensions(request.get(), extensions);
    sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
    require_crypto(added == 1);
    if (bad_signature) {
        auto other = tunnel_csr();
        require_crypto(X509_REQ_sign(request.get(), other.key.get(), EVP_sha256()) > 0);
    } else {
        require_crypto(X509_REQ_sign(request.get(), result.key.get(), EVP_sha256()) > 0);
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    require_crypto(output && PEM_write_bio_X509_REQ(output.get(), request.get()) == 1);
    char* data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    result.pem.assign(data, static_cast<std::size_t>(size));
    return result;
}

inline TunnelCertificate tunnel_certificate(const std::string& pem) {
    std::unique_ptr<BIO, decltype(&BIO_free)> input(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    TunnelCertificate certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr),
                                  X509_free);
    require_crypto(certificate != nullptr);
    return certificate;
}

} // namespace lake::test
