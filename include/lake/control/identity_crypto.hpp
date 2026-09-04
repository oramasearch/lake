#pragma once

#include <string>
#include <string_view>

namespace lake::control::identity_crypto {

std::string random_secret();
std::string hash_secret(std::string_view secret);
bool verify_secret(std::string_view encoded, std::string_view secret);
std::string new_totp_secret();
bool verify_totp(std::string_view secret, std::string_view code);

} // namespace lake::control::identity_crypto
