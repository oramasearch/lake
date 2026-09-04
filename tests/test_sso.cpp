#include <gtest/gtest.h>

#include <algorithm>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"

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

    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id.clear();
        return registry.call(name, input, context);
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

    surface.context.authorized = false;
    surface.context.operator_credential = false;
    const ApiResult denied = surface.call("sign_in_sso",
                                          {{"provider_id", saml.value.at("id")},
                                           {"issuer", "https://saml.stub.example"},
                                           {"subject", "saml-user"},
                                           {"claims", {{"email", "saml-user@example.com"}}}});
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
    const ApiResult audit = surface.call("audit", {{"limit", 1000}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    EXPECT_TRUE(std::ranges::any_of(audit.value.at("items"), [](const json& event) {
        return event.at("action") == "sign_in_sso" && event.at("outcome") == "denied";
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
        surface.call("list_sso_provisioning_logs", {{"provider_id", provider_id}});
    ASSERT_TRUE(logs.ok) << logs.error.message;
    EXPECT_TRUE(std::ranges::any_of(logs.value.at("items"), [](const json& entry) {
        return entry.at("resource") == "user" && entry.at("operation") == "deactivate";
    }));
}

} // namespace
