#include "lake/api/saml.hpp"

#include <chrono>
#include <initializer_list>

#include "lake/control/saml.hpp"

namespace lake::api {
namespace {

ApiError failure(std::string code, std::string message) {
    return ApiError{.code = std::move(code), .message = std::move(message)};
}

ApiError from_store(const control::StoreError& error) {
    return failure(error.code, error.message);
}

// The attribute a provider's `attribute_mapping` names for `field`, else the first of the usual
// names identity providers send.
std::string attribute_value(const nlohmann::json& attributes,
                            const nlohmann::json& mapping,
                            std::initializer_list<std::string_view> fields,
                            std::initializer_list<std::string_view> defaults) {
    if (mapping.is_object()) {
        for (const std::string_view field : fields) {
            const auto mapped = mapping.find(std::string(field));
            if (mapped != mapping.end() && mapped->is_string()) {
                const auto value = attributes.find(mapped->get<std::string>());
                return value != attributes.end() && value->is_string() ? value->get<std::string>()
                                                                       : std::string();
            }
        }
    }
    for (const std::string_view name : defaults) {
        const auto value = attributes.find(std::string(name));
        if (value != attributes.end() && value->is_string()) {
            return value->get<std::string>();
        }
    }
    return {};
}

} // namespace

std::int64_t saml_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::expected<std::string, ApiError> start_saml_sign_in(control::Store& store,
                                                        std::string_view public_host,
                                                        const std::string& provider_id,
                                                        const std::string& device_name,
                                                        const std::string& taap_id,
                                                        const std::string& return_to) {
    if (public_host.empty()) {
        return std::unexpected(
            failure("store_unavailable", "LAKE_PUBLIC_HOST is required for SAML sign-in"));
    }
    auto provider = store.get_sso_provider(provider_id);
    if (!provider) {
        return std::unexpected(from_store(provider.error()));
    }
    if (provider->protocol != "saml" || !provider->enabled) {
        return std::unexpected(failure("not_found", "SAML provider is unavailable"));
    }
    auto sso_url = control::saml::idp_sso_url(provider->metadata, provider->issuer);
    if (!sso_url) {
        return std::unexpected(failure("invalid_input", sso_url.error()));
    }
    const std::string request_id = control::saml::new_request_id();
    if (request_id.empty()) {
        return std::unexpected(failure("store_error", "could not generate a SAML request id"));
    }
    if (auto begun = store.begin_saml({provider->id,
                                       request_id,
                                       device_name.empty() ? "browser" : device_name,
                                       taap_id,
                                       return_to});
        !begun) {
        return std::unexpected(from_store(begun.error()));
    }
    return control::saml::authn_request_url(*sso_url,
                                            request_id,
                                            control::saml::sp_entity_id(public_host, provider->id),
                                            control::saml::acs_url(public_host, provider->id),
                                            saml_now());
}

std::expected<SamlSignIn, ApiError> accept_saml_response(control::Store& store,
                                                         std::string_view public_host,
                                                         const std::string& provider_id,
                                                         std::string_view response_base64,
                                                         const std::string& fallback_taap_id,
                                                         const std::string& fallback_device_name,
                                                         std::int64_t now) {
    if (public_host.empty()) {
        return std::unexpected(
            failure("store_unavailable", "LAKE_PUBLIC_HOST is required for SAML sign-in"));
    }
    if (response_base64.empty()) {
        return std::unexpected(failure("invalid_input", "the SAMLResponse is missing"));
    }
    auto provider = store.get_sso_provider(provider_id);
    if (!provider) {
        return std::unexpected(from_store(provider.error()));
    }
    if (provider->protocol != "saml" || !provider->enabled) {
        return std::unexpected(failure("invalid_input", "provider is not a SAML provider"));
    }
    const std::string sp_entity_id = control::saml::sp_entity_id(public_host, provider->id);
    const std::string acs_url = control::saml::acs_url(public_host, provider->id);
    auto assertion =
        control::saml::verify_response(response_base64,
                                       {.certificate_pem = provider->signing_certificate,
                                        .idp_entity_id = provider->issuer,
                                        .sp_entity_id = sp_entity_id,
                                        .acs_url = acs_url,
                                        .now = now});
    if (!assertion) {
        return std::unexpected(failure("unauthorized", assertion.error()));
    }
    auto request = store.consume_saml(
        {provider->id, assertion->in_response_to, assertion->id, assertion->expires_at});
    if (!request) {
        return std::unexpected(from_store(request.error()));
    }
    const std::string email =
        attribute_value(assertion->attributes,
                        provider->attribute_mapping,
                        {"email"},
                        {"email",
                         "mail",
                         "http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress",
                         "urn:oid:0.9.2342.19200300.100.1.3"});
    const std::string display_name =
        attribute_value(assertion->attributes,
                        provider->attribute_mapping,
                        {"display_name", "name"},
                        {"name",
                         "displayName",
                         "http://schemas.microsoft.com/identity/claims/displayname",
                         "urn:oid:2.16.840.1.113730.3.1.241"});
    SamlSignIn result;
    result.identity = control::SsoIdentity{
        provider->id,
        assertion->issuer,
        assertion->subject,
        email,
        display_name,
        request->taap_id.empty() ? fallback_taap_id : request->taap_id,
        !request->device_name.empty()   ? request->device_name
        : !fallback_device_name.empty() ? fallback_device_name
                                        : std::string("browser"),
        // The claims the issuer policy checks are the ones Lake uses, as for OIDC.
        email.empty() ? nlohmann::json::object() : nlohmann::json{{"email", email}}};
    result.return_to = request->return_to;
    return result;
}

} // namespace lake::api
