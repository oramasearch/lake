#pragma once

// SAML 2.0 Web Browser SSO for an enterprise SSO provider (docs/api/sso.md#saml-sign-in, lake#836).
//
// Lake is the service provider. It sends an AuthnRequest over the HTTP-Redirect binding and takes
// the identity provider's Response over the HTTP-POST binding at its Assertion Consumer Service.
// Verification is Lake's own (no XML or XML-security library; liquid does the same
// for X.509): a strict XML reader that refuses DTDs, entities other than the predefined ones and
// processing instructions, Exclusive XML Canonicalization 1.0 without comments, and XML Signature
// verification with the provider's registered certificate. Every value Lake uses is read from the
// element the signature covers.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lake::control::saml {

// What a response must match. Times are Unix seconds.
struct Expectations {
    std::string_view certificate_pem; // the provider's registered signing certificate
    std::string_view idp_entity_id;   // the provider's issuer
    std::string_view sp_entity_id;    // sp_entity_id(public_host, provider id)
    std::string_view acs_url;         // acs_url(public_host, provider id)
    std::int64_t now = 0;
    std::int64_t skew_seconds = 120;
};

struct Assertion {
    std::string id;              // the assertion's ID, accepted once (replay)
    std::string in_response_to;  // the AuthnRequest it answers
    std::int64_t expires_at = 0; // the earliest NotOnOrAfter it carries
    std::string issuer;
    std::string subject;       // the NameID
    nlohmann::json attributes; // each Attribute's Name and its first value
};

// The most a decoded response or a metadata document may be.
inline constexpr std::size_t maximum_document_bytes = 1024 * 1024;

std::string sp_entity_id(std::string_view public_host, std::string_view provider_id);
std::string acs_url(std::string_view public_host, std::string_view provider_id);
std::string start_url(std::string_view public_host, std::string_view provider_id);
// Lake's service-provider metadata for one provider (an md:EntityDescriptor).
std::string sp_metadata(std::string_view public_host, std::string_view provider_id);

// The identity provider's HTTP-Redirect SingleSignOnService location from its metadata, whose
// entityID must be `idp_entity_id`. HTTPS only, except loopback for tests.
std::expected<std::string, std::string> idp_sso_url(std::string_view metadata,
                                                    std::string_view idp_entity_id);

// A fresh AuthnRequest ID ("_" and 64 random hex digits); empty when randomness fails.
std::string new_request_id();
// The HTTP-Redirect URL that carries an AuthnRequest (raw DEFLATE, base64, URL-encoded).
std::string authn_request_url(std::string_view idp_sso_url,
                              std::string_view request_id,
                              std::string_view sp_entity_id,
                              std::string_view acs_url,
                              std::int64_t now);

// Verifies a base64 SAMLResponse against `expected` and returns the assertion it carries. It
// fails closed: anything it does not understand is refused.
std::expected<Assertion, std::string> verify_response(std::string_view response_base64,
                                                      const Expectations& expected);

// Exclusive XML Canonicalization 1.0, without comments, of a document's root element.
// `inclusive_prefixes` is an InclusiveNamespaces PrefixList ("#default" names the default
// namespace). Exposed for the conformance tests and the tests' signing helper.
std::expected<std::string, std::string>
canonicalize_exclusive(std::string_view xml, std::string_view inclusive_prefixes = {});

// The same, of the root's first descendant element named `local_name` (any namespace), in the
// namespace context of the whole document, with `omit_local_name` descendants left out (the
// enveloped-signature transform when it is "Signature"). For the tests' signing helper.
std::expected<std::string, std::string>
canonicalize_element(std::string_view xml,
                     std::string_view local_name,
                     std::string_view omit_local_name = {},
                     std::string_view inclusive_prefixes = {});

} // namespace lake::control::saml
