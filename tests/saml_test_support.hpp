// SPDX-License-Identifier: MIT

#pragma once

// A test identity provider for SAML sign-in (lake#836): an RSA key, its certificate, and responses
// it signs the way identity providers do (an enveloped signature on the assertion, exclusive
// canonicalization, RSA-SHA256). It canonicalizes with Lake's own canonicalizer, so it shows the
// checks around the signature; tests/fixtures/saml carries responses signed by an independent
// implementation for the canonicalization itself.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "lake/control/saml.hpp"

namespace lake::test::saml {

inline std::string base64(std::string_view value) {
    std::string encoded(((value.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

inline std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

inline std::string iso(std::int64_t seconds) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::sys_seconds{std::chrono::seconds{seconds}});
}

class Idp {
public:
    Idp() {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EXPECT_EQ(EVP_PKEY_keygen_init(context), 1);
        EXPECT_EQ(EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048), 1);
        EXPECT_EQ(EVP_PKEY_keygen(context, &key_), 1);
        EVP_PKEY_CTX_free(context);
    }
    ~Idp() {
        EVP_PKEY_free(key_);
    }
    Idp(const Idp&) = delete;
    Idp& operator=(const Idp&) = delete;

    std::string certificate() const {
        X509* result = X509_new();
        X509_set_version(result, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(result), 1);
        X509_gmtime_adj(X509_get_notBefore(result), 0);
        X509_gmtime_adj(X509_get_notAfter(result), 3600);
        X509_set_pubkey(result, key_);
        X509_sign(result, key_, EVP_sha256());
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(bio, result);
        char* value = nullptr;
        const long size = BIO_get_mem_data(bio, &value);
        std::string pem(value, static_cast<std::size_t>(size));
        BIO_free(bio);
        X509_free(result);
        return pem;
    }

    std::string sign_bytes(std::string_view value, const EVP_MD* digest = EVP_sha256()) const {
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        std::size_t size = 0;
        EXPECT_EQ(EVP_DigestSignInit(context, nullptr, digest, nullptr, key_), 1);
        EXPECT_EQ(EVP_DigestSignUpdate(context, value.data(), value.size()), 1);
        EXPECT_EQ(EVP_DigestSignFinal(context, nullptr, &size), 1);
        std::string signature(size, '\0');
        EXPECT_EQ(
            EVP_DigestSignFinal(context, reinterpret_cast<unsigned char*>(signature.data()), &size),
            1);
        EVP_MD_CTX_free(context);
        signature.resize(size);
        return signature;
    }

    // Signs `xml`'s Assertion: the marker <SIGNATURE/> inside it becomes an enveloped signature
    // that references `#reference` (the assertion's ID unless a test says otherwise).
    std::string sign_assertion(const std::string& xml,
                               const std::string& reference,
                               std::string_view signature_method =
                                   "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256") const {
        const std::string unsigned_xml = replace_once(xml, "<SIGNATURE/>", "");
        const auto canonical = lake::control::saml::canonicalize_element(unsigned_xml, "Assertion");
        EXPECT_TRUE(canonical.has_value()) << canonical.error();
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_size = 0;
        EVP_Digest(
            canonical->data(), canonical->size(), digest, &digest_size, EVP_sha256(), nullptr);
        const std::string signature =
            "<ds:Signature xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\"><ds:SignedInfo>"
            "<ds:CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"/>"
            "<ds:SignatureMethod Algorithm=\"" +
            std::string(signature_method) + "\"/><ds:Reference URI=\"#" + reference +
            "\"><ds:Transforms><ds:Transform "
            "Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\"/><ds:Transform "
            "Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"/></"
            "ds:Transforms><ds:DigestMethod "
            "Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\"/><ds:DigestValue>" +
            base64(std::string_view(reinterpret_cast<const char*>(digest), digest_size)) +
            "</ds:DigestValue></ds:Reference></ds:SignedInfo><ds:SignatureValue>SIGNATURE-VALUE"
            "</ds:SignatureValue></ds:Signature>";
        const std::string placed = replace_once(xml, "<SIGNATURE/>", signature);
        const auto info = lake::control::saml::canonicalize_element(placed, "SignedInfo");
        EXPECT_TRUE(info.has_value()) << info.error();
        return replace_once(placed, "SIGNATURE-VALUE", base64(sign_bytes(*info)));
    }

private:
    EVP_PKEY* key_ = nullptr;
};

// The parts of a response a test changes.
struct ResponseShape {
    std::string issuer = "https://idp.test/saml";
    std::string destination; // the ACS URL; empty leaves Destination out
    std::string recipient;   // the ACS URL
    std::string audience;    // the SP entity id
    std::string in_response_to = "_request";
    std::string assertion_id = "_assertion-1";
    std::string subject = "person-1";
    std::string email = "person@example.com";
    std::int64_t issued = 0;
    std::int64_t lifetime = 300;
    std::string status = "urn:oasis:names:tc:SAML:2.0:status:Success";
};

// An unsigned response with the <SIGNATURE/> marker after the assertion's Issuer.
inline std::string response_xml(const ResponseShape& shape) {
    const std::string issued = iso(shape.issued);
    const std::string expires = iso(shape.issued + shape.lifetime);
    const std::string in_response_to = shape.in_response_to.empty()
                                           ? std::string()
                                           : " InResponseTo=\"" + shape.in_response_to + "\"";
    return "<samlp:Response xmlns:samlp=\"urn:oasis:names:tc:SAML:2.0:protocol\" "
           "xmlns:saml=\"urn:oasis:names:tc:SAML:2.0:assertion\" ID=\"_response-1\" "
           "Version=\"2.0\" "
           "IssueInstant=\"" +
           issued + "\"" +
           (shape.destination.empty() ? std::string()
                                      : " Destination=\"" + shape.destination + "\"") +
           in_response_to + "><saml:Issuer>" + shape.issuer +
           "</saml:Issuer><samlp:Status><samlp:StatusCode Value=\"" + shape.status +
           "\"/></samlp:Status><saml:Assertion ID=\"" + shape.assertion_id +
           "\" Version=\"2.0\" IssueInstant=\"" + issued + "\"><saml:Issuer>" + shape.issuer +
           "</saml:Issuer><SIGNATURE/><saml:Subject><saml:NameID>" + shape.subject +
           "</saml:NameID><saml:SubjectConfirmation "
           "Method=\"urn:oasis:names:tc:SAML:2.0:cm:bearer\"><saml:SubjectConfirmationData" +
           in_response_to + " NotOnOrAfter=\"" + expires + "\" Recipient=\"" + shape.recipient +
           "\"/></saml:SubjectConfirmation></saml:Subject><saml:Conditions NotBefore=\"" + issued +
           "\" NotOnOrAfter=\"" + expires + "\"><saml:AudienceRestriction><saml:Audience>" +
           shape.audience +
           "</saml:Audience></saml:AudienceRestriction></saml:Conditions><saml:AuthnStatement "
           "AuthnInstant=\"" +
           issued +
           "\"><saml:AuthnContext><saml:AuthnContextClassRef>urn:oasis:names:tc:SAML:2.0:ac:"
           "classes:Password</saml:AuthnContextClassRef></saml:AuthnContext></"
           "saml:AuthnStatement><saml:AttributeStatement><saml:Attribute "
           "Name=\"email\"><saml:AttributeValue>" +
           shape.email +
           "</saml:AttributeValue></saml:Attribute></saml:AttributeStatement></saml:Assertion></"
           "samlp:Response>";
}

} // namespace lake::test::saml
