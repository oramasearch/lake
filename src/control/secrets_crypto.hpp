#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lake/control/store.hpp"

namespace lake::control::secrets_crypto {

using Key = std::array<unsigned char, 32>;

struct Ciphertext {
    std::string key_id;
    std::uint64_t version = 0;
    std::string sealed;
};

Result<Key> instance_key(std::string_view encoded);
Result<Key> random_key();
Result<Key> derive(const Key& key, std::string_view context);
Result<Key> derive_hkdf(const Key& key, std::string_view label);
Result<std::string> seal(const Key& key, std::string_view plaintext, std::string_view associated);
Result<std::string> open(const Key& key, std::string_view sealed, std::string_view associated);
Result<std::string> base64url_encode(std::string_view value);
Result<std::string> base64url_decode(std::string_view value);
Result<std::string> sign(const Key& key, std::string_view message);
Result<bool> verify(const Key& key, std::string_view message, std::string_view signature);
Result<std::string> hmac(const Key& key, std::string_view message);
std::string format_ciphertext(const Ciphertext& value);
Result<Ciphertext> parse_ciphertext(std::string_view value);
void cleanse(Key& key);

} // namespace lake::control::secrets_crypto
