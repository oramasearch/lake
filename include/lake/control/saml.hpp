#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lake::control::saml {

struct Assertion {
    std::string issuer;
    std::string subject;
    std::string email;
    std::string display_name;
    nlohmann::json claims;
};

std::expected<std::string, std::string> canonicalize_exclusive(std::string_view xml,
                                                               bool omit_signature = false);
std::expected<Assertion, std::string> verify_response(std::string_view response_base64,
                                                      std::string_view certificate_pem,
                                                      std::string_view expected_issuer);

} // namespace lake::control::saml
