"""Writes the SAML response fixtures that tests/test_saml.cpp verifies (lake#836).

The responses are signed by signxml (lxml and libxml2's canonicalization), an implementation
independent of Lake's, so the tests show Lake verifies what a real XML-security library produces.
They are shaped like Okta's (response and assertion both signed, `saml2:` prefixes, an
InclusiveNamespaces prefix list) and Microsoft Entra's (only the assertion signed, the default
namespace for both the assertion and the signature).

Each run makes a new key, so it rewrites every file together. Not part of the build; regenerate with

    uv run --no-project --with signxml python -I tests/fixtures/saml/make-fixtures.py

The private key is never written.
"""

import datetime
import pathlib

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
from lxml import etree
from signxml import XMLSigner

HERE = pathlib.Path(__file__).resolve().parent
EXC_C14N = "http://www.w3.org/2001/10/xml-exc-c14n#"
DSIG = "http://www.w3.org/2000/09/xmldsig#"

# Kept in step with the constants in tests/test_saml.cpp.
PROVIDER = "11111111-1111-1111-1111-111111111111"
HOST = "lake.test"
ACS = f"https://{HOST}/v1/saml/{PROVIDER}/acs"
AUDIENCE = f"https://{HOST}/v1/saml/{PROVIDER}/metadata"
REQUEST = "_fixture-request"
ISSUED = "2026-10-21T12:00:00Z"
NOT_BEFORE = "2026-10-21T11:59:00Z"
NOT_ON_OR_AFTER = "2026-10-21T12:05:00Z"


def key_and_certificate():
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "lake-saml-fixture")])
    now = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(1)
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=3650))
        .sign(key, hashes.SHA256())
    )
    return key, certificate.public_bytes(serialization.Encoding.PEM)


class EnvelopedSigner(XMLSigner):
    """signxml canonicalizes an enveloped reference with the InclusiveNamespaces prefix list but
    leaves the list out of the reference's exclusive-canonicalization Transform, so no verifier can
    reproduce the digest. This writes the list where XML Signature puts it; the canonicalization
    and the digest are still signxml's."""

    def _build_transforms_for_reference(self, *, transforms_node, reference, exclude_c14n_transform_element=True):
        super()._build_transforms_for_reference(
            transforms_node=transforms_node,
            reference=reference,
            exclude_c14n_transform_element=exclude_c14n_transform_element,
        )
        if reference.inclusive_ns_prefixes:
            transform = transforms_node[-1]
            etree.SubElement(
                transform,
                "{%s}InclusiveNamespaces" % EXC_C14N,
                PrefixList=" ".join(reference.inclusive_ns_prefixes),
            )


def signer(default_namespace=False):
    result = EnvelopedSigner(c14n_algorithm=EXC_C14N)
    if default_namespace:
        result.namespaces = {None: DSIG}
    return result


def sign(element, key, certificate, identifier, prefixes=None, default_namespace=False):
    return signer(default_namespace).sign(
        element,
        key=key,
        cert=certificate.decode(),
        reference_uri="#" + identifier,
        inclusive_ns_prefixes=prefixes,
    )


def replace(parent, old, new):
    parent.replace(old, new)


def okta(key, certificate):
    issuer = "http://www.okta.com/exkFixture"
    text = f"""<saml2p:Response xmlns:saml2p="urn:oasis:names:tc:SAML:2.0:protocol" Destination="{ACS}" ID="id-okta-response" InResponseTo="{REQUEST}" IssueInstant="{ISSUED}" Version="2.0"><saml2:Issuer xmlns:saml2="urn:oasis:names:tc:SAML:2.0:assertion" Format="urn:oasis:names:tc:SAML:2.0:nameid-format:entity">{issuer}</saml2:Issuer><ds:Signature xmlns:ds="{DSIG}" Id="placeholder"></ds:Signature><saml2p:Status xmlns:saml2p="urn:oasis:names:tc:SAML:2.0:protocol"><saml2p:StatusCode Value="urn:oasis:names:tc:SAML:2.0:status:Success"/></saml2p:Status><saml2:Assertion xmlns:saml2="urn:oasis:names:tc:SAML:2.0:assertion" xmlns:xs="http://www.w3.org/2001/XMLSchema" ID="id-okta-assertion" IssueInstant="{ISSUED}" Version="2.0"><saml2:Issuer Format="urn:oasis:names:tc:SAML:2.0:nameid-format:entity">{issuer}</saml2:Issuer><ds:Signature xmlns:ds="{DSIG}" Id="placeholder"></ds:Signature><saml2:Subject><saml2:NameID Format="urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress">ada@example.com</saml2:NameID><saml2:SubjectConfirmation Method="urn:oasis:names:tc:SAML:2.0:cm:bearer"><saml2:SubjectConfirmationData InResponseTo="{REQUEST}" NotOnOrAfter="{NOT_ON_OR_AFTER}" Recipient="{ACS}"/></saml2:SubjectConfirmation></saml2:Subject><saml2:Conditions NotBefore="{NOT_BEFORE}" NotOnOrAfter="{NOT_ON_OR_AFTER}"><saml2:AudienceRestriction><saml2:Audience>{AUDIENCE}</saml2:Audience></saml2:AudienceRestriction></saml2:Conditions><saml2:AuthnStatement AuthnInstant="{ISSUED}" SessionIndex="id-okta-session"><saml2:AuthnContext><saml2:AuthnContextClassRef>urn:oasis:names:tc:SAML:2.0:ac:classes:PasswordProtectedTransport</saml2:AuthnContextClassRef></saml2:AuthnContext></saml2:AuthnStatement><saml2:AttributeStatement><saml2:Attribute Name="email" NameFormat="urn:oasis:names:tc:SAML:2.0:attrname-format:unspecified"><saml2:AttributeValue xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:type="xs:string">ada@example.com</saml2:AttributeValue></saml2:Attribute><saml2:Attribute Name="name" NameFormat="urn:oasis:names:tc:SAML:2.0:attrname-format:unspecified"><saml2:AttributeValue xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:type="xs:string">Ada Lovelace</saml2:AttributeValue></saml2:Attribute></saml2:AttributeStatement></saml2:Assertion></saml2p:Response>"""
    response = etree.fromstring(text)
    assertion = response.find("{urn:oasis:names:tc:SAML:2.0:assertion}Assertion")
    signed_assertion = sign(assertion, key, certificate, "id-okta-assertion", prefixes=["xs"])
    replace(response, assertion, signed_assertion)
    return sign(response, key, certificate, "id-okta-response", prefixes=["xs"])


def entra(key, certificate):
    issuer = "https://sts.windows.net/00000000-0000-0000-0000-00000000f1c5/"
    text = f"""<samlp:Response xmlns:samlp="urn:oasis:names:tc:SAML:2.0:protocol" ID="_entra-response" Version="2.0" IssueInstant="{ISSUED}" Destination="{ACS}" InResponseTo="{REQUEST}"><Issuer xmlns="urn:oasis:names:tc:SAML:2.0:assertion">{issuer}</Issuer><samlp:Status><samlp:StatusCode Value="urn:oasis:names:tc:SAML:2.0:status:Success"/></samlp:Status><Assertion xmlns="urn:oasis:names:tc:SAML:2.0:assertion" ID="_entra-assertion" IssueInstant="{ISSUED}" Version="2.0"><Issuer>{issuer}</Issuer><Signature xmlns="{DSIG}" Id="placeholder"></Signature><Subject><NameID Format="urn:oasis:names:tc:SAML:2.0:nameid-format:persistent">entra-subject-1</NameID><SubjectConfirmation Method="urn:oasis:names:tc:SAML:2.0:cm:bearer"><SubjectConfirmationData InResponseTo="{REQUEST}" NotOnOrAfter="{NOT_ON_OR_AFTER}" Recipient="{ACS}"/></SubjectConfirmation></Subject><Conditions NotBefore="{NOT_BEFORE}" NotOnOrAfter="{NOT_ON_OR_AFTER}"><AudienceRestriction><Audience>{AUDIENCE}</Audience></AudienceRestriction></Conditions><AttributeStatement><Attribute Name="http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress"><AttributeValue>grace@example.com</AttributeValue></Attribute><Attribute Name="http://schemas.microsoft.com/identity/claims/displayname"><AttributeValue>Grace Hopper</AttributeValue></Attribute></AttributeStatement><AuthnStatement AuthnInstant="{ISSUED}" SessionIndex="_entra-session"><AuthnContext><AuthnContextClassRef>urn:oasis:names:tc:SAML:2.0:ac:classes:Password</AuthnContextClassRef></AuthnContext></AuthnStatement></Assertion></samlp:Response>"""
    response = etree.fromstring(text)
    assertion = response.find("{urn:oasis:names:tc:SAML:2.0:assertion}Assertion")
    signed_assertion = sign(assertion, key, certificate, "_entra-assertion", default_namespace=True)
    replace(response, assertion, signed_assertion)
    return response


def main():
    key, certificate = key_and_certificate()
    (HERE / "idp-certificate.pem").write_bytes(certificate)
    for name, build in (("okta-response.xml", okta), ("entra-response.xml", entra)):
        document = build(key, certificate)
        (HERE / name).write_bytes(
            etree.tostring(document, xml_declaration=True, encoding="UTF-8")
        )


if __name__ == "__main__":
    main()
