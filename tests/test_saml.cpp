// SAML sign-in through Lake's Assertion Consumer Service (lake#836, docs/api/sso.md#saml-sign-in):
// the canonicalizer against the specification's examples, verification of responses signed by an
// independent implementation (tests/fixtures/saml), the refusals (tampering, signature wrapping,
// audience, recipient, destination, time, replay), and the routes end to end.

#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
#include "saml_test_support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/saml.hpp"

namespace {

namespace saml = lake::control::saml;
using lake::test::saml::base64;
using lake::test::saml::Idp;
using lake::test::saml::replace_once;
using lake::test::saml::response_xml;
using lake::test::saml::ResponseShape;
using nlohmann::json;

// The fixtures' service provider and time (tests/fixtures/saml/make-fixtures.py).
constexpr std::string_view fixture_provider = "11111111-1111-1111-1111-111111111111";
constexpr std::string_view fixture_host = "lake.test";

std::int64_t fixture_time(int hour, int minute) {
    using namespace std::chrono;
    return duration_cast<seconds>(
               (sys_days{year{2026} / 10 / 21} + hours{hour} + minutes{minute}).time_since_epoch())
        .count();
}

std::string read_file(const std::string& relative) {
    std::ifstream file(std::string(LAKE_TEST_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct Fixture {
    std::string certificate = read_file("tests/fixtures/saml/idp-certificate.pem");
    std::string sp = saml::sp_entity_id(fixture_host, fixture_provider);
    std::string acs = saml::acs_url(fixture_host, fixture_provider);

    saml::Expectations expect(std::string_view issuer, std::int64_t now) const {
        return {.certificate_pem = certificate,
                .idp_entity_id = issuer,
                .sp_entity_id = sp,
                .acs_url = acs,
                .now = now};
    }
};

constexpr std::string_view okta_issuer = "http://www.okta.com/exkFixture";
constexpr std::string_view entra_issuer =
    "https://sts.windows.net/00000000-0000-0000-0000-00000000f1c5/";

// A response the test identity provider signs for `expected`, valid at `now`.
struct Signed {
    Idp idp;
    std::string certificate = idp.certificate();
    std::string sp = saml::sp_entity_id(fixture_host, fixture_provider);
    std::string acs = saml::acs_url(fixture_host, fixture_provider);
    std::int64_t now = fixture_time(12, 1);

    ResponseShape shape() const {
        ResponseShape result;
        result.destination = acs;
        result.recipient = acs;
        result.audience = sp;
        result.issued = now - 30;
        return result;
    }
    std::string xml(const ResponseShape& value) const {
        return idp.sign_assertion(response_xml(value), value.assertion_id);
    }
    saml::Expectations expect() const {
        return {.certificate_pem = certificate,
                .idp_entity_id = "https://idp.test/saml",
                .sp_entity_id = sp,
                .acs_url = acs,
                .now = now};
    }
    std::expected<saml::Assertion, std::string> verify(const std::string& document) const {
        return saml::verify_response(base64(document), expect());
    }
};

void expect_refused(const std::expected<saml::Assertion, std::string>& result,
                    std::string_view because) {
    ASSERT_FALSE(result.has_value()) << "accepted; expected: " << because;
    EXPECT_NE(result.error().find(because), std::string::npos) << result.error();
}

} // namespace

// --- Exclusive XML Canonicalization ---------------------------------------------------------

TEST(SamlCanonicalization, RendersOnlyVisiblyUtilizedNamespacesAsTheSpecificationShows) {
    // https://www.w3.org/TR/xml-exc-c14n/ §2.2: the same subtree in two different contexts
    // canonicalizes to the same bytes.
    const std::string expected = "<n1:elem2 xmlns:n1=\"http://example.net\" xml:lang=\"en\">"
                                 "<n3:stuff xmlns:n3=\"ftp://example.org\"></n3:stuff></n1:elem2>";
    const auto first = saml::canonicalize_element(
        "<n0:local xmlns:n0=\"foo:bar\" xmlns:n3=\"ftp://example.org\"><n1:elem2 "
        "xmlns:n1=\"http://example.net\" xml:lang=\"en\"><n3:stuff "
        "xmlns:n3=\"ftp://example.org\"/></n1:elem2></n0:local>",
        "elem2");
    const auto second = saml::canonicalize_element(
        "<n2:pdu xmlns:n1=\"http://example.com\" xmlns:n2=\"http://foo.example\" xml:lang=\"fr\" "
        "xml:space=\"retain\"><n1:elem2 xmlns:n1=\"http://example.net\" xml:lang=\"en\"><n3:stuff "
        "xmlns:n3=\"ftp://example.org\"/></n1:elem2></n2:pdu>",
        "elem2");
    ASSERT_TRUE(first.has_value()) << first.error();
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_EQ(*first, expected);
    EXPECT_EQ(*second, expected);
}

TEST(SamlCanonicalization, StartAndEndTagsAttributesAndDefaultNamespaces) {
    // The C14N 1.0 §3.3 document; the expected bytes are libxml2's `xmllint --exc-c14n`.
    const std::string input = "<doc>\n   <e1   />\n   <e2   ></e2>\n"
                              "   <e3   name = \"elem3\"   id=\"elem3\"   />\n"
                              "   <e4   name=\"elem4\"   id=\"elem4\"   ></e4>\n"
                              "   <e5 a:attr=\"out\" b:attr=\"sorted\" attr2=\"all\" attr=\"I'm\"\n"
                              "      xmlns:b=\"http://www.ietf.org\"\n"
                              "      xmlns:a=\"http://www.w3.org\"\n"
                              "      xmlns=\"http://example.org\"/>\n"
                              "   <e6 xmlns=\"\" xmlns:a=\"http://www.w3.org\">\n"
                              "      <e7 xmlns=\"http://www.ietf.org\">\n"
                              "         <e8 xmlns=\"\" xmlns:a=\"http://www.w3.org\">\n"
                              "            <e9 xmlns=\"\" xmlns:a=\"http://www.ietf.org\"/>\n"
                              "         </e8>\n      </e7>\n   </e6>\n</doc>";
    const std::string expected =
        "<doc>\n   <e1></e1>\n   <e2></e2>\n   <e3 id=\"elem3\" name=\"elem3\"></e3>\n"
        "   <e4 id=\"elem4\" name=\"elem4\"></e4>\n"
        "   <e5 xmlns=\"http://example.org\" xmlns:a=\"http://www.w3.org\" "
        "xmlns:b=\"http://www.ietf.org\" attr=\"I'm\" attr2=\"all\" b:attr=\"sorted\" "
        "a:attr=\"out\"></e5>\n"
        "   <e6>\n      <e7 xmlns=\"http://www.ietf.org\">\n         <e8 xmlns=\"\">\n"
        "            <e9></e9>\n         </e8>\n      </e7>\n   </e6>\n</doc>";
    const auto canonical = saml::canonicalize_exclusive(input);
    ASSERT_TRUE(canonical.has_value()) << canonical.error();
    EXPECT_EQ(*canonical, expected);
}

TEST(SamlCanonicalization, CharacterReferencesLineEndsAndAttributeWhiteSpace) {
    // Expected bytes from `xmllint --exc-c14n`; CR LF in text becomes LF.
    const auto canonical = saml::canonicalize_exclusive(
        "<doc a=\"x\ty\nz\" b=\"a&#x9;b&#xA;c&#xD;d\" t='x\"y'>A &amp; B &lt; C &gt; "
        "D&#xD;&#169;\r\nE</doc>");
    ASSERT_TRUE(canonical.has_value()) << canonical.error();
    EXPECT_EQ(*canonical,
              "<doc a=\"x y z\" b=\"a&#x9;b&#xA;c&#xD;d\" t=\"x&quot;y\">A &amp; B &lt; C &gt; "
              "D&#xD;\xC2\xA9\nE</doc>");
}

TEST(SamlCanonicalization, InclusiveNamespacesPrefixListKeepsTheNamedPrefixes) {
    const std::string input = "<a:r xmlns:a=\"urn:a\" xmlns:b=\"urn:b\" xmlns=\"urn:d\">"
                              "<a:c b:x=\"1\"><d/></a:c><a:e/></a:r>";
    const auto whole = saml::canonicalize_exclusive(input);
    ASSERT_TRUE(whole.has_value()) << whole.error();
    EXPECT_EQ(*whole,
              "<a:r xmlns:a=\"urn:a\"><a:c xmlns:b=\"urn:b\" b:x=\"1\"><d "
              "xmlns=\"urn:d\"></d></a:c><a:e></a:e></a:r>");
    const auto listed = saml::canonicalize_element(input, "e", "", "b #default");
    ASSERT_TRUE(listed.has_value()) << listed.error();
    EXPECT_EQ(*listed, "<a:e xmlns=\"urn:d\" xmlns:a=\"urn:a\" xmlns:b=\"urn:b\"></a:e>");
    const auto unlisted = saml::canonicalize_element(input, "e");
    ASSERT_TRUE(unlisted.has_value()) << unlisted.error();
    EXPECT_EQ(*unlisted, "<a:e xmlns:a=\"urn:a\"></a:e>");
}

TEST(SamlCanonicalization, RefusesDtdsEntitiesInstructionsCdataAndMalformedXml) {
    for (const std::string_view input : {
             std::string_view("<!DOCTYPE r [<!ENTITY x \"y\">]><r>&x;</r>"),
             std::string_view("<r>&x;</r>"),
             std::string_view("<r><?pi data?></r>"),
             std::string_view("<?xml-stylesheet href=\"a\"?><r/>"),
             std::string_view("<r><![CDATA[x]]></r>"),
             std::string_view("<p:r/>"),
             std::string_view("<r a=\"1\" a=\"2\"/>"),
             std::string_view("<r xmlns:p=\"urn:x\" xmlns:q=\"urn:x\" p:a=\"1\" q:a=\"2\"/>"),
             std::string_view("<r><s></r></s>"),
             std::string_view("<r/><r/>"),
             std::string_view("<r>&#0;</r>"),
         }) {
        EXPECT_FALSE(saml::canonicalize_exclusive(input).has_value()) << input;
    }
    std::string deep;
    for (int level = 0; level < 100; ++level) {
        deep += "<d>";
    }
    for (int level = 0; level < 100; ++level) {
        deep += "</d>";
    }
    EXPECT_FALSE(saml::canonicalize_exclusive(deep).has_value());
    // The XML declaration and comments are accepted, and comments are left out.
    const auto commented =
        saml::canonicalize_exclusive("<?xml version=\"1.0\"?>\n<!-- a --><r>x<!-- b -->y</r>");
    ASSERT_TRUE(commented.has_value()) << commented.error();
    EXPECT_EQ(*commented, "<r>xy</r>");
}

// --- Responses signed by an independent implementation --------------------------------------

TEST(SamlResponse, VerifiesAnOktaShapedResponseSignedByAnotherImplementation) {
    const Fixture fixture;
    const std::string document = read_file("tests/fixtures/saml/okta-response.xml");
    ASSERT_FALSE(document.empty());
    const auto assertion =
        saml::verify_response(base64(document), fixture.expect(okta_issuer, fixture_time(12, 1)));
    ASSERT_TRUE(assertion.has_value()) << assertion.error();
    EXPECT_EQ(assertion->id, "id-okta-assertion");
    EXPECT_EQ(assertion->in_response_to, "_fixture-request");
    EXPECT_EQ(assertion->issuer, okta_issuer);
    EXPECT_EQ(assertion->subject, "ada@example.com");
    EXPECT_EQ(assertion->attributes.value("email", ""), "ada@example.com");
    EXPECT_EQ(assertion->attributes.value("name", ""), "Ada Lovelace");
    EXPECT_EQ(assertion->expires_at, fixture_time(12, 5));
}

TEST(SamlResponse, VerifiesAnEntraShapedResponseSignedByAnotherImplementation) {
    const Fixture fixture;
    const std::string document = read_file("tests/fixtures/saml/entra-response.xml");
    ASSERT_FALSE(document.empty());
    const auto assertion =
        saml::verify_response(base64(document), fixture.expect(entra_issuer, fixture_time(12, 1)));
    ASSERT_TRUE(assertion.has_value()) << assertion.error();
    EXPECT_EQ(assertion->id, "_entra-assertion");
    EXPECT_EQ(assertion->subject, "entra-subject-1");
    EXPECT_EQ(assertion->attributes.value(
                  "http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress", ""),
              "grace@example.com");
    // Base64 with line breaks (as the fixture's certificate is) and CR LF line ends are accepted.
    std::string wrapped = base64(document);
    for (std::size_t at = 76; at < wrapped.size(); at += 78) {
        wrapped.insert(at, "\r\n");
    }
    EXPECT_TRUE(saml::verify_response(wrapped, fixture.expect(entra_issuer, fixture_time(12, 1)))
                    .has_value());
}

TEST(SamlResponse, RefusesTamperedResponses) {
    const Fixture fixture;
    const std::string okta = read_file("tests/fixtures/saml/okta-response.xml");
    const std::string entra = read_file("tests/fixtures/saml/entra-response.xml");
    const auto at = fixture_time(12, 1);
    expect_refused(
        saml::verify_response(base64(replace_once(entra, ">entra-subject-1<", ">admin<")),
                              fixture.expect(entra_issuer, at)),
        "digest does not match");
    expect_refused(saml::verify_response(base64(replace_once(entra, "Grace Hopper", "Mallory")),
                                         fixture.expect(entra_issuer, at)),
                   "digest does not match");
    // The Okta response is signed twice; changing the assertion breaks both digests.
    expect_refused(saml::verify_response(base64(replace_once(okta,
                                                             ">ada@example.com</saml2:NameID>",
                                                             ">eve@example.com</saml2:NameID>")),
                                         fixture.expect(okta_issuer, at)),
                   "digest does not match");
    // A changed SignatureValue.
    const std::size_t value = entra.find("<SignatureValue>") + 20;
    std::string forged = entra;
    forged[value] = forged[value] == 'A' ? 'B' : 'A';
    expect_refused(saml::verify_response(base64(forged), fixture.expect(entra_issuer, at)),
                   "signature is invalid");
    // Another certificate.
    Idp other;
    const std::string other_certificate = other.certificate();
    auto expected = fixture.expect(entra_issuer, at);
    expected.certificate_pem = other_certificate;
    expect_refused(saml::verify_response(base64(entra), expected), "signature is invalid");
}

TEST(SamlResponse, RefusesWrongAudienceDestinationIssuerAndTime) {
    const Fixture fixture;
    const std::string entra = read_file("tests/fixtures/saml/entra-response.xml");
    const auto at = fixture_time(12, 1);
    auto audience = fixture.expect(entra_issuer, at);
    const std::string other_sp = saml::sp_entity_id(fixture_host, "another-provider");
    audience.sp_entity_id = other_sp;
    expect_refused(saml::verify_response(base64(entra), audience), "another audience");
    auto destination = fixture.expect(entra_issuer, at);
    const std::string other_acs = saml::acs_url(fixture_host, "another-provider");
    destination.acs_url = other_acs;
    expect_refused(saml::verify_response(base64(entra), destination), "another destination");
    expect_refused(saml::verify_response(base64(entra), fixture.expect("https://other.idp/", at)),
                   "issuer is not accepted");
    // NotOnOrAfter is 12:05 with two minutes of skew: refused from 12:07.
    EXPECT_TRUE(
        saml::verify_response(base64(entra), fixture.expect(entra_issuer, fixture_time(12, 6)))
            .has_value());
    expect_refused(
        saml::verify_response(base64(entra), fixture.expect(entra_issuer, fixture_time(12, 7))),
        "expired");
    // NotBefore is 11:59: refused before 11:57.
    expect_refused(
        saml::verify_response(base64(entra), fixture.expect(entra_issuer, fixture_time(11, 56))),
        "not valid yet");
}

// --- The checks around the signature ----------------------------------------------------------

TEST(SamlResponse, RefusesAWrongRecipientAndAnswersNoRequest) {
    const Signed signer;
    auto recipient = signer.shape();
    recipient.destination.clear();
    recipient.recipient = saml::acs_url(fixture_host, "another-provider");
    expect_refused(signer.verify(signer.xml(recipient)), "another recipient");
    auto unsolicited = signer.shape();
    unsolicited.in_response_to.clear();
    expect_refused(signer.verify(signer.xml(unsolicited)), "unsolicited");
    auto failed = signer.shape();
    failed.status = "urn:oasis:names:tc:SAML:2.0:status:Requester";
    expect_refused(signer.verify(signer.xml(failed)), "did not sign the person in");
    const auto good = signer.verify(signer.xml(signer.shape()));
    ASSERT_TRUE(good.has_value()) << good.error();
    EXPECT_EQ(good->subject, "person-1");
    EXPECT_EQ(good->in_response_to, "_request");
}

TEST(SamlResponse, RefusesUnsignedEncryptedAndWeaklySignedResponses) {
    const Signed signer;
    const std::string unsigned_xml = replace_once(response_xml(signer.shape()), "<SIGNATURE/>", "");
    expect_refused(signer.verify(unsigned_xml), "not signed");
    const std::string encrypted =
        replace_once(unsigned_xml,
                     "</samlp:Response>",
                     "<saml:EncryptedAssertion><x/></saml:EncryptedAssertion></samlp:Response>");
    expect_refused(signer.verify(encrypted), "encrypted");
    expect_refused(
        signer.verify(signer.idp.sign_assertion(response_xml(signer.shape()),
                                                "_assertion-1",
                                                "http://www.w3.org/2000/09/xmldsig#rsa-sha1")),
        "algorithm is not accepted");
    std::string xpath = signer.xml(signer.shape());
    xpath =
        replace_once(xpath,
                     "<ds:Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"/>",
                     "<ds:Transform Algorithm=\"http://www.w3.org/TR/1999/REC-xpath-19991116\"/>");
    expect_refused(signer.verify(xpath), "transforms");
}

TEST(SamlResponse, RefusesSignatureWrapping) {
    const Signed signer;
    const std::string genuine = signer.xml(signer.shape());
    const std::size_t start = genuine.find("<saml:Assertion");
    const std::size_t end = genuine.find("</saml:Assertion>") + 17;
    const std::string signed_assertion = genuine.substr(start, end - start);
    auto evil_shape = signer.shape();
    evil_shape.subject = "admin";
    const std::string evil_unsigned = replace_once(response_xml(evil_shape), "<SIGNATURE/>", "");
    const std::string evil_assertion = evil_unsigned.substr(
        evil_unsigned.find("<saml:Assertion"),
        evil_unsigned.find("</saml:Assertion>") + 17 - evil_unsigned.find("<saml:Assertion"));

    // The genuine signed assertion moved into Extensions, an unsigned one in its place, with the
    // same ID so the reference would resolve to either.
    const std::string moved = replace_once(genuine,
                                           signed_assertion,
                                           "<samlp:Extensions>" + signed_assertion +
                                               "</samlp:Extensions>" + evil_assertion);
    expect_refused(signer.verify(moved), "exactly one assertion");

    // The genuine signature lifted onto an assertion it does not cover.
    const std::size_t signature_start = signed_assertion.find("<ds:Signature");
    const std::size_t signature_end = signed_assertion.find("</ds:Signature>") + 15;
    const std::string signature =
        signed_assertion.substr(signature_start, signature_end - signature_start);
    const std::string lifted =
        replace_once(genuine,
                     signed_assertion,
                     replace_once(evil_assertion, "</saml:Issuer>", "</saml:Issuer>" + signature));
    expect_refused(signer.verify(lifted), "digest does not match");

    // A second, fake Signature inside the assertion carrying a digest of the forged content:
    // only the response's and the assertion's own signature positions are accepted.
    std::string forged = replace_once(genuine, ">person-1<", ">admin<");
    forged =
        replace_once(forged,
                     "<saml:Subject>",
                     "<saml:Advice><ds:Signature xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\">"
                     "<ds:DigestValue>AAAA</ds:DigestValue>"
                     "</ds:Signature></saml:Advice><saml:Subject>");
    expect_refused(signer.verify(forged), "signature where none belongs");

    // A reference to another element's ID.
    expect_refused(
        signer.verify(signer.idp.sign_assertion(response_xml(signer.shape()), "_response-1")),
        "does not reference the element it signs");

    // Two elements sharing the signed ID.
    const std::string duplicated = replace_once(
        genuine, "<samlp:Status>", "<samlp:Extensions ID=\"_assertion-1\"/><samlp:Status>");
    expect_refused(signer.verify(duplicated), "does not reference the element it signs");
}

TEST(SamlResponse, ACommentInsideTheNameIdIsPartOfTheSignedValue) {
    // The value the provider signed is the whole text; reading only the first text node would
    // sign in "admin@example.com" (CVE-2017-11427 and its kin).
    const Signed signer;
    auto shape = signer.shape();
    shape.subject = "admin@example.com.evil.test";
    std::string document = signer.xml(shape);
    document = replace_once(
        document, "admin@example.com.evil.test", "admin@example.com<!-- -->.evil.test");
    const auto assertion = signer.verify(document);
    ASSERT_TRUE(assertion.has_value()) << assertion.error();
    EXPECT_EQ(assertion->subject, "admin@example.com.evil.test");
}

// --- AuthnRequest and metadata ----------------------------------------------------------------

namespace {

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

std::string url_encode(std::string_view value) {
    std::string encoded;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            encoded += static_cast<char>(byte);
        } else {
            encoded += std::format("%{:02X}", static_cast<unsigned>(byte));
        }
    }
    return encoded;
}

std::string base64_decode(std::string_view value) {
    std::string decoded((value.size() / 4) * 3, '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    EXPECT_GE(size, 0);
    const std::size_t padding = value.ends_with("==") ? 2 : value.ends_with('=') ? 1 : 0;
    decoded.resize(static_cast<std::size_t>(std::max(size, 0)) - padding);
    return decoded;
}

// Inflates DEFLATE stored blocks, the only kind Lake writes.
std::string inflate_stored(std::string_view value) {
    std::string out;
    std::size_t position = 0;
    for (;;) {
        EXPECT_LE(position + 5, value.size());
        if (position + 5 > value.size()) {
            return out;
        }
        const auto header = static_cast<unsigned char>(value[position]);
        EXPECT_EQ(header & 0x06, 0) << "not a stored block";
        const std::size_t size = static_cast<unsigned char>(value[position + 1]) |
                                 (static_cast<unsigned char>(value[position + 2]) << 8);
        out.append(value.substr(position + 5, size));
        position += 5 + size;
        if ((header & 1) != 0) {
            return out;
        }
    }
}

// The AuthnRequest an HTTP-Redirect URL carries.
std::string authn_request(std::string_view url) {
    const std::size_t at = url.find("SAMLRequest=");
    EXPECT_NE(at, std::string_view::npos) << url;
    std::string_view encoded = url.substr(at + 12);
    encoded = encoded.substr(0, encoded.find('&'));
    return inflate_stored(base64_decode(url_decode(encoded)));
}

std::string attribute_in(std::string_view xml, std::string_view name) {
    const std::string prefix = " " + std::string(name) + "=\"";
    const std::size_t at = xml.find(prefix);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t start = at + prefix.size();
    return std::string(xml.substr(start, xml.find('"', start) - start));
}

std::string idp_metadata(std::string_view entity_id, std::string_view location) {
    return "<?xml version=\"1.0\"?><md:EntityDescriptor "
           "xmlns:md=\"urn:oasis:names:tc:SAML:2.0:metadata\" entityID=\"" +
           std::string(entity_id) +
           "\"><md:IDPSSODescriptor "
           "protocolSupportEnumeration=\"urn:oasis:names:tc:SAML:2.0:protocol\"><md:"
           "SingleSignOnService Binding=\"urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST\" "
           "Location=\"https://idp.test/sso/post\"/><md:SingleSignOnService "
           "Binding=\"urn:oasis:names:tc:SAML:2.0:bindings:HTTP-Redirect\" Location=\"" +
           std::string(location) + "\"/></md:IDPSSODescriptor></md:EntityDescriptor>";
}

} // namespace

TEST(SamlRequest, TheRedirectUrlCarriesAnAuthnRequestForLakesAcs) {
    const std::string acs = saml::acs_url(fixture_host, fixture_provider);
    const std::string sp = saml::sp_entity_id(fixture_host, fixture_provider);
    const std::string url = saml::authn_request_url(
        "https://idp.test/sso?tenant=1", "_abc", sp, acs, fixture_time(12, 0));
    EXPECT_TRUE(url.starts_with("https://idp.test/sso?tenant=1&SAMLRequest=")) << url;
    const std::string request = authn_request(url);
    EXPECT_EQ(attribute_in(request, "ID"), "_abc");
    EXPECT_EQ(attribute_in(request, "AssertionConsumerServiceURL"), acs);
    EXPECT_EQ(attribute_in(request, "IssueInstant"), "2026-10-21T12:00:00Z");
    EXPECT_EQ(attribute_in(request, "Destination"), "https://idp.test/sso?tenant=1");
    EXPECT_NE(request.find("<saml:Issuer>" + sp + "</saml:Issuer>"), std::string::npos) << request;
    EXPECT_TRUE(saml::canonicalize_exclusive(request).has_value());
    const std::string id = saml::new_request_id();
    EXPECT_EQ(id.size(), 65U);
    EXPECT_NE(id, saml::new_request_id());
}

TEST(SamlMetadata, ReadsTheIssuersRedirectSignOnUrlAndPublishesLakesOwn) {
    const auto location = saml::idp_sso_url(
        idp_metadata("https://idp.test/saml", "https://idp.test/sso"), "https://idp.test/saml");
    ASSERT_TRUE(location.has_value()) << location.error();
    EXPECT_EQ(*location, "https://idp.test/sso");
    EXPECT_FALSE(saml::idp_sso_url(idp_metadata("https://other.test/", "https://idp.test/sso"),
                                   "https://idp.test/saml")
                     .has_value());
    EXPECT_FALSE(saml::idp_sso_url(idp_metadata("https://idp.test/saml", "http://idp.test/sso"),
                                   "https://idp.test/saml")
                     .has_value());
    EXPECT_FALSE(saml::idp_sso_url("<EntityDescriptor entityID=\"https://idp.test/saml\"/>",
                                   "https://idp.test/saml")
                     .has_value());
    const std::string metadata = saml::sp_metadata(fixture_host, fixture_provider);
    EXPECT_TRUE(saml::canonicalize_exclusive(metadata).has_value());
    EXPECT_EQ(attribute_in(metadata, "entityID"),
              saml::sp_entity_id(fixture_host, fixture_provider));
    EXPECT_EQ(attribute_in(metadata, "Location"), saml::acs_url(fixture_host, fixture_provider));
}

// --- Sign-in end to end -----------------------------------------------------------------------

namespace {

std::string http(unsigned short port,
                 std::string_view method,
                 std::string_view target,
                 std::string_view body = {},
                 std::string_view content_type = {},
                 std::string_view extra_headers = {}) {
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
    message += std::string(extra_headers);
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

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct SamlLake {
    Idp idp;
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    unsigned short port = free_port();
    std::string host = "127.0.0.1:" + std::to_string(port);
    std::string provider_id;
    std::unique_ptr<lake::api::ApiServer> server;

    SamlLake() {
        const auto organization = store->create_organization({"", "SAML organization"});
        EXPECT_TRUE(organization) << organization.error().message;
        const auto provider = store->register_sso_provider(
            {organization->id,
             "lake",
             "corporate-saml",
             "saml",
             "https://idp.test/saml",
             idp_metadata("https://idp.test/saml", "https://idp.test/sso"),
             idp.certificate(),
             "",
             "",
             "scim-saml-token",
             nlohmann::json::object(),
             true});
        EXPECT_TRUE(provider) << provider.error().message;
        provider_id = provider->id;
        EXPECT_TRUE(
            store->create_federation_issuer({organization->id, "https://idp.test/saml", "email"}));
        lake::api::ServerOptions options;
        options.port = port;
        options.public_host = host;
        options.store = store;
        server = std::make_unique<lake::api::ApiServer>(options);
        EXPECT_TRUE(server->start()) << server->reason();
    }
    ~SamlLake() {
        server->stop();
    }

    // start_sso_saml, as a page or an MCP client calls it; answers the request ID.
    std::string start(std::string_view taap_id, std::string_view return_to = {}) {
        json input{{"provider_id", provider_id}, {"taap_id", taap_id}};
        if (!return_to.empty()) {
            input["return_to"] = return_to;
        }
        const std::string started =
            http(port, "POST", "/v1/start_sso_saml", input.dump(), "application/json");
        EXPECT_NE(started.find(" 200 "), std::string::npos) << started;
        const std::string url =
            json::parse(body_of(started))["value"]["authorization_url"].get<std::string>();
        EXPECT_TRUE(url.starts_with("https://idp.test/sso?SAMLRequest=")) << url;
        return attribute_in(authn_request(url), "ID");
    }

    std::string response(std::string_view request_id,
                         std::string_view assertion_id = "_assertion-1",
                         std::string_view subject = "person-1") const {
        ResponseShape shape;
        shape.destination = saml::acs_url(host, provider_id);
        shape.recipient = shape.destination;
        shape.audience = saml::sp_entity_id(host, provider_id);
        shape.in_response_to = request_id;
        shape.assertion_id = assertion_id;
        shape.subject = subject;
        shape.issued = now_seconds() - 5;
        return base64(idp.sign_assertion(response_xml(shape), shape.assertion_id));
    }

    std::string post(const std::string& saml_response, std::string_view extra_headers = {}) {
        return http(port,
                    "POST",
                    "/v1/saml/" + provider_id + "/acs",
                    "SAMLResponse=" + url_encode(saml_response) + "&RelayState=ignored",
                    "application/x-www-form-urlencoded",
                    extra_headers);
    }
};

} // namespace

TEST(SamlSignIn, TheAcsSignsInOnceAndRefusesReplayTamperingAndUnsolicitedResponses) {
    SamlLake lake;
    const std::string request = lake.start("taap:1:ed25519:saml-person", "/w/home?tab=1");
    ASSERT_FALSE(request.empty());

    const std::string response = lake.response(request);
    // The provider posts cross-site; a browser that still sends an old session cookie is not
    // refused, because the ACS never reads it.
    const std::string signed_in =
        lake.post(response, "Cookie: lake_session=stale\r\nOrigin: https://idp.test\r\n");
    ASSERT_NE(signed_in.find(" 302 "), std::string::npos) << signed_in;
    EXPECT_EQ(header(signed_in, "Location"), "/w/home?tab=1");
    const std::string cookie = header(signed_in, "Set-Cookie");
    ASSERT_TRUE(cookie.starts_with("lake_session=")) << cookie;
    const std::string secret = cookie.substr(13, cookie.find(';') - 13);
    const auto principal = lake.store->context_for(secret);
    ASSERT_TRUE(principal) << principal.error().message;
    // lake#872: the just-in-time account takes a managed key Lake minted, not the TAAP id the
    // unauthenticated start named.
    EXPECT_NE(principal->taap_id, "taap:1:ed25519:saml-person");
    EXPECT_TRUE(principal->taap_id.starts_with("taap:1:ed25519:")) << principal->taap_id;
    EXPECT_EQ(lake.store->account_identity(principal->user_id)->key, "managed");

    // The same response again: its request is used and its assertion recorded.
    const std::string replayed = lake.post(response);
    EXPECT_NE(replayed.find(" 403 "), std::string::npos) << replayed;
    // The same assertion id answering a fresh request is refused too.
    const std::string second_request = lake.start("taap:1:ed25519:saml-person");
    const std::string reused = lake.post(lake.response(second_request, "_assertion-1"));
    EXPECT_NE(reused.find(" 403 "), std::string::npos) << reused;
    EXPECT_NE(reused.find("already used"), std::string::npos) << reused;
    // ...and the request it consumed nothing from still works with a new assertion.
    const std::string fresh = lake.post(lake.response(second_request, "_assertion-2"));
    EXPECT_NE(fresh.find(" 302 "), std::string::npos) << fresh;
    EXPECT_EQ(header(fresh, "Location"), "/");

    // A response to a request Lake never made.
    const std::string unsolicited = lake.post(lake.response("_never-requested", "_assertion-3"));
    EXPECT_NE(unsolicited.find(" 403 "), std::string::npos) << unsolicited;

    // Tampered: a changed subject under the original signature.
    const std::string third_request = lake.start("taap:1:ed25519:saml-person");
    std::string tampered = base64_decode(lake.response(third_request, "_assertion-4"));
    tampered = replace_once(tampered, ">person-1<", ">admin<");
    const std::string refused = lake.post(base64(tampered));
    EXPECT_NE(refused.find(" 403 "), std::string::npos) << refused;
    const auto events = lake::test::audit_events(*lake.store, lake::audit::Filter{.limit = 200});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.method == "saml" && event.draft.outcome == "denied";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.method == "saml" && event.draft.outcome == "success";
    }));

    // Another provider's ACS refuses this provider's response.
    const std::string elsewhere =
        http(lake.port,
             "POST",
             "/v1/saml/00000000-0000-0000-0000-000000000000/acs",
             "SAMLResponse=" + url_encode(lake.response(third_request, "_assertion-5")),
             "application/x-www-form-urlencoded");
    EXPECT_EQ(elsewhere.find(" 302 "), std::string::npos) << elsewhere;
}

TEST(SamlSignIn, MetadataAndStartRoutesAndTheCapabilityPath) {
    SamlLake lake;
    const std::string metadata =
        http(lake.port, "GET", "/v1/saml/" + lake.provider_id + "/metadata");
    ASSERT_NE(metadata.find(" 200 "), std::string::npos) << metadata;
    EXPECT_NE(header(metadata, "Content-Type").find("application/samlmetadata+xml"),
              std::string::npos);
    EXPECT_EQ(attribute_in(body_of(metadata), "entityID"),
              saml::sp_entity_id(lake.host, lake.provider_id));
    EXPECT_EQ(attribute_in(body_of(metadata), "Location"),
              saml::acs_url(lake.host, lake.provider_id));
    const std::string missing =
        http(lake.port, "GET", "/v1/saml/00000000-0000-0000-0000-000000000000/metadata");
    EXPECT_NE(missing.find(" 404 "), std::string::npos) << missing;

    // The start route is the provider's sign-on URL for its app tile.
    const std::string started =
        http(lake.port, "GET", "/v1/saml/" + lake.provider_id + "/start?return_to=//evil.test/");
    ASSERT_NE(started.find(" 302 "), std::string::npos) << started;
    const std::string location = header(started, "Location");
    EXPECT_TRUE(location.starts_with("https://idp.test/sso?SAMLRequest=")) << location;
    const std::string request = attribute_in(authn_request(location), "ID");
    ASSERT_FALSE(request.empty());

    // sign_in_saml takes the same response through the registry: an unknown request is refused,
    // an outstanding one signs in, provisioning the person just in time with a managed key; the
    // taap_id it is given is ignored (lake#872).
    const auto refused = http(lake.port,
                              "POST",
                              "/v1/sign_in_saml",
                              json{{"provider_id", lake.provider_id},
                                   {"saml_response", lake.response("_unknown", "_assertion-9")}}
                                  .dump(),
                              "application/json");
    EXPECT_EQ(refused.find(" 200 "), std::string::npos) << refused;
    const auto accepted = http(lake.port,
                               "POST",
                               "/v1/sign_in_saml",
                               json{{"provider_id", lake.provider_id},
                                    {"saml_response", lake.response(request, "_assertion-10")},
                                    {"taap_id", "taap:1:ed25519:saml-capability"}}
                                   .dump(),
                               "application/json");
    ASSERT_NE(accepted.find(" 200 "), std::string::npos) << accepted;
    const auto person = lake.store->context_for(json::parse(body_of(accepted))["value"]["secret"]);
    ASSERT_TRUE(person.has_value());
    EXPECT_NE(person->taap_id, "taap:1:ed25519:saml-capability");
}

TEST(SamlSignIn, TheStoreAcceptsEachRequestAndAssertionOnce) {
    const auto check = [](lake::control::Store& store) {
        const auto organization = store.create_organization({"", "SAML store organization"});
        ASSERT_TRUE(organization) << organization.error().message;
        Idp idp;
        const auto provider = store.register_sso_provider(
            {organization->id,
             "lake",
             "saml-store-" + std::to_string(now_seconds()) + "-" + std::to_string(getpid()),
             "saml",
             "https://idp.test/saml/" + std::to_string(getpid()),
             idp_metadata("https://idp.test/saml", "https://idp.test/sso"),
             idp.certificate(),
             "",
             "",
             "scim-saml-store-token",
             nlohmann::json::object(),
             true});
        ASSERT_TRUE(provider) << provider.error().message;
        const std::string request = saml::new_request_id();
        ASSERT_TRUE(store.begin_saml({provider->id, request, "laptop", "taap:1:x", "/next"}));
        EXPECT_FALSE(
            store.begin_saml({"00000000-0000-0000-0000-000000000000", request, "", "", ""}));
        const std::int64_t expires = now_seconds() + 300;
        const auto consumed = store.consume_saml({provider->id, request, "_a1", expires});
        ASSERT_TRUE(consumed) << consumed.error().message;
        EXPECT_EQ(consumed->device_name, "laptop");
        EXPECT_EQ(consumed->taap_id, "taap:1:x");
        EXPECT_EQ(consumed->return_to, "/next");
        // The request is used.
        EXPECT_FALSE(store.consume_saml({provider->id, request, "_a2", expires}));
        // The assertion is used, even for a new request; that request stays outstanding.
        const std::string second = saml::new_request_id();
        ASSERT_TRUE(store.begin_saml({provider->id, second, "laptop", "", ""}));
        const auto replayed = store.consume_saml({provider->id, second, "_a1", expires});
        ASSERT_FALSE(replayed);
        EXPECT_EQ(replayed.error().code, "unauthorized");
        EXPECT_TRUE(store.consume_saml({provider->id, second, "_a3", expires}));
        // Another provider's request is not this provider's.
        const std::string third = saml::new_request_id();
        ASSERT_TRUE(store.begin_saml({provider->id, third, "laptop", "", ""}));
        EXPECT_FALSE(
            store.consume_saml({"00000000-0000-0000-0000-000000000000", third, "_a4", expires}));
    };
    lake::control::InMemoryStore memory;
    check(memory);
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || *uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured for the Postgres half";
    }
    auto postgres = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    check(*postgres);
}
