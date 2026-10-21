#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/saml.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

struct SsoSurface {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext context;

    SsoSurface() {
        lake::api::register_core_capabilities(registry, "test");
        context.store = &store;
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = "sso-test-instance-kek";
    }

    // Through the asynchronous path, as REST and MCP call every capability.
    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id.clear();
        auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        return completed ? std::move(*completed)
                         : ApiResult::failure("store_error", completed.error().message);
    }
};

json provider_input(const std::string& organization_id,
                    const std::string& name,
                    const std::string& protocol,
                    const std::string& issuer) {
    json provider{{"organization_id", organization_id},
                  {"name", name},
                  {"protocol", protocol},
                  {"issuer", issuer},
                  {"scim_token", "token-" + name},
                  {"attribute_mapping", {{"email", "mail"}}},
                  {"jit_provisioning", false}};
    if (protocol == "saml") {
        provider["metadata"] = "<EntityDescriptor entityID=\"" + issuer + "\"/>";
        provider["signing_certificate"] = "stub-saml-certificate";
    } else {
        provider["client_id"] = "stub-oidc-client";
        provider["client_secret"] = "stub-oidc-secret";
    }
    return provider;
}

std::string base64(const unsigned char* value, std::size_t size) {
    std::string encoded(((size + 2) / 3) * 4, '\0');
    const int encoded_size =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), value, size);
    encoded.resize(static_cast<std::size_t>(encoded_size));
    return encoded;
}

std::string sign(EVP_PKEY* key, std::string_view value) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    std::size_t size = 0;
    EXPECT_EQ(EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key), 1);
    EXPECT_EQ(EVP_DigestSignUpdate(context, value.data(), value.size()), 1);
    EXPECT_EQ(EVP_DigestSignFinal(context, nullptr, &size), 1);
    std::vector<unsigned char> signature(size);
    EXPECT_EQ(EVP_DigestSignFinal(context, signature.data(), &size), 1);
    EVP_MD_CTX_free(context);
    signature.resize(size);
    return base64(signature.data(), signature.size());
}

std::string certificate(EVP_PKEY* key) {
    X509* result = X509_new();
    X509_set_version(result, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(result), 1);
    X509_gmtime_adj(X509_get_notBefore(result), 0);
    X509_gmtime_adj(X509_get_notAfter(result), 3600);
    X509_set_pubkey(result, key);
    X509_sign(result, key, EVP_sha256());
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, result);
    char* value = nullptr;
    const long size = BIO_get_mem_data(bio, &value);
    const std::string pem(value, static_cast<std::size_t>(size));
    BIO_free(bio);
    X509_free(result);
    return pem;
}

std::string signed_saml_response(EVP_PKEY* key, const std::string& issuer) {
    const std::string unsigned_assertion =
        "<saml:Assertion ID=\"stub\" xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\" "
        "xmlns:saml=\"urn:oasis:names:tc:SAML:2.0:assertion\">"
        "<saml:Issuer>" +
        issuer +
        "</saml:Issuer><saml:Subject><saml:NameID>saml-signed-user</saml:NameID>"
        "</saml:Subject><saml:Attribute Name=\"email\"><saml:AttributeValue>"
        "signed@example.com</saml:AttributeValue></saml:Attribute></saml:Assertion>";
    const auto canonical = lake::control::saml::canonicalize_exclusive(unsigned_assertion);
    EXPECT_TRUE(canonical.has_value());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    EVP_Digest(canonical->data(), canonical->size(), digest, &digest_size, EVP_sha256(), nullptr);
    const std::string signed_info =
        "<ds:SignedInfo xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\">"
        "<ds:CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"/>"
        "<ds:SignatureMethod Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#rsa-sha256\"/>"
        "<ds:Reference URI=\"#stub\"><ds:DigestMethod "
        "Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\"/>"
        "<ds:DigestValue>" +
        base64(digest, digest_size) + "</ds:DigestValue></ds:Reference></ds:SignedInfo>";
    const auto canonical_info = lake::control::saml::canonicalize_exclusive(signed_info);
    EXPECT_TRUE(canonical_info.has_value());
    const std::string signature = sign(key, *canonical_info);
    const std::string signed_assertion =
        unsigned_assertion.substr(0, unsigned_assertion.rfind("</saml:Assertion>")) +
        "<ds:Signature>" + signed_info + "<ds:SignatureValue>" + signature +
        "</ds:SignatureValue></ds:Signature></saml:Assertion>";
    const std::string response =
        "<samlp:Response xmlns:samlp=\"urn:oasis:names:tc:SAML:2.0:protocol\">" + signed_assertion +
        "</samlp:Response>";
    return base64(reinterpret_cast<const unsigned char*>(response.data()), response.size());
}

TEST(Sso, SamlAndOidcProviderAssertionsRequireFederationPolicy) {
    SsoSurface surface;
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "SSO organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const std::string organization_id = organization.value.at("id");

    const ApiResult saml = surface.call(
        "register_sso_provider",
        provider_input(organization_id, "stub-saml", "saml", "https://saml.stub.example"));
    ASSERT_TRUE(saml.ok) << saml.error.message;
    const ApiResult oidc = surface.call(
        "register_sso_provider",
        provider_input(organization_id, "stub-oidc", "oidc", "https://oidc.stub.example"));
    ASSERT_TRUE(oidc.ok) << oidc.error.message;
    EXPECT_TRUE(surface.call("test_sso_provider", {{"provider_id", saml.value.at("id")}}).ok);
    EXPECT_TRUE(surface.call("test_sso_provider", {{"provider_id", oidc.value.at("id")}}).ok);

    const auto provision = [&](const ApiResult& provider, const std::string& external_id) {
        return surface.call("upsert_scim_user",
                            {{"provider_id", provider.value.at("id")},
                             {"external_id", external_id},
                             {"email", external_id + "@example.com"},
                             {"display_name", external_id},
                             {"taap_id", "taap:1:ed25519:" + external_id},
                             {"active", true}});
    };
    ASSERT_TRUE(provision(saml, "saml-user").ok);
    ASSERT_TRUE(provision(oidc, "oidc-user").ok);

    surface.context.authorized = false;
    surface.context.operator_credential = false;
    const ApiResult denied = surface.call("sign_in_sso",
                                          {{"provider_id", saml.value.at("id")},
                                           {"issuer", "https://saml.stub.example"},
                                           {"subject", "saml-user"},
                                           {"claims", {{"email", "saml-user@example.com"}}}});
    const std::string denied_request = surface.context.request_id;
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");

    surface.context.authorized = true;
    surface.context.operator_credential = true;
    ASSERT_TRUE(surface
                    .call("create_federation_issuer",
                          {{"organization_id", organization_id},
                           {"issuer", "https://saml.stub.example"},
                           {"claim", "email"}})
                    .ok);
    ASSERT_TRUE(surface
                    .call("create_federation_issuer",
                          {{"organization_id", organization_id},
                           {"issuer", "https://oidc.stub.example"},
                           {"claim", "email"}})
                    .ok);

    surface.context.authorized = false;
    surface.context.operator_credential = false;
    const ApiResult saml_sign_in = surface.call("sign_in_sso",
                                                {{"provider_id", saml.value.at("id")},
                                                 {"issuer", "https://saml.stub.example"},
                                                 {"subject", "saml-user"},
                                                 {"device_name", "SAML stub"},
                                                 {"claims", {{"email", "saml-user@example.com"}}}});
    ASSERT_TRUE(saml_sign_in.ok) << saml_sign_in.error.message;
    const ApiResult oidc_sign_in = surface.call("sign_in_sso",
                                                {{"provider_id", oidc.value.at("id")},
                                                 {"issuer", "https://oidc.stub.example"},
                                                 {"subject", "oidc-user"},
                                                 {"device_name", "OIDC stub"},
                                                 {"claims", {{"email", "oidc-user@example.com"}}}});
    ASSERT_TRUE(oidc_sign_in.ok) << oidc_sign_in.error.message;
    EXPECT_TRUE(surface.store.context_for(saml_sign_in.value.at("secret")).has_value());
    EXPECT_TRUE(surface.store.context_for(oidc_sign_in.value.at("secret")).has_value());

    surface.context.authorized = true;
    surface.context.operator_credential = true;
    const ApiResult audit = surface.call("audit", {{"request_id", denied_request}, {"limit", 1}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    EXPECT_TRUE(std::ranges::any_of(audit.value.at("items"), [](const json& event) {
        return event.at("action") == "sign_in_sso" && event.at("outcome") == "denied";
    }));
}

TEST(Sso, SignedSamlAssertionIsVerifiedBeforeSignIn) {
    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    ASSERT_NE(key_context, nullptr);
    ASSERT_EQ(EVP_PKEY_keygen_init(key_context), 1);
    ASSERT_EQ(EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048), 1);
    EVP_PKEY* key = nullptr;
    ASSERT_EQ(EVP_PKEY_keygen(key_context, &key), 1);
    EVP_PKEY_CTX_free(key_context);
    const std::string issuer = "https://saml.signed.stub.example";
    SsoSurface surface;
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Signed SAML organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    json input = provider_input(organization.value.at("id"), "signed-saml", "saml", issuer);
    input["signing_certificate"] = certificate(key);
    input["jit_provisioning"] = true;
    const ApiResult provider = surface.call("register_sso_provider", input);
    ASSERT_TRUE(provider.ok) << provider.error.message;
    ASSERT_TRUE(surface
                    .call("create_federation_issuer",
                          {{"organization_id", organization.value.at("id")},
                           {"issuer", issuer},
                           {"claim", "email"}})
                    .ok);
    surface.context.authorized = false;
    surface.context.operator_credential = false;
    const std::string response = signed_saml_response(key, issuer);
    const ApiResult signed_in = surface.call("sign_in_saml",
                                             {{"provider_id", provider.value.at("id")},
                                              {"saml_response", response},
                                              {"taap_id", "taap:1:ed25519:saml-signed"}});
    ASSERT_TRUE(signed_in.ok) << signed_in.error.message;
    EXPECT_TRUE(surface.store.context_for(signed_in.value.at("secret")).has_value());
    std::string tampered = response;
    tampered[tampered.size() / 2] = tampered[tampered.size() / 2] == 'A' ? 'B' : 'A';
    const ApiResult rejected = surface.call(
        "sign_in_saml", {{"provider_id", provider.value.at("id")}, {"saml_response", tampered}});
    const std::string rejected_request = surface.context.request_id;
    EXPECT_FALSE(rejected.ok);
    surface.context.authorized = true;
    surface.context.operator_credential = true;
    const ApiResult audit = surface.call("audit", {{"request_id", rejected_request}, {"limit", 1}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    EXPECT_TRUE(std::ranges::any_of(audit.value.at("items"), [](const json& event) {
        return event.at("action") == "sign_in_saml" && event.at("outcome") == "denied";
    }));
    EVP_PKEY_free(key);
}

TEST(Scim, UserUpdateGroupReplacementAndDeactivationRevokeSessions) {
    SsoSurface surface;
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "SCIM organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const ApiResult provider = surface.call(
        "register_sso_provider",
        provider_input(
            organization.value.at("id"), "stub-scim", "oidc", "https://scim.stub.example"));
    ASSERT_TRUE(provider.ok) << provider.error.message;
    const std::string provider_id = provider.value.at("id");

    const auto provision_user = [&](const std::string& external_id, const std::string& name) {
        return surface.call("upsert_scim_user",
                            {{"provider_id", provider_id},
                             {"external_id", external_id},
                             {"email", external_id + "@example.com"},
                             {"display_name", name},
                             {"taap_id", "taap:1:ed25519:" + external_id},
                             {"active", true},
                             {"attributes", {{"department", "engineering"}}}});
    };
    ASSERT_TRUE(provision_user("one", "One").ok);
    ASSERT_TRUE(provision_user("two", "Two").ok);
    const ApiResult updated = provision_user("one", "One updated");
    ASSERT_TRUE(updated.ok) << updated.error.message;

    const ApiResult group = surface.call("upsert_scim_group",
                                         {{"provider_id", provider_id},
                                          {"external_id", "engineering"},
                                          {"team_name", "engineering"},
                                          {"display_name", "Engineering"},
                                          {"member_external_ids", json::array({"one", "two"})}});
    ASSERT_TRUE(group.ok) << group.error.message;
    ASSERT_TRUE(surface
                    .call("upsert_scim_group",
                          {{"provider_id", provider_id},
                           {"external_id", "engineering"},
                           {"team_name", "engineering"},
                           {"display_name", "Engineering"},
                           {"member_external_ids", json::array({"two"})}})
                    .ok);

    ASSERT_TRUE(surface
                    .call("create_federation_issuer",
                          {{"organization_id", organization.value.at("id")},
                           {"issuer", "https://scim.stub.example"},
                           {"claim", "email"}})
                    .ok);
    surface.context.authorized = false;
    surface.context.operator_credential = false;
    const ApiResult session = surface.call("sign_in_sso",
                                           {{"provider_id", provider_id},
                                            {"issuer", "https://scim.stub.example"},
                                            {"subject", "two"},
                                            {"claims", {{"email", "two@example.com"}}}});
    ASSERT_TRUE(session.ok) << session.error.message;

    surface.context.authorized = true;
    surface.context.operator_credential = true;
    const ApiResult deactivated = surface.call("upsert_scim_user",
                                               {{"provider_id", provider_id},
                                                {"external_id", "two"},
                                                {"email", "two@example.com"},
                                                {"display_name", "Two"},
                                                {"taap_id", "taap:1:ed25519:two"},
                                                {"active", false}});
    ASSERT_TRUE(deactivated.ok) << deactivated.error.message;
    EXPECT_FALSE(surface.store.context_for(session.value.at("secret")).has_value());

    const ApiResult logs =
        surface.call("list_sso_provisioning_logs", {{"provider_id", provider_id}, {"limit", 1}});
    ASSERT_TRUE(logs.ok) << logs.error.message;
    ASSERT_EQ(logs.value.at("items").size(), 1U);
    EXPECT_EQ(logs.value.at("items").front().at("resource"), "user");
    EXPECT_EQ(logs.value.at("items").front().at("operation"), "deactivate");
}

// lake#870: principal ids are TAAP ids; `admin` belongs to the admin token alone.
TEST(PrincipalIds, OnlyTaapIdentitiesAreValid) {
    using lake::control::valid_principal_id;
    EXPECT_TRUE(valid_principal_id("taap:1:ed25519:abc"));
    EXPECT_FALSE(valid_principal_id("admin"));
    EXPECT_FALSE(valid_principal_id(""));
    EXPECT_FALSE(valid_principal_id("taap:"));
    EXPECT_FALSE(valid_principal_id("agent-service:ask"));
    EXPECT_FALSE(valid_principal_id("share-link:1"));
    EXPECT_FALSE(valid_principal_id("taap:1:\nadmin"));
    EXPECT_FALSE(valid_principal_id("taap:" + std::string(508, 'a')));
    EXPECT_TRUE(valid_principal_id("taap:" + std::string(507, 'a')));
}

TEST(PrincipalIds, NoCreationPathAcceptsTheOperatorIdentity) {
    SsoSurface surface;
    EXPECT_FALSE(surface.store.create_user({"admin", "", "Operator"}));
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Org"}, {"taap_id", "admin"}});
    EXPECT_FALSE(organization.ok);
    EXPECT_EQ(organization.error.code, "invalid_input");

    const ApiResult created =
        surface.call("create_organization", {{"display_name", "JIT organization"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    json provider =
        provider_input(created.value.at("id"), "jit", "oidc", "https://oidc.attacker.example");
    provider["jit_provisioning"] = true;
    const ApiResult registered = surface.call("register_sso_provider", provider);
    ASSERT_TRUE(registered.ok) << registered.error.message;

    const ApiResult scim = surface.call("upsert_scim_user",
                                        {{"provider_id", registered.value.at("id")},
                                         {"external_id", "scim-admin"},
                                         {"display_name", "Operator"},
                                         {"taap_id", "admin"},
                                         {"active", true}});
    EXPECT_FALSE(scim.ok);
    EXPECT_EQ(scim.error.code, "invalid_input");

    const auto jit = surface.store.sign_in_sso({registered.value.at("id"),
                                                "https://oidc.attacker.example",
                                                "jit-admin",
                                                "",
                                                "Operator",
                                                "admin",
                                                "browser",
                                                json::object()});
    ASSERT_FALSE(jit);
    EXPECT_EQ(jit.error().code, "invalid_input");
}

} // namespace
