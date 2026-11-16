// SPDX-License-Identifier: MIT

#pragma once

// SAML sign-in for enterprise SSO providers (docs/api/sso.md#saml-sign-in, lake#836): the steps the
// `start_sso_saml` and `sign_in_saml` capabilities and the `/v1/saml/<provider id>/...` routes
// share.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

std::int64_t saml_now();

// Records an outstanding AuthnRequest for an enabled SAML provider and answers the identity
// provider's HTTP-Redirect URL that carries it. `return_to` must already be a same-origin path.
std::expected<std::string, ApiError> start_saml_sign_in(control::Store& store,
                                                        std::string_view public_host,
                                                        const std::string& provider_id,
                                                        const std::string& device_name,
                                                        const std::string& taap_id,
                                                        const std::string& return_to);

struct SamlSignIn {
    control::SsoIdentity identity; // ready for Store::sign_in_sso
    std::string return_to;
};

// Verifies a base64 SAMLResponse for the provider (signature, issuer, audience, recipient,
// destination, validity window), then consumes the request it answers and records the assertion,
// once. The TAAP identity and device name are the request's; the fallbacks apply only where the
// request recorded none.
std::expected<SamlSignIn, ApiError> accept_saml_response(control::Store& store,
                                                         std::string_view public_host,
                                                         const std::string& provider_id,
                                                         std::string_view response_base64,
                                                         const std::string& fallback_taap_id,
                                                         const std::string& fallback_device_name,
                                                         std::int64_t now);

} // namespace lake::api
