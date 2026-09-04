#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace lake::test {

inline std::string base64url(const unsigned char *bytes, std::size_t count) {
  std::string encoded(((count + 2) / 3) * 4, '\0');
  const int size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), bytes,
                      static_cast<int>(count));
  EXPECT_GE(size, 0);
  encoded.resize(static_cast<std::size_t>(size));
  while (encoded.ends_with('=')) {
    encoded.pop_back();
  }
  std::replace(encoded.begin(), encoded.end(), '+', '-');
  std::replace(encoded.begin(), encoded.end(), '/', '_');
  return encoded;
}

inline std::string base64url(std::string_view value) {
  return base64url(reinterpret_cast<const unsigned char *>(value.data()),
                   value.size());
}

inline std::string base64url(const std::vector<unsigned char> &value) {
  return base64url(value.data(), value.size());
}

inline std::vector<unsigned char> sha256(std::string_view value) {
  std::vector<unsigned char> digest(EVP_MD_get_size(EVP_sha256()));
  unsigned int size = 0;
  EXPECT_EQ(EVP_Digest(value.data(), value.size(), digest.data(), &size,
                       EVP_sha256(), nullptr),
            1);
  digest.resize(size);
  return digest;
}

inline void append_count(std::vector<unsigned char> &target,
                         std::uint32_t value) {
  target.push_back(static_cast<unsigned char>(value >> 24));
  target.push_back(static_cast<unsigned char>(value >> 16));
  target.push_back(static_cast<unsigned char>(value >> 8));
  target.push_back(static_cast<unsigned char>(value));
}

struct CeremonyResponse {
  std::string credential_id;
  std::string client_data_json;
  std::string authenticator_data;
  std::string signature;
};

class SoftwareAuthenticator {
public:
  SoftwareAuthenticator() {
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EXPECT_NE(context, nullptr);
    EXPECT_EQ(EVP_PKEY_keygen_init(context), 1);
    EXPECT_EQ(EVP_PKEY_CTX_set_group_name(context, "prime256v1"), 1);
    EXPECT_EQ(EVP_PKEY_generate(context, &key_), 1);
    EVP_PKEY_CTX_free(context);
    EXPECT_NE(key_, nullptr);
    for (std::size_t index = 0; index < credential_.size(); ++index) {
      credential_[index] = static_cast<unsigned char>(index + 1);
    }
  }

  ~SoftwareAuthenticator() { EVP_PKEY_free(key_); }

  SoftwareAuthenticator(const SoftwareAuthenticator &) = delete;
  SoftwareAuthenticator &operator=(const SoftwareAuthenticator &) = delete;

  CeremonyResponse registration(std::string_view challenge,
                                std::string_view rp_id,
                                std::string_view origin) const {
    using nlohmann::json;
    const std::string client = json{
        {"type", "webauthn.create"},
        {"challenge", challenge},
        {"origin", origin},
        {"crossOrigin", false}}.dump();
    std::vector<unsigned char> auth_data = sha256(rp_id);
    auth_data.push_back(0x41);
    append_count(auth_data, 0);
    auth_data.insert(auth_data.end(), 16, 0);
    auth_data.push_back(0);
    auth_data.push_back(static_cast<unsigned char>(credential_.size()));
    auth_data.insert(auth_data.end(), credential_.begin(), credential_.end());
    const std::vector<unsigned char> cose = cose_public_key();
    auth_data.insert(auth_data.end(), cose.begin(), cose.end());
    const std::vector<unsigned char> attestation =
        json::to_cbor(json{{"fmt", "none"},
                           {"attStmt", json::object()},
                           {"authData", json::binary(auth_data)}});
    return {base64url(credential_.data(), credential_.size()),
            base64url(client), base64url(attestation), ""};
  }

  CeremonyResponse assertion(std::string_view challenge, std::string_view rp_id,
                             std::string_view origin,
                             std::uint32_t sign_count) const {
    using nlohmann::json;
    const std::string client = json{
        {"type", "webauthn.get"},
        {"challenge", challenge},
        {"origin", origin},
        {"crossOrigin", false}}.dump();
    std::vector<unsigned char> auth_data = sha256(rp_id);
    auth_data.push_back(0x01);
    append_count(auth_data, sign_count);
    const std::vector<unsigned char> client_hash = sha256(client);
    std::vector<unsigned char> signed_data = auth_data;
    signed_data.insert(signed_data.end(), client_hash.begin(),
                       client_hash.end());
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    EXPECT_NE(context, nullptr);
    EXPECT_EQ(EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key_),
              1);
    std::size_t signature_size = 0;
    EXPECT_EQ(EVP_DigestSign(context, nullptr, &signature_size,
                             signed_data.data(), signed_data.size()),
              1);
    std::vector<unsigned char> signature(signature_size);
    EXPECT_EQ(EVP_DigestSign(context, signature.data(), &signature_size,
                             signed_data.data(), signed_data.size()),
              1);
    EVP_MD_CTX_free(context);
    signature.resize(signature_size);
    return {base64url(credential_.data(), credential_.size()),
            base64url(client), base64url(auth_data), base64url(signature)};
  }

private:
  std::vector<unsigned char> cose_public_key() const {
    std::array<unsigned char, 65> public_key{};
    std::size_t public_key_size = public_key.size();
    EXPECT_EQ(EVP_PKEY_get_octet_string_param(
                  key_, OSSL_PKEY_PARAM_PUB_KEY, public_key.data(),
                  public_key.size(), &public_key_size),
              1);
    EXPECT_EQ(public_key_size, public_key.size());
    EXPECT_EQ(public_key[0], 0x04);
    std::vector<unsigned char> cose{0xa5, 0x01, 0x02, 0x03, 0x26,
                                    0x20, 0x01, 0x21, 0x58, 0x20};
    cose.insert(cose.end(), public_key.begin() + 1, public_key.begin() + 33);
    cose.insert(cose.end(), {0x22, 0x58, 0x20});
    cose.insert(cose.end(), public_key.begin() + 33, public_key.end());
    return cose;
  }

  EVP_PKEY *key_ = nullptr;
  std::array<unsigned char, 32> credential_{};
};

} // namespace lake::test
