#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
#include "saml_test_support.hpp"

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

    // lake#562: the capability verifies no assertion, so nobody but the operator may call it. An
    // anonymous caller and a signed-in person are both refused before the store is reached.
    const json assertion{{"provider_id", saml.value.at("id")},
                         {"issuer", "https://saml.stub.example"},
                         {"subject", "saml-user"},
                         {"claims", {{"email", "saml-user@example.com"}}}};
    surface.context.authorized = false;
    surface.context.operator_credential = false;
    surface.context.acting_identity.clear();
    const ApiResult anonymous = surface.call("sign_in_sso", assertion);
    EXPECT_FALSE(anonymous.ok);
    EXPECT_EQ(anonymous.error.code, "unauthenticated");
    surface.context.authorized = true;
    surface.context.acting_identity = "taap:1:ed25519:someone";
    const ApiResult person = surface.call("sign_in_sso", assertion);
    EXPECT_FALSE(person.ok);
    EXPECT_EQ(person.error.code, "unauthorized");
    surface.context.acting_identity = "admin";

    // The operator is still held to the federation issuer policy.
    surface.context.operator_credential = true;
    const ApiResult denied = surface.call("sign_in_sso", assertion);
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

    const ApiResult audit = surface.call("audit", {{"request_id", denied_request}, {"limit", 1}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    EXPECT_TRUE(std::ranges::any_of(audit.value.at("items"), [](const json& event) {
        return event.at("action") == "sign_in_sso" && event.at("outcome") == "denied";
    }));
}

TEST(Sso, SignedSamlResponseIsVerifiedBeforeSignIn) {
    lake::test::saml::Idp idp;
    const std::string issuer = "https://saml.signed.stub.example";
    SsoSurface surface;
    surface.context.public_host = "lake.sso.test";
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Signed SAML organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    json input = provider_input(organization.value.at("id"), "signed-saml", "saml", issuer);
    input["metadata"] =
        "<md:EntityDescriptor xmlns:md=\"urn:oasis:names:tc:SAML:2.0:metadata\" entityID=\"" +
        issuer +
        "\"><md:IDPSSODescriptor protocolSupportEnumeration=\"urn:oasis:names:tc:SAML:2.0:"
        "protocol\"><md:SingleSignOnService "
        "Binding=\"urn:oasis:names:tc:SAML:2.0:bindings:HTTP-Redirect\" "
        "Location=\"https://saml.signed.stub.example/sso\"/></md:IDPSSODescriptor></"
        "md:EntityDescriptor>";
    input["signing_certificate"] = idp.certificate();
    input["jit_provisioning"] = true;
    const ApiResult provider = surface.call("register_sso_provider", input);
    ASSERT_TRUE(provider.ok) << provider.error.message;
    const std::string provider_id = provider.value.at("id");
    ASSERT_TRUE(surface
                    .call("create_federation_issuer",
                          {{"organization_id", organization.value.at("id")},
                           {"issuer", issuer},
                           {"claim", "email"}})
                    .ok);
    surface.context.authorized = false;
    surface.context.operator_credential = false;
    const ApiResult started = surface.call("start_sso_saml", {{"provider_id", provider_id}});
    ASSERT_TRUE(started.ok) << started.error.message;
    // The request id is inside the redirect; Lake's own store has the one outstanding request.
    const auto request = lake::control::saml::new_request_id();
    ASSERT_TRUE(surface.store.begin_saml({provider_id, request, "browser", "", ""}));
    lake::test::saml::ResponseShape shape;
    shape.issuer = issuer;
    shape.destination = lake::control::saml::acs_url("lake.sso.test", provider_id);
    shape.recipient = shape.destination;
    shape.audience = lake::control::saml::sp_entity_id("lake.sso.test", provider_id);
    shape.in_response_to = request;
    shape.issued = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count() -
                   5;
    shape.subject = "saml-signed-user";
    shape.email = "signed@example.com";
    const std::string response = lake::test::saml::base64(
        idp.sign_assertion(lake::test::saml::response_xml(shape), shape.assertion_id));
    std::string tampered = response;
    tampered[tampered.size() / 2] = tampered[tampered.size() / 2] == 'A' ? 'B' : 'A';
    const ApiResult rejected =
        surface.call("sign_in_saml", {{"provider_id", provider_id}, {"saml_response", tampered}});
    const std::string rejected_request = surface.context.request_id;
    EXPECT_FALSE(rejected.ok);
    const ApiResult signed_in = surface.call("sign_in_saml",
                                             {{"provider_id", provider_id},
                                              {"saml_response", response},
                                              {"taap_id", "taap:1:ed25519:saml-signed"}});
    ASSERT_TRUE(signed_in.ok) << signed_in.error.message;
    EXPECT_TRUE(surface.store.context_for(signed_in.value.at("secret")).has_value());
    const ApiResult replayed =
        surface.call("sign_in_saml", {{"provider_id", provider_id}, {"saml_response", response}});
    EXPECT_FALSE(replayed.ok);
    surface.context.authorized = true;
    surface.context.operator_credential = true;
    const ApiResult audit = surface.call("audit", {{"request_id", rejected_request}, {"limit", 1}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    EXPECT_TRUE(std::ranges::any_of(audit.value.at("items"), [](const json& event) {
        return event.at("action") == "sign_in_saml" && event.at("outcome") == "denied";
    }));
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

// lake#872: a grant or membership someone writes names a TAAP identity: never `admin`, never lake's
// own synthetic `share-link:<id>` (whose first grant is a share link's folder), never a bare name.
TEST(PrincipalIds, GrantsAndMembershipsNameOnlyTaapIdentities) {
    SsoSurface surface;
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Grants organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const auto object =
        surface.store.find_policy_object("organization", organization.value.at("id"));
    ASSERT_TRUE(object);
    for (const std::string refused : {"admin",
                                      "share-link:00000000-0000-4000-8000-000000000001",
                                      "reader",
                                      "agent-service:ask"}) {
        const ApiResult grant = surface.call(
            "create_grant",
            {{"principal_taap_id", refused}, {"object_id", object->id}, {"role", "read"}});
        EXPECT_FALSE(grant.ok) << refused;
        EXPECT_EQ(grant.error.code, "invalid_input") << refused;
        const ApiResult member = surface.call("create_membership",
                                              {{"organization_id", organization.value.at("id")},
                                               {"principal_type", "user"},
                                               {"principal_id", "u-1"},
                                               {"principal_taap_id", refused},
                                               {"role", "read"}});
        EXPECT_FALSE(member.ok) << refused;
        EXPECT_EQ(member.error.code, "invalid_input") << refused;
    }
    const ApiResult granted = surface.call("create_grant",
                                           {{"principal_taap_id", "taap:1:ed25519:reader"},
                                            {"object_id", object->id},
                                            {"role", "read"}});
    EXPECT_TRUE(granted.ok) << granted.error.message;
}

// lake#885: a SCIM bearer authenticates only its own provider, among many tenants', and an unknown
// token is refused without trying any provider's hash.
TEST(Sso, ScimBearerFindsItsOwnProviderAmongManyTenants) {
    SsoSurface surface;
    std::vector<std::string> providers;
    for (int index = 0; index < 4; ++index) {
        const ApiResult organization = surface.call(
            "create_organization", {{"display_name", "SCIM tenant " + std::to_string(index)}});
        ASSERT_TRUE(organization.ok) << organization.error.message;
        const ApiResult provider =
            surface.call("register_sso_provider",
                         provider_input(organization.value.at("id"),
                                        "scim-tenant-" + std::to_string(index),
                                        "oidc",
                                        "https://scim-" + std::to_string(index) + ".example"));
        ASSERT_TRUE(provider.ok) << provider.error.message;
        providers.push_back(provider.value.at("id"));
    }
    for (int index = 0; index < 4; ++index) {
        const auto found =
            surface.store.authenticate_scim("token-scim-tenant-" + std::to_string(index));
        ASSERT_TRUE(found) << found.error().message;
        EXPECT_EQ(found->id, providers[static_cast<std::size_t>(index)]);
    }
    const auto unknown = surface.store.authenticate_scim("token-scim-tenant-unknown");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, "unauthorized");
    EXPECT_FALSE(surface.store.authenticate_scim(""));
    const ApiResult listed =
        surface.call("scim_list_users", {{"bearer_token", "token-scim-tenant-unknown"}});
    EXPECT_FALSE(listed.ok);
}

} // namespace
